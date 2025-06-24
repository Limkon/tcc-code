#define UNICODE
#define _UNICODE

#define _WIN32_WINNT 0x0501 // For Windows XP compatibility

#include <windows.h>    
#include <stdio.h>      // For FILE operations (for config.ini), sscanf, fprintf
#include <stdlib.h>     // For malloc, free
#include <string.h>     // For strlen, strcpy, strstr, strchr, strncpy
#include <wchar.h>      // For wide character string operations, wcscmp, wcsstr etc.
#include <time.h>       // For time-related functions
#include <io.h>         // For _commit (for INI file flush)

// For Resource Hacker icon embedding
#define IDI_APPICON 101 // You would typically define this in a resource.h and include it

// --- Constants and Global Variables ---
// Window Class Names
const WCHAR *MAIN_WINDOW_CLASS = L"ShutdownAssistantMainWindowClass";
const WCHAR *HIDDEN_WINDOW_CLASS = L"ShutdownAssistantHiddenWindowClass";
const WCHAR *MUTEX_NAME = L"Global\\ShutdownAssistantMutex"; // Global mutex name

// Config file name
const WCHAR *CONFIG_FILE_BASE_NAME = L"config.ini";
const WCHAR *CONFIG_SECTION_NAME = L"Settings"; // Section name in INI file

// Paths for config file (global, will store FULL ABSOLUTE path)
WCHAR g_config_file_path[MAX_PATH];

// Timer IDs
#define IDT_TIMER_CHECK_IDLE 2001
#define IDT_TIMER_CHECK_TIMED_SHUTDOWN 2002
#define IDT_TIMER_SHUTDOWN_COUNTDOWN 2003

// GUI Control IDs
#define IDC_CHK_AUTORUN             100
#define IDC_CHK_TIMED_SHUTDOWN      101
#define IDC_EDIT_SHUTDOWN_HOUR      102
#define IDC_EDIT_SHUTDOWN_MINUTE    103
#define IDC_CHK_IDLE_SHUTDOWN       104
#define IDC_EDIT_IDLE_MINUTES       105
#define IDC_EDIT_COUNTDOWN_SECONDS  106
// IDC_CHK_SHOW_WARNING (removed)
#define IDC_CHK_HIDE_MAIN_WINDOW    108 // Hide main window on next launch
#define IDC_BTN_SAVE_SETTINGS       109
#define IDC_BTN_SHUTDOWN_NOW        110
#define IDC_BTN_EXIT_APP            112 // Exit application button
#define IDC_BTN_HIDE_PROGRAM        113 // New: Hide main window button (current launch)


// Global variables
HWND g_hMainWindow = NULL; // Main GUI window handle
HWND g_hHiddenWindow = NULL; // Hidden window handle for timers
HANDLE g_hMutex = NULL; // Global mutex handle for single instance
BOOL g_shutdown_executed_today = FALSE;

// --- Configuration Structure ---
typedef struct {
    BOOL enable_autorun;        // Whether to start automatically with Windows
    BOOL enable_timed_shutdown; // Whether to enable scheduled shutdown
    int shutdown_hour;          // Scheduled shutdown hour (0-23)
    int shutdown_minute;        // Scheduled shutdown minute (0-59)
    BOOL enable_idle_shutdown;  // Whether to enable idle shutdown
    int idle_minutes;           // Minutes of inactivity before shutdown
    int countdown_seconds;      // Seconds to countdown before executing shutdown command
    // BOOL show_shutdown_warning; // Removed
    BOOL hide_main_window;      // Whether to hide main window on next launch
} AppConfig;

AppConfig g_config; // Global configuration instance

// --- Function Prototypes ---
// Window Procedures
LRESULT CALLBACK MainWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK HiddenWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Helper to read boolean from INI file
BOOL GetPrivateProfileBoolW(LPCWSTR lpAppName, LPCWSTR lpKeyName, BOOL fDefault, LPCWSTR lpFileName);
// Helper to write boolean to INI file
BOOL WritePrivateProfileBoolW(LPCWSTR lpAppName, LPCWSTR lpKeyName, BOOL fValue, LPCWSTR lpFileName);
// Helper to write int to INI file (using WritePrivateProfileStringW)
BOOL WritePrivateProfileIntW(LPCWSTR lpAppName, LPCWSTR lpKeyName, int iValue, LPCWSTR lpFileName);


