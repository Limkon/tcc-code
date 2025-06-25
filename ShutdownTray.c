#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0501 // For Windows XP compatibility

#include <windows.h>        
#include <stdio.h>          // For FILE operations (for config.ini), sscanf, fprintf
#include <stdlib.h>         // For malloc, free
#include <string.h>         // For strlen, strcpy, strstr, strchr, strncpy
#include <wchar.h>          // For wide character string operations, wcscmp, wcsstr etc.
#include <time.h>           // For time-related functions
#include <io.h>             // For _commit (for INI file flush)

// For Resource Hacker icon embedding
#define IDI_APPICON 101

// --- Constants and Global Variables ---
const WCHAR *MAIN_WINDOW_CLASS = L"ShutdownAssistantMainWindowClass";
const WCHAR *HIDDEN_WINDOW_CLASS = L"ShutdownAssistantHiddenWindowClass";
const WCHAR *MUTEX_NAME = L"Global\\ShutdownAssistantMutex";
const WCHAR *CONFIG_FILE_BASE_NAME = L"config.ini";
const WCHAR *CONFIG_SECTION_NAME = L"Settings";

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
#define IDC_CHK_HIDE_MAIN_WINDOW    108
#define IDC_BTN_SAVE_SETTINGS       109
#define IDC_BTN_SHUTDOWN_NOW        110
#define IDC_BTN_EXIT_APP            112
#define IDC_BTN_HIDE_PROGRAM        113

// Global Variables
HWND g_hMainWindow = NULL;
HWND g_hHiddenWindow = NULL;
HANDLE g_hMutex = NULL;
BOOL g_shutdown_executed_today = FALSE;
BOOL g_is_shutdown_pending = FALSE;
static WORD s_last_handled_day_for_timed_shutdown = 0;

// GetLastInputInfo Function Pointer
typedef BOOL (WINAPI *PFN_GetLastInputInfo)(LPLASTINPUTINFO);
PFN_GetLastInputInfo pfnGetLastInputInfo = NULL;
HMODULE hUser32 = NULL;

// Configuration Structure
typedef struct {
    BOOL enable_autorun;
    BOOL enable_timed_shutdown;
    int shutdown_hour;
    int shutdown_minute;
    BOOL enable_idle_shutdown;
    int idle_minutes;
    int countdown_seconds;
    BOOL hide_main_window;
} AppConfig;

AppConfig g_config;

// --- Function Prototypes ---
LRESULT CALLBACK MainWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK HiddenWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
BOOL GetPrivateProfileBoolW(LPCWSTR lpAppName, LPCWSTR lpKeyName, BOOL fDefault, LPCWSTR lpFileName);
BOOL WritePrivateProfileBoolW(LPCWSTR lpAppName, LPCWSTR lpKeyName, BOOL fValue, LPCWSTR lpFileName);
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
DWORD GetIdleTime();
void InitializeGetLastInputInfo();
void CleanupGetLastInputInfo();

// --- Helper Functions ---
BOOL GetPrivateProfileBoolW(LPCWSTR lpAppName, LPCWSTR lpKeyName, BOOL fDefault, LPCWSTR lpFileName) {
    WCHAR szRet[8];
    GetPrivateProfileStringW(lpAppName, lpKeyName, fDefault ? L"true" : L"false", szRet, ARRAYSIZE(szRet), lpFileName);
    return (wcscmp(szRet, L"true") == 0);
}

BOOL WritePrivateProfileBoolW(LPCWSTR lpAppName, LPCWSTR lpKeyName, BOOL fValue, LPCWSTR lpFileName) {
    return WritePrivateProfileStringW(lpAppName, lpKeyName, fValue ? L"true" : L"false", lpFileName);
}

BOOL WritePrivateProfileIntW(LPCWSTR lpAppName, LPCWSTR lpKeyName, int iValue, LPCWSTR lpFileName) {
    WCHAR szValue[16];
    swprintf_s(szValue, ARRAYSIZE(szValue), L"%d", iValue);
    return WritePrivateProfileStringW(lpAppName, lpKeyName, szValue, lpFileName);
}