BOOL LoadConfig(const WCHAR* configPath);
BOOL SaveConfig(const WCHAR* configPath);
void ApplyConfigToGUI();
void GetConfigFromGUI();
BOOL IsAutorunEnabled();
void SetAutorun(BOOL enable);
void InitiateShutdown(UINT countdown);
void StopShutdownCountdown();
void SetShutdownTimers();


// Helper to read boolean from INI file
BOOL GetPrivateProfileBoolW(LPCWSTR lpAppName, LPCWSTR lpKeyName, BOOL fDefault, LPCWSTR lpFileName) {
    WCHAR szRet[8]; // "true" or "false" + null terminator
    GetPrivateProfileStringW(lpAppName, lpKeyName, fDefault ? L"true" : L"false", szRet, ARRAYSIZE(szRet), lpFileName);
    return (wcscmp(szRet, L"true") == 0);
}

// Helper to write boolean to INI file
BOOL WritePrivateProfileBoolW(LPCWSTR lpAppName, LPCWSTR lpKeyName, BOOL fValue, LPCWSTR lpFileName) {
    return WritePrivateProfileStringW(lpAppName, lpKeyName, fValue ? L"true" : L"false", lpFileName);
}

// Helper to write int to INI file (using WritePrivateProfileStringW)
BOOL WritePrivateProfileIntW(LPCWSTR lpAppName, LPCWSTR lpKeyName, int iValue, LPCWSTR lpFileName) {
    WCHAR szValue[16];
    swprintf_s(szValue, ARRAYSIZE(szValue), L"%d", iValue);
    return WritePrivateProfileStringW(lpAppName, lpKeyName, szValue, lpFileName);
}


// --- Configuration Read/Write Functions ---
BOOL LoadConfig(const WCHAR* configPath) {
    // Initialize with default values. These will be overwritten by successful reads.
    g_config.enable_autorun = FALSE;
    g_config.enable_timed_shutdown = FALSE;
    g_config.shutdown_hour = 0;
    g_config.shutdown_minute = 0;
    g_config.enable_idle_shutdown = FALSE;
    g_config.idle_minutes = 0;
    g_config.countdown_seconds = 0;
    // g_config.show_shutdown_warning = FALSE; // Removed
    g_config.hide_main_window = FALSE;    // Default: show GUI

    // Check if config file exists using FindFirstFileW
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(configPath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        // config.ini not found. Generate default config.
        FindClose(hFind); // Close handle (even if invalid)
        
        // --- MANUALLY CREATE AND WRITE DEFAULT CONFIG USING _wfopen ---
        FILE* f_create = _wfopen(configPath, L"w,ccs=UTF-16LE");
        if (!f_create) { // If _wfopen fails, it's a critical error for file creation
             MessageBoxW(NULL, L"致命错误：无法创建配置文件！请检查程序权限。\n尝试将程序放在桌面或“文档”等有写入权限的目录。\n错误码: %lu", L"配置文件创建失败", MB_OK | MB_ICONERROR);
             return FALSE; // Return FALSE if creation failed
        }
        
        // Write default INI content
        fwprintf(f_create, L"[%ls]\n", CONFIG_SECTION_NAME);
        fwprintf(f_create, L"EnableAutorun=%ls\n", g_config.enable_autorun ? L"true" : L"false");
        fwprintf(f_create, L"EnableTimedShutdown=%ls\n", g_config.enable_timed_shutdown ? L"true" : L"false");
        fwprintf(f_create, L"ShutdownHour=%d\n", g_config.shutdown_hour);
        fwprintf(f_create, L"ShutdownMinute=%d\n", g_config.shutdown_minute);
        fwprintf(f_create, L"EnableIdleShutdown=%ls\n", g_config.enable_idle_shutdown ? L"true" : L"false");
        fwprintf(f_create, L"IdleMinutes=%d\n", g_config.idle_minutes);
        fwprintf(f_create, L"CountdownSeconds=%d\n", g_config.countdown_seconds);
        // fwprintf(f_create, L"ShowShutdownWarning=%ls\n", g_config.show_shutdown_warning ? L"true" : L"false"); // Removed
        fwprintf(f_create, L"HideMainWindow=%ls\n", g_config.hide_main_window ? L"true" : L"false");
        
        fclose(f_create);
    } else {
        // config.ini found.
    }
    FindClose(hFind); // Ensure handle is closed if file was found

    // Read values from INI file (whether just created or existing)
    g_config.enable_autorun = GetPrivateProfileBoolW(CONFIG_SECTION_NAME, L"EnableAutorun", g_config.enable_autorun, configPath);
    g_config.enable_timed_shutdown = GetPrivateProfileBoolW(CONFIG_SECTION_NAME, L"EnableTimedShutdown", g_config.enable_timed_shutdown, configPath);
    g_config.shutdown_hour = GetPrivateProfileIntW(CONFIG_SECTION_NAME, L"ShutdownHour", g_config.shutdown_hour, configPath);
    g_config.shutdown_minute = GetPrivateProfileIntW(CONFIG_SECTION_NAME, L"ShutdownMinute", g_config.shutdown_minute, configPath);
    g_config.enable_idle_shutdown = GetPrivateProfileBoolW(CONFIG_SECTION_NAME, L"EnableIdleShutdown", g_config.enable_idle_shutdown, configPath);
    g_config.idle_minutes = GetPrivateProfileIntW(CONFIG_SECTION_NAME, L"IdleMinutes", g_config.idle_minutes, configPath);
    g_config.countdown_seconds = GetPrivateProfileIntW(CONFIG_SECTION_NAME, L"CountdownSeconds", g_config.countdown_seconds, configPath);
    // g_config.show_shutdown_warning = GetPrivateProfileBoolW(CONFIG_SECTION_NAME, L"ShowShutdownWarning", g_config.show_shutdown_warning, configPath); // Removed
    g_config.hide_main_window = GetPrivateProfileBoolW(CONFIG_SECTION_NAME, L"HideMainWindow", g_config.hide_main_window, configPath);

    return TRUE;
}