void InitializeGetLastInputInfo() {
    hUser32 = LoadLibraryW(L"User32.dll");
    if (hUser32) {
        pfnGetLastInputInfo = (PFN_GetLastInputInfo)GetProcAddress(hUser32, "GetLastInputInfo");
        if (!pfnGetLastInputInfo) {
            FreeLibrary(hUser32);
            hUser32 = NULL;
        }
    }
}

void CleanupGetLastInputInfo() {
    if (hUser32) {
        FreeLibrary(hUser32);
        hUser32 = NULL;
        pfnGetLastInputInfo = NULL;
    }
}

// --- Configuration Functions ---
BOOL LoadConfig(const WCHAR* configPath) {
    g_config.enable_autorun = FALSE;
    g_config.enable_timed_shutdown = FALSE;
    g_config.shutdown_hour = 0;
    g_config.shutdown_minute = 0;
    g_config.enable_idle_shutdown = FALSE;
    g_config.idle_minutes = 0;
    g_config.countdown_seconds = 30;
    g_config.hide_main_window = FALSE;

    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(configPath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        FILE* f_create = _wfopen(configPath, L"w,ccs=UTF-16LE");
        if (!f_create) {
            MessageBoxW(NULL, L"致命错误：无法创建配置文件！请检查程序权限。\n尝试将程序放在桌面或“文档”等有写入权限的目录。\n错误码: %lu", L"配置文件创建失败", MB_OK | MB_ICONERROR);
            return FALSE;
        }
        fwprintf(f_create, L"[%ls]\n", CONFIG_SECTION_NAME);
        fwprintf(f_create, L"EnableAutorun=%ls\n", g_config.enable_autorun ? L"true" : L"false");
        fwprintf(f_create, L"EnableTimedShutdown=%ls\n", g_config.enable_timed_shutdown ? L"true" : L"false");
        fwprintf(f_create, L"ShutdownHour=%d\n", g_config.shutdown_hour);
        fwprintf(f_create, L"ShutdownMinute=%d\n", g_config.shutdown_minute);
        fwprintf(f_create, L"EnableIdleShutdown=%ls\n", g_config.enable_idle_shutdown ? L"true" : L"false");
        fwprintf(f_create, L"IdleMinutes=%d\n", g_config.idle_minutes);
        fwprintf(f_create, L"CountdownSeconds=%d\n", g_config.countdown_seconds);
        fwprintf(f_create, L"HideMainWindow=%ls\n", g_config.hide_main_window ? L"true" : L"false");
        fclose(f_create);
    }
    FindClose(hFind);

    g_config.enable_autorun = GetPrivateProfileBoolW(CONFIG_SECTION_NAME, L"EnableAutorun", g_config.enable_autorun, configPath);
    g_config.enable_timed_shutdown = GetPrivateProfileBoolW(CONFIG_SECTION_NAME, L"EnableTimedShutdown", g_config.enable_timed_shutdown, configPath);
    g_config.shutdown_hour = GetPrivateProfileIntW(CONFIG_SECTION_NAME, L"ShutdownHour", g_config.shutdown_hour, configPath);
    g_config.shutdown_minute = GetPrivateProfileIntW(CONFIG_SECTION_NAME, L"ShutdownMinute", g_config.shutdown_minute, configPath);
    g_config.enable_idle_shutdown = GetPrivateProfileBoolW(CONFIG_SECTION_NAME, L"EnableIdleShutdown", g_config.enable_idle_shutdown, configPath);
    g_config.idle_minutes = GetPrivateProfileIntW(CONFIG_SECTION_NAME, L"IdleMinutes", g_config.idle_minutes, configPath);
    g_config.countdown_seconds = GetPrivateProfileIntW(CONFIG_SECTION_NAME, L"CountdownSeconds", g_config.countdown_seconds, configPath);
    g_config.hide_main_window = GetPrivateProfileBoolW(CONFIG_SECTION_NAME, L"HideMainWindow", g_config.hide_main_window, configPath);

    return TRUE;
}