BOOL SaveConfig(const WCHAR* configPath) {
    
    // Write values to INI file
    WritePrivateProfileBoolW(CONFIG_SECTION_NAME, L"EnableAutorun", g_config.enable_autorun, configPath);
    WritePrivateProfileBoolW(CONFIG_SECTION_NAME, L"EnableTimedShutdown", g_config.enable_timed_shutdown, configPath);
    WritePrivateProfileIntW(CONFIG_SECTION_NAME, L"ShutdownHour", g_config.shutdown_hour, configPath);
    WritePrivateProfileIntW(CONFIG_SECTION_NAME, L"ShutdownMinute", g_config.shutdown_minute, configPath);
    WritePrivateProfileBoolW(CONFIG_SECTION_NAME, L"EnableIdleShutdown", g_config.enable_idle_shutdown, configPath);
    WritePrivateProfileIntW(CONFIG_SECTION_NAME, L"IdleMinutes", g_config.idle_minutes, configPath);
    WritePrivateProfileIntW(CONFIG_SECTION_NAME, L"CountdownSeconds", g_config.countdown_seconds, configPath);
    // WritePrivateProfileBoolW(CONFIG_SECTION_NAME, L"ShowShutdownWarning", g_config.show_shutdown_warning, configPath); // Removed
    WritePrivateProfileBoolW(CONFIG_SECTION_NAME, L"HideMainWindow", g_config.hide_main_window, configPath);

    // Force INI file writes to disk immediately
    if (WritePrivateProfileStringW(NULL, NULL, NULL, configPath) == 0) {
        return FALSE;
    }

    return TRUE;
}

// --- Autorun Functions ---
BOOL IsAutorunEnabled() {
    HKEY hKey;
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                     0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR value[MAX_PATH];
        DWORD size = sizeof(value);
        LONG res = RegQueryValueExW(hKey, L"ShutdownTray", NULL, NULL, (LPBYTE)value, &size);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS && wcscmp(value, path) == 0);
    }
    return FALSE;
}

void SetAutorun(BOOL enable) {
    HKEY hKey;
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);

    LONG createRes = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                                     0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (createRes == ERROR_SUCCESS) {
        if (enable) {
            RegSetValueExW(hKey, L"ShutdownTray", 0, REG_SZ, (BYTE*)path,
                           (lstrlenW(path) + 1) * sizeof(WCHAR));
        } else {
            RegDeleteValueW(hKey, L"ShutdownTray");
        }
        RegCloseKey(hKey);
    } else {
    }
}

// --- Shutdown Functions ---
void InitiateShutdown(UINT countdown) {
    SetTimer(g_hHiddenWindow, IDT_TIMER_SHUTDOWN_COUNTDOWN, countdown * 1000, NULL);
}

void StopShutdownCountdown() {
    KillTimer(g_hHiddenWindow, IDT_TIMER_SHUTDOWN_COUNTDOWN);
}

// --- Timer Management Functions ---
void SetShutdownTimers() {
    KillTimer(g_hHiddenWindow, IDT_TIMER_CHECK_IDLE);
    KillTimer(g_hHiddenWindow, IDT_TIMER_CHECK_TIMED_SHUTDOWN);

    if (g_config.enable_idle_shutdown) {
        if (g_config.idle_minutes <= 0) {
            g_config.enable_idle_shutdown = FALSE;
        } else {
            SetTimer(g_hHiddenWindow, IDT_TIMER_CHECK_IDLE, g_config.idle_minutes * 60 * 1000, NULL);
        }
    } else {
    }

    if (g_config.enable_timed_shutdown) {
        if (g_config.shutdown_hour < 0 || g_config.shutdown_hour > 23 ||
            g_config.shutdown_minute < 0 || g_config.shutdown_minute > 59) {
            g_config.enable_timed_shutdown = FALSE;
        } else {
            SetTimer(g_hHiddenWindow, IDT_TIMER_CHECK_TIMED_SHUTDOWN, 60 * 1000, NULL);
        }
    } else {
    }
}