BOOL SaveConfig(const WCHAR* configPath) {
    WritePrivateProfileBoolW(CONFIG_SECTION_NAME, L"EnableAutorun", g_config.enable_autorun, configPath);
    WritePrivateProfileBoolW(CONFIG_SECTION_NAME, L"EnableTimedShutdown", g_config.enable_timed_shutdown, configPath);
    WritePrivateProfileIntW(CONFIG_SECTION_NAME, L"ShutdownHour", g_config.shutdown_hour, configPath);
    WritePrivateProfileIntW(CONFIG_SECTION_NAME, L"ShutdownMinute", g_config.shutdown_minute, configPath);
    WritePrivateProfileBoolW(CONFIG_SECTION_NAME, L"EnableIdleShutdown", g_config.enable_idle_shutdown, configPath);
    WritePrivateProfileIntW(CONFIG_SECTION_NAME, L"IdleMinutes", g_config.idle_minutes, configPath);
    WritePrivateProfileIntW(CONFIG_SECTION_NAME, L"CountdownSeconds", g_config.countdown_seconds, configPath);
    WritePrivateProfileBoolW(CONFIG_SECTION_NAME, L"HideMainWindow", g_config.hide_main_window, configPath);

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
    }
}

// --- Shutdown Functions ---
void InitiateShutdown(UINT countdown) {
    if (g_is_shutdown_pending) {
        return;
    }
    g_is_shutdown_pending = TRUE;
    SetTimer(g_hHiddenWindow, IDT_TIMER_SHUTDOWN_COUNTDOWN, countdown * 1000, NULL);
    WCHAR szMessage[256];
    swprintf_s(szMessage, ARRAYSIZE(szMessage), L"系统将在 %d 秒后关机。请保存您的工作。", countdown);
    MessageBoxW(g_hMainWindow, szMessage, L"关机提示", MB_OK | MB_ICONWARNING);
}

void StopShutdownCountdown() {
    KillTimer(g_hHiddenWindow, IDT_TIMER_SHUTDOWN_COUNTDOWN);
    g_is_shutdown_pending = FALSE;
}

DWORD GetIdleTime() {
    if (pfnGetLastInputInfo) {
        LASTINPUTINFO lii = { sizeof(LASTINPUTINFO) };
        if (pfnGetLastInputInfo(&lii)) {
            return (GetTickCount() - lii.dwTime);
        }
    }
    return 0;
}

// --- Timer Management ---
void SetShutdownTimers() {
    KillTimer(g_hHiddenWindow, IDT_TIMER_CHECK_IDLE);
    KillTimer(g_hHiddenWindow, IDT_TIMER_CHECK_TIMED_SHUTDOWN);
    StopShutdownCountdown();

    if (g_config.enable_idle_shutdown) {
        if (g_config.idle_minutes <= 0) {
            g_config.enable_idle_shutdown = FALSE;
            MessageBoxW(NULL, L"闲置时间无效，已禁用闲置关机！", L"错误", MB_OK | MB_ICONERROR);
        } else {
            SetTimer(g_hHiddenWindow, IDT_TIMER_CHECK_IDLE, 30 * 1000, NULL);
        }
    }

    if (g_config.enable_timed_shutdown) {
        if (g_config.shutdown_hour < 0 || g_config.shutdown_hour > 23 ||
            g_config.shutdown_minute < 0 || g_config.shutdown_minute > 59) {
            g_config.enable_timed_shutdown = FALSE;
            MessageBoxW(NULL, L"定时关机时间无效，已禁用定时关机！", L"错误", MB_OK | MB_ICONERROR);
        } else {
            // 定时检查的间隔数修改为60秒 (Already 60 * 1000 ms)
            SetTimer(g_hHiddenWindow, IDT_TIMER_CHECK_TIMED_SHUTDOWN, 60 * 1000, NULL);
        }
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

// --- Main Window Procedure ---
LRESULT CALLBACK MainWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hMainWindow = hWnd;
            int yPos = 20;
            const int lineSpacing = 28;
            const int labelWidth = 120;
            const int checkboxColX = 145;
            const int editWidthSmall = 35;
            const int editWidthLarge = 60;
            const int buttonWidth = 75;
            const int buttonHeight = 25;
            const int buttonHorizontalSpacing = 8;

            CreateWindowW(L"STATIC", L"开机启动:", WS_VISIBLE | WS_CHILD, 20, yPos, labelWidth, 20, hWnd, NULL, NULL, NULL);
            CreateWindowW(L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, checkboxColX, yPos, 20, 20, hWnd, (HMENU)IDC_CHK_AUTORUN, NULL, NULL);
            yPos += lineSpacing;

            CreateWindowW(L"STATIC", L"定时关机:", WS_VISIBLE | WS_CHILD, 20, yPos, labelWidth, 20, hWnd, NULL, NULL, NULL);
            CreateWindowW(L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, checkboxColX, yPos, 20, 20, hWnd, (HMENU)IDC_CHK_TIMED_SHUTDOWN, NULL, NULL);
            CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, checkboxColX + 30, yPos, editWidthSmall, 20, hWnd, (HMENU)IDC_EDIT_SHUTDOWN_HOUR, NULL, NULL);
            CreateWindowW(L"STATIC", L":", WS_VISIBLE | WS_CHILD, checkboxColX + 70, yPos, 10, 20, hWnd, NULL, NULL, NULL);
            CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, checkboxColX + 85, yPos, editWidthSmall, 20, hWnd, (HMENU)IDC_EDIT_SHUTDOWN_MINUTE, NULL, NULL);
            yPos += lineSpacing;

            CreateWindowW(L"STATIC", L"闲置关机:", WS_VISIBLE | WS_CHILD, 20, yPos, labelWidth, 20, hWnd, NULL, NULL, NULL);
            CreateWindowW(L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, checkboxColX, yPos, 20, 20, hWnd, (HMENU)IDC_CHK_IDLE_SHUTDOWN, NULL, NULL);
            CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, checkboxColX + 30, yPos, editWidthLarge, 20, hWnd, (HMENU)IDC_EDIT_IDLE_MINUTES, NULL, NULL);
            CreateWindowW(L"STATIC", L"分钟", WS_VISIBLE | WS_CHILD, checkboxColX + 95, yPos, 40, 20, hWnd, NULL, NULL, NULL);
            yPos += lineSpacing;

            CreateWindowW(L"STATIC", L"关机倒计时:", WS_VISIBLE | WS_CHILD, 20, yPos, labelWidth, 20, hWnd, NULL, NULL, NULL);
            CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, checkboxColX, yPos, editWidthLarge, 20, hWnd, (HMENU)IDC_EDIT_COUNTDOWN_SECONDS, NULL, NULL);
            CreateWindowW(L"STATIC", L"秒", WS_VISIBLE | WS_CHILD, checkboxColX + 65, yPos, 40, 20, hWnd, NULL, NULL, NULL);
            yPos += lineSpacing;

            CreateWindowW(L"STATIC", L"下次启动隐藏界面:", WS_VISIBLE | WS_CHILD, 20, yPos, labelWidth + 30, 20, hWnd, NULL, NULL, NULL);
            CreateWindowW(L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, checkboxColX + 30, yPos, 20, 20, hWnd, (HMENU)IDC_CHK_HIDE_MAIN_WINDOW, NULL, NULL);
            yPos += lineSpacing + 15;

            RECT clientRect;
            GetClientRect(hWnd, &clientRect);
            int windowWidth = clientRect.right - clientRect.left;
            int totalButtonsWidth = (buttonWidth * 4) + (buttonHorizontalSpacing * 3);
            int btnX = (windowWidth - totalButtonsWidth) / 2;

            CreateWindowW(L"BUTTON", L"保存设置", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnX, yPos, buttonWidth, buttonHeight, hWnd, (HMENU)IDC_BTN_SAVE_SETTINGS, NULL, NULL);
            btnX += buttonWidth + buttonHorizontalSpacing;
            CreateWindowW(L"BUTTON", L"立即关机", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnX, yPos, buttonWidth, buttonHeight, hWnd, (HMENU)IDC_BTN_SHUTDOWN_NOW, NULL, NULL);
            btnX += buttonWidth + buttonHorizontalSpacing;
            CreateWindowW(L"BUTTON", L"隐藏程序", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnX, yPos, buttonWidth, buttonHeight, hWnd, (HMENU)IDC_BTN_HIDE_PROGRAM, NULL, NULL);
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

                    // --- 保存设置时立即检查定时关机条件 ---
                    BOOL immediate_shutdown_triggered = FALSE;
                    if (g_config.enable_timed_shutdown && !g_is_shutdown_pending) {
                        SYSTEMTIME st_now;
                        GetLocalTime(&st_now);

                        // 检查日期是否改变，如果改变则重置今日关机标志
                        if (s_last_handled_day_for_timed_shutdown == 0) {
                            s_last_handled_day_for_timed_shutdown = st_now.wDay;
                        }
                        if (st_now.wDay != s_last_handled_day_for_timed_shutdown) {
                            g_shutdown_executed_today = FALSE;
                            s_last_handled_day_for_timed_shutdown = st_now.wDay;
                        }

                        if (!g_shutdown_executed_today) {
                            long current_total_seconds = st_now.wHour * 3600 + st_now.wMinute * 60 + st_now.wSecond;
                            long scheduled_total_seconds = g_config.shutdown_hour * 3600 + g_config.shutdown_minute * 60;
                            long time_diff_seconds = current_total_seconds - scheduled_total_seconds;

                            // 检查当前时间与排程时间的差是否在 [0, 60] 秒之间，并且没有关机在进行中
                            if (time_diff_seconds >= 0 && time_diff_seconds <= 60) {
                                InitiateShutdown(g_config.countdown_seconds);
                                g_shutdown_executed_today = TRUE; // 标记今日已执行关机
                                immediate_shutdown_triggered = TRUE;
                            }
                        }
                    }
                    // --- 结束保存设置时立即检查定时关机条件 ---

                    if (!immediate_shutdown_triggered) {
                        // 如果没有立即触发关机，则设置常规定时器并显示保存成功的消息
                        SetShutdownTimers();
                        MessageBoxW(hWnd, L"设置已保存并应用！", L"提示", MB_OK | MB_ICONINFORMATION);
                    } else {
                        // 如果已触发关机，InitiateShutdown 会显示提示框，此处无需再显示。
                        // 程序很快会退出。
                    }
                    break;
                case IDC_BTN_SHUTDOWN_NOW:
                    InitiateShutdown(g_config.countdown_seconds);
                    break;
                case IDC_BTN_HIDE_PROGRAM:
                    ShowWindow(hWnd, SW_HIDE);
                    break;
                case IDC_BTN_EXIT_APP:
                    DestroyWindow(g_hHiddenWindow);
                    break;
            }
            break;
        }

        case WM_CLOSE:
            ShowWindow(hWnd, SW_HIDE);
            break;

        case WM_DESTROY:
            break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// --- Hidden Window Procedure ---