// --- GUI Functions ---
void ApplyConfigToGUI() {
    if (!g_hMainWindow) return;

    SendMessage(GetDlgItem(g_hMainWindow, IDC_CHK_AUTORUN), BM_SETCHECK, g_config.enable_autorun ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(GetDlgItem(g_hMainWindow, IDC_CHK_TIMED_SHUTDOWN), BM_SETCHECK, g_config.enable_timed_shutdown ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(GetDlgItem(g_hMainWindow, IDC_CHK_IDLE_SHUTDOWN), BM_SETCHECK, g_config.enable_idle_shutdown ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(GetDlgItem(g_hMainWindow, IDC_CHK_HIDE_MAIN_WINDOW), BM_SETCHECK, g_config.hide_main_window ? BST_CHECKED : BST_UNCHECKED, 0);

    WCHAR szTime[5];
    swprintf_s(szTime, ARRAYSIZE(szTime), L"%02d", g_config.shutdown_hour);
    SetDlgItemTextW(g_hMainWindow, IDC_EDIT_SHUTDOWN_HOUR, szTime);
    swprintf_s(szTime, ARRAYSIZE(szTime), L"%02d", g_config.shutdown_minute);
    SetDlgItemTextW(g_hMainWindow, IDC_EDIT_SHUTDOWN_MINUTE, szTime);

    swprintf_s(szTime, ARRAYSIZE(szTime), L"%d", g_config.idle_minutes);
    SetDlgItemTextW(g_hMainWindow, IDC_EDIT_IDLE_MINUTES, szTime);

    swprintf_s(szTime, ARRAYSIZE(szTime), L"%d", g_config.countdown_seconds);
    SetDlgItemTextW(g_hMainWindow, IDC_EDIT_COUNTDOWN_SECONDS, szTime);

    InvalidateRect(g_hMainWindow, NULL, TRUE);
    UpdateWindow(g_hMainWindow);
}

void GetConfigFromGUI() {
    g_config.enable_autorun = (SendMessage(GetDlgItem(g_hMainWindow, IDC_CHK_AUTORUN), BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_config.enable_timed_shutdown = (SendMessage(GetDlgItem(g_hMainWindow, IDC_CHK_TIMED_SHUTDOWN), BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_config.enable_idle_shutdown = (SendMessage(GetDlgItem(g_hMainWindow, IDC_CHK_IDLE_SHUTDOWN), BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_config.hide_main_window = (SendMessage(GetDlgItem(g_hMainWindow, IDC_CHK_HIDE_MAIN_WINDOW), BM_GETCHECK, 0, 0) == BST_CHECKED);

    WCHAR szTime[5];
    GetDlgItemTextW(g_hMainWindow, IDC_EDIT_SHUTDOWN_HOUR, szTime, ARRAYSIZE(szTime));
    g_config.shutdown_hour = _wtoi(szTime);
    GetDlgItemTextW(g_hMainWindow, IDC_EDIT_SHUTDOWN_MINUTE, szTime, ARRAYSIZE(szTime));
    g_config.shutdown_minute = _wtoi(szTime);

    GetDlgItemTextW(g_hMainWindow, IDC_EDIT_IDLE_MINUTES, szTime, ARRAYSIZE(szTime));
    g_config.idle_minutes = _wtoi(szTime);

    GetDlgItemTextW(g_hMainWindow, IDC_EDIT_COUNTDOWN_SECONDS, szTime, ARRAYSIZE(szTime));
    g_config.countdown_seconds = _wtoi(szTime);
}

// --- Main GUI Window Procedure ---
LRESULT CALLBACK MainWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hMainWindow = hWnd;
            int yPos = 20;
            const int lineSpacing = 28; // Tighter spacing
            const int labelWidth = 120; // Slightly narrower labels
            const int checkboxColX = 145; // Consistent column for checkboxes/edits
            const int editWidthSmall = 35;
            const int editWidthLarge = 60;
            const int buttonWidth = 90;
            const int buttonHeight = 30;
            const int buttonHorizontalSpacing = 10;


            CreateWindowW(L"STATIC", L"开机启动:", WS_VISIBLE | WS_CHILD, 20, yPos, labelWidth, 20, hWnd, NULL, NULL, NULL);
            CreateWindowW(L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, checkboxColX, yPos, 20, 20, hWnd, (HMENU)IDC_CHK_AUTORUN, NULL, NULL);
            yPos += lineSpacing;

            CreateWindowW(L"STATIC", L"定时关机:", WS_VISIBLE | WS_CHILD, 20, yPos, labelWidth, 20, hWnd, NULL, NULL, NULL);
            CreateWindowW(L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, checkboxColX, yPos, 20, 20, hWnd, (HMENU)IDC_CHK_TIMED_SHUTDOWN, NULL, NULL);
            CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, checkboxColX + 30, yPos, editWidthSmall, 20, hWnd, (HMENU)IDC_EDIT_SHUTDOWN_HOUR, NULL, NULL);
            CreateWindowW(L"STATIC", L":", WS_VISIBLE | WS_CHILD, checkboxColX + 70, yPos, 10, 20, hWnd, NULL, NULL, NULL);
            CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, checkboxColX + 85, yPos, editWidthSmall, 20, hWnd, (HMENU)IDC_EDIT_SHUTDOWN_MINUTE, NULL, NULL);
            yPos += lineSpacing;

            CreateWindowW(L"STATIC", L"空闲关机:", WS_VISIBLE | WS_CHILD, 20, yPos, labelWidth, 20, hWnd, NULL, NULL, NULL);
            CreateWindowW(L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, checkboxColX, yPos, 20, 20, hWnd, (HMENU)IDC_CHK_IDLE_SHUTDOWN, NULL, NULL);
            CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, checkboxColX + 30, yPos, editWidthLarge, 20, hWnd, (HMENU)IDC_EDIT_IDLE_MINUTES, NULL, NULL);
            CreateWindowW(L"STATIC", L"分钟", WS_VISIBLE | WS_CHILD, checkboxColX + 95, yPos, 40, 20, hWnd, NULL, NULL, NULL);
            yPos += lineSpacing;

            CreateWindowW(L"STATIC", L"关机倒计时:", WS_VISIBLE | WS_CHILD, 20, yPos, labelWidth, 20, hWnd, NULL, NULL, NULL);
            CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, checkboxColX, yPos, editWidthLarge, 20, hWnd, (HMENU)IDC_EDIT_COUNTDOWN_SECONDS, NULL, NULL);
            CreateWindowW(L"STATIC", L"秒", WS_VISIBLE | WS_CHILD, checkboxColX + 65, yPos, 40, 20, hWnd, NULL, NULL, NULL);
            yPos += lineSpacing;

            // Removed "显示警告弹窗:" checkbox and "启用日志记录:" checkbox lines
            
            CreateWindowW(L"STATIC", L"下次启动隐藏界面:", WS_VISIBLE | WS_CHILD, 20, yPos, labelWidth + 30, 20, hWnd, NULL, NULL, NULL);
            CreateWindowW(L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, checkboxColX + 30, yPos, 20, 20, hWnd, (HMENU)IDC_CHK_HIDE_MAIN_WINDOW, NULL, NULL);
            yPos += lineSpacing + 15; // Extra spacing before buttons

            // Buttons layout
            int btnX = 20;
            CreateWindowW(L"BUTTON", L"保存设置", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnX, yPos, buttonWidth, buttonHeight, hWnd, (HMENU)IDC_BTN_SAVE_SETTINGS, NULL, NULL);
            btnX += buttonWidth + buttonHorizontalSpacing;
            CreateWindowW(L"BUTTON", L"立即关机", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnX, yPos, buttonWidth, buttonHeight, hWnd, (HMENU)IDC_BTN_SHUTDOWN_NOW, NULL, NULL);
            btnX += buttonWidth + buttonHorizontalSpacing;
            CreateWindowW(L"BUTTON", L"隐藏程序", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnX, yPos, buttonWidth, buttonHeight, hWnd, (HMENU)IDC_BTN_HIDE_PROGRAM, NULL, NULL); // New Hide button
            btnX += buttonWidth + buttonHorizontalSpacing;
            CreateWindowW(L"BUTTON", L"退出程序", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnX, yPos, buttonWidth, buttonHeight, hWnd, (HMENU)IDC_BTN_EXIT_APP, NULL, NULL);

            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            switch (wmId) {
                case IDC_BTN_SAVE_SETTINGS:
                    GetConfigFromGUI();
                    SaveConfig(g_config_file_path);
                    SetAutorun(g_config.enable_autorun);
                    SetShutdownTimers();
                    MessageBoxW(hWnd, L"设置已保存并应用！", L"提示", MB_OK | MB_ICONINFORMATION);
                    break;
                case IDC_BTN_SHUTDOWN_NOW:
                    // Stop any existing countdown before initiating a new one
                    StopShutdownCountdown(); 
                    InitiateShutdown(g_config.countdown_seconds);
                    break;
                case IDC_BTN_HIDE_PROGRAM: // Handle new hide button
                    ShowWindow(hWnd, SW_HIDE);
                    break;
                case IDC_BTN_EXIT_APP:
                    // Destroy the hidden window, which will post WM_QUIT
                    DestroyWindow(g_hHiddenWindow);
                    break;
            }
            break;
        }

        case WM_CLOSE:
            // When user clicks 'X' button, hide the window instead of destroying it
            ShowWindow(hWnd, SW_HIDE);
            break;

        case WM_DESTROY:
            // This message is handled by PostQuitMessage in HiddenWindowProc when DestroyWindow is called
            break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// --- Hidden Window Procedure (for Timers) ---
LRESULT CALLBACK HiddenWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g_hHiddenWindow = hWnd;
            break;

        case WM_TIMER:
            if (LOWORD(wParam) == IDT_TIMER_CHECK_IDLE) {
                // To trigger idle shutdown only once, kill this timer
                KillTimer(hWnd, IDT_TIMER_CHECK_IDLE); 
                InitiateShutdown(g_config.countdown_seconds);

            } else if (LOWORD(wParam) == IDT_TIMER_CHECK_TIMED_SHUTDOWN) {
                SYSTEMTIME st;
                GetLocalTime(&st);

                // Reset shutdown_executed_today at midnight
                if (st.wHour == 0 && st.wMinute == 0 && g_shutdown_executed_today) {
                    g_shutdown_executed_today = FALSE;
                }

                if (g_config.enable_timed_shutdown && !g_shutdown_executed_today) {
                    int currentTimeInMinutes = st.wHour * 60 + st.wMinute;
                    int scheduledTimeInMinutes = g_config.shutdown_hour * 60 + g_config.shutdown_minute;

                    if (currentTimeInMinutes >= scheduledTimeInMinutes) {
                        InitiateShutdown(g_config.countdown_seconds);
                        g_shutdown_executed_today = TRUE;
                    } else {
                    }
                }
            } else if (LOWORD(wParam) == IDT_TIMER_SHUTDOWN_COUNTDOWN) {
                KillTimer(hWnd, IDT_TIMER_SHUTDOWN_COUNTDOWN); // Stop countdown timer

                STARTUPINFOW si = { sizeof(si) };
                PROCESS_INFORMATION pi = {0};
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE; // Hide the command prompt window

                WCHAR cmdLine[] = L"shutdown.exe -s -t 0"; // Shutdown command
                
                if (CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE,
                                    CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                } else {
                    // Critical error: Failed to execute shutdown.exe
                    // In a production app, you might log this or show a critical message
                }
                PostQuitMessage(0); // Exit the application after initiating shutdown
            }
            break;

        case WM_ENDSESSION:
            // Handle system shutdown/logoff by cleaning up timers
            if (wParam == TRUE) {
                KillTimer(hWnd, IDT_TIMER_CHECK_IDLE);
                KillTimer(hWnd, IDT_TIMER_CHECK_TIMED_SHUTDOWN);
                KillTimer(hWnd, IDT_TIMER_SHUTDOWN_COUNTDOWN);
            }
            return 0; // Return 0 to allow session to end
        case WM_QUERYENDSESSION:
            return TRUE; // Allow system to shutdown/logoff
        case WM_DESTROY:
            PostQuitMessage(0); // Post quit message to end the message loop
            break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}


// --- Program Entry Point ---
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    // --- Set Current Working Directory (This remains the executable's directory) ---
    WCHAR exeDir[MAX_PATH];
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    WCHAR* p = wcsrchr(exeDir, L'\\');
    if (p != NULL) {
        *p = L'\0'; // Null-terminate to get only the directory path
        if (!SetCurrentDirectoryW(exeDir)) {
            MessageBoxW(NULL, L"致命错误：无法设置程序工作目录！请检查程序权限或放置位置。", L"程序启动失败", MB_OK | MB_ICONERROR);
            return 1;
        }
    } else {
        MessageBoxW(NULL, L"致命错误：无法确定程序所在目录！请将程序放置在有效位置。", L"程序启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }

    // --- Determine config file path (now absolute path in program directory) ---
    swprintf_s(g_config_file_path, ARRAYSIZE(g_config_file_path), L"%s\\%s", exeDir, CONFIG_FILE_BASE_NAME);

    // --- Initial file write permission test (for config.ini) ---
    FILE* testConfigFile = _wfopen(g_config_file_path, L"a"); // "a" mode to append (creates if not exists)
    if (!testConfigFile) {
        MessageBoxW(NULL,
                     L"致命错误：程序所在目录无写入权限！\n\n"
                     L"请将本程序 (ShutdownTray.exe) 移动到桌面、文档、下载等您拥有完全写入权限的目录，然后再次运行。\n"
                     L"错误码: %lu",
                     L"权限不足，无法启动", MB_OK | MB_ICONERROR);
        return 1;
    }
    fclose(testConfigFile); // Close the test file immediately

    // --- Mutex to ensure single instance ---
    g_hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing_hwnd = FindWindowW(MAIN_WINDOW_CLASS, NULL);
        if (existing_hwnd) {
            ShowWindow(existing_hwnd, SW_RESTORE);
            SetWindowPos(existing_hwnd, HWND_TOPMOST, 0,0,0,0, SWP_NOMOVE | SWP_NOSIZE);
            SetWindowPos(existing_hwnd, HWND_NOTOPMOST, 0,0,0,0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(existing_hwnd);
        } else {
            // No message for duplicate instance if main window is not found.
        }
        if (g_hMutex) CloseHandle(g_hMutex);
        return 0;
    }
    
    // --- Load Configuration (will generate if not found) ---
    if (!LoadConfig(g_config_file_path)) {
        // Fatal error message box already shown in LoadConfig
        if (g_hMutex) CloseHandle(g_hMutex);
        return 1;
    }
    
    // --- Set Autorun after config is loaded ---
    SetAutorun(g_config.enable_autorun);

    // Initialize g_shutdown_executed_today flag
    SYSTEMTIME st_init;
    GetLocalTime(&st_init);
    int currentTimeInMinutes_init = st_init.wHour * 60 + st_init.wMinute;
    int scheduledTimeInMinutes_init = g_config.shutdown_hour * 60 + g_config.shutdown_minute;
    
    if (g_config.enable_timed_shutdown && currentTimeInMinutes_init >= scheduledTimeInMinutes_init) {
        g_shutdown_executed_today = TRUE;
    } else {
        g_shutdown_executed_today = FALSE;
    }

    // --- Register Window Classes ---
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = MainWindowProc; // Main window procedure
    wc.hInstance = hInstance;
    wc.lpszClassName = MAIN_WINDOW_CLASS;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_APPICON)); // Load icon from resources (for Resource Hacker)
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); // A standard gray background
    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"致命错误：无法注册主窗口类！程序将无法运行。", L"程序启动失败", MB_OK | MB_ICONERROR);
        if (g_hMutex) CloseHandle(g_hMutex); return 1;
    }

    wc.lpfnWndProc = HiddenWindowProc; // Hidden window procedure
    wc.lpszClassName = HIDDEN_WINDOW_CLASS;
    wc.hIcon = NULL; // Hidden window needs no icon
    wc.hbrBackground = NULL; // No background for hidden window
    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"致命错误：无法注册隐藏窗口类！程序将无法接收定时器消息。", L"程序启动失败", MB_OK | MB_ICONERROR);
        if (g_hMutex) CloseHandle(g_hMutex); return 1;
    }

    // --- Create Hidden Window (must exist for timers) ---
    g_hHiddenWindow = CreateWindowExW(0, HIDDEN_WINDOW_CLASS, L"ShutdownAssistantHiddenWindow", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!g_hHiddenWindow) {
        MessageBoxW(NULL, L"致命错误：无法创建隐藏窗口！程序将无法运行。", L"程序启动失败", MB_OK | MB_ICONERROR);
        if (g_hMutex) CloseHandle(g_hMutex); return 1;
    }

    // --- Decide whether to show main window or hide it ---
    int initialCmdShow;
    if (g_config.hide_main_window) {
        initialCmdShow = SW_HIDE; // Start hidden
    } else {
        initialCmdShow = SW_SHOW; // Show main window
    }

    // --- Create Main GUI Window ---
    g_hMainWindow = CreateWindowExW(
        0, MAIN_WINDOW_CLASS, L"定时空闲关机助手",
        WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, // Standard window styles
        CW_USEDEFAULT, CW_USEDEFAULT, 430, 250, // Adjusted Width and Height for better, more compact layout
        NULL, NULL, hInstance, NULL);

    if (!g_hMainWindow) {
        MessageBoxW(NULL, L"致命错误：无法创建主界面窗口！程序将无法运行。", L"程序启动失败", MB_OK | MB_ICONERROR);
        DestroyWindow(g_hHiddenWindow); // Clean up hidden window too
        if (g_hMutex) CloseHandle(g_hMutex); return 1;
    }
    
    // --- Apply initial config to GUI controls ---
    ApplyConfigToGUI();

    ShowWindow(g_hMainWindow, initialCmdShow); // Show or hide based on config
    UpdateWindow(g_hMainWindow);

    // --- Set up shutdown timers based on loaded config ---
    SetShutdownTimers();

    // --- Message Loop ---
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // --- Cleanup on Exit ---
    if (g_hMutex) CloseHandle(g_hMutex);
    UnregisterClassW(MAIN_WINDOW_CLASS, hInstance);
    UnregisterClassW(HIDDEN_WINDOW_CLASS, hInstance);
    return (int)msg.wParam;
}