LRESULT CALLBACK HiddenWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g_hHiddenWindow = hWnd;
            break;

        case WM_TIMER:
            if (LOWORD(wParam) == IDT_TIMER_CHECK_IDLE) {
                if (g_config.enable_idle_shutdown && !g_is_shutdown_pending) {
                    DWORD idle_time_ms = GetIdleTime();
                    DWORD configured_idle_time_ms = (DWORD)g_config.idle_minutes * 60 * 1000;
                    if (configured_idle_time_ms > 0 && idle_time_ms >= configured_idle_time_ms) {
                        InitiateShutdown(g_config.countdown_seconds);
                    }
                }
            } else if (LOWORD(wParam) == IDT_TIMER_CHECK_TIMED_SHUTDOWN) {
                SYSTEMTIME st;
                GetLocalTime(&st);

                // 检查日期是否改变，如果改变则重置今日关机标志
                if (s_last_handled_day_for_timed_shutdown == 0) {
                    s_last_handled_day_for_timed_shutdown = st.wDay;
                }
                if (st.wDay != s_last_handled_day_for_timed_shutdown) {
                    g_shutdown_executed_today = FALSE;
                    s_last_handled_day_for_timed_shutdown = st.wDay;
                }

                // 检查是否需要定时关机
                if (g_config.enable_timed_shutdown && !g_shutdown_executed_today && !g_is_shutdown_pending) {
                    long current_total_seconds = st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
                    long scheduled_total_seconds = g_config.shutdown_hour * 3600 + g_config.shutdown_minute * 60;
                    long time_diff_seconds = current_total_seconds - scheduled_total_seconds;

                    // 只有当前时间与排程时间的差在 [0, 60] 秒之间时才执行关机
                    if (time_diff_seconds >= 0 && time_diff_seconds <= 60) {
                        InitiateShutdown(g_config.countdown_seconds);
                        g_shutdown_executed_today = TRUE;
                    }
                }
            } else if (LOWORD(wParam) == IDT_TIMER_SHUTDOWN_COUNTDOWN) {
                KillTimer(hWnd, IDT_TIMER_SHUTDOWN_COUNTDOWN);
                g_is_shutdown_pending = FALSE;

                STARTUPINFOW si = { sizeof(si) };
                PROCESS_INFORMATION pi = {0};
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;
                WCHAR cmdLine[] = L"shutdown.exe -s -t 0";
                if (CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE,
                                   CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                } else {
                    MessageBoxW(NULL, L"执行关机命令失败！请检查权限。", L"关机错误", MB_OK | MB_ICONERROR);
                }
                PostQuitMessage(0);
            }
            break;

        case WM_ENDSESSION:
            if (wParam == TRUE) {
                KillTimer(hWnd, IDT_TIMER_CHECK_IDLE);
                KillTimer(hWnd, IDT_TIMER_CHECK_TIMED_SHUTDOWN);
                KillTimer(hWnd, IDT_TIMER_SHUTDOWN_COUNTDOWN);
                g_is_shutdown_pending = FALSE;
            }
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// --- Program Entry Point ---
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    // 设置工作目录
    WCHAR exeDir[MAX_PATH];
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    WCHAR* p = wcsrchr(exeDir, L'\\');
    if (p != NULL) {
        *p = L'\0';
        if (!SetCurrentDirectoryW(exeDir)) {
            MessageBoxW(NULL, L"致命错误：无法设置程序工作目录！请检查程序权限或放置位置。", L"程序启动失败", MB_OK | MB_ICONERROR);
            return 1;
        }
    } else {
        MessageBoxW(NULL, L"致命错误：无法确定程序所在目录！请将程序放置在有效位置。", L"程序启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 确定配置文件路径
    swprintf_s(g_config_file_path, ARRAYSIZE(g_config_file_path), L"%s\\%s", exeDir, CONFIG_FILE_BASE_NAME);

    // 测试写入权限
    FILE* testConfigFile = _wfopen(g_config_file_path, L"a");
    if (!testConfigFile) {
        MessageBoxW(NULL,
                             L"致命错误：程序所在目录无写入权限！\n\n"
                             L"请将本程序 (ShutdownTray.exe) 移动到桌面、文档、下载等您拥有完全写入权限的目录，然后再次运行。\n"
                             L"错误码: %lu",
                             L"权限不足，无法启动", MB_OK | MB_ICONERROR);
        return 1;
    }
    fclose(testConfigFile);

    // 互斥量以确保单实例运行
    g_hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing_hwnd = FindWindowW(MAIN_WINDOW_CLASS, NULL);
        if (existing_hwnd) {
            ShowWindow(existing_hwnd, SW_RESTORE);
            SetWindowPos(existing_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetWindowPos(existing_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(existing_hwnd);
        }
        if (g_hMutex) CloseHandle(g_hMutex);
        return 0;
    }

    // 加载配置
    if (!LoadConfig(g_config_file_path)) {
        if (g_hMutex) CloseHandle(g_hMutex);
        return 1;
    }

    // 设置开机启动
    SetAutorun(g_config.enable_autorun);

    // 根据当前时间初始化 g_shutdown_executed_today
    SYSTEMTIME st_init;
    GetLocalTime(&st_init);
    long current_total_seconds_init = st_init.wHour * 3600 + st_init.wMinute * 60 + st_init.wSecond;
    long scheduled_total_seconds_init = g_config.shutdown_hour * 3600 + g_config.shutdown_minute * 60;

    if (g_config.enable_timed_shutdown) {
        // 如果当前时间已经超过或等于计划关机时间，则认为今天已经“执行”过关机（避免当天再次触发）
        if (current_total_seconds_init >= scheduled_total_seconds_init) {
            g_shutdown_executed_today = TRUE;
        } else {
            g_shutdown_executed_today = FALSE;
        }
    } else {
        g_shutdown_executed_today = FALSE;
    }
    s_last_handled_day_for_timed_shutdown = st_init.wDay;

    // 初始化 GetLastInputInfo
    InitializeGetLastInputInfo();

    // 注册窗口类
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = MainWindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = MAIN_WINDOW_CLASS;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_APPICON));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"致命错误：无法注册主窗口类！程序将无法运行。", L"程序启动失败", MB_OK | MB_ICONERROR);
        CleanupGetLastInputInfo();
        if (g_hMutex) CloseHandle(g_hMutex);
        return 1;
    }

    wc.lpfnWndProc = HiddenWindowProc;
    wc.lpszClassName = HIDDEN_WINDOW_CLASS;
    wc.hIcon = NULL;
    wc.hbrBackground = NULL;
    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"致命错误：无法注册隐藏窗口类！程序将无法接收定时器消息。", L"程序启动失败", MB_OK | MB_ICONERROR);
        CleanupGetLastInputInfo();
        if (g_hMutex) CloseHandle(g_hMutex);
        return 1;
    }

    // 创建隐藏窗口
    g_hHiddenWindow = CreateWindowExW(0, HIDDEN_WINDOW_CLASS, L"ShutdownAssistantHiddenWindow", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!g_hHiddenWindow) {
        MessageBoxW(NULL, L"致命错误：无法创建隐藏窗口！程序将无法运行。", L"程序启动失败", MB_OK | MB_ICONERROR);
        CleanupGetLastInputInfo();
        if (g_hMutex) CloseHandle(g_hMutex);
        return 1;
    }

    // 创建主窗口
    int initialCmdShow = g_config.hide_main_window ? SW_HIDE : SW_SHOW;
    g_hMainWindow = CreateWindowExW(
        0, MAIN_WINDOW_CLASS, L"定时闲置关机助手",
        WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 430, 250,
        NULL, NULL, hInstance, NULL);

    if (!g_hMainWindow) {
        MessageBoxW(NULL, L"致命错误：无法创建主界面窗口！程序将无法运行。", L"程序启动失败", MB_OK | MB_ICONERROR);
        DestroyWindow(g_hHiddenWindow);
        CleanupGetLastInputInfo();
        if (g_hMutex) CloseHandle(g_hMutex);
        return 1;
    }

    ApplyConfigToGUI();
    ShowWindow(g_hMainWindow, initialCmdShow);
    UpdateWindow(g_hMainWindow);

    SetShutdownTimers();

    // 消息循环
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 清理资源
    CleanupGetLastInputInfo();
    if (g_hMutex) CloseHandle(g_hMutex);
    UnregisterClassW(MAIN_WINDOW_CLASS, hInstance);
    UnregisterClassW(HIDDEN_WINDOW_CLASS, hInstance);
    return (int)msg.wParam;
}
