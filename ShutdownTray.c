#define UNICODE
#define _UNICODE

#define _WIN32_WINNT 0x0501 // For Windows XP compatibility

#include <windows.h>       
#include <stdio.h>         // For FILE operations (for config.ini), sscanf, fprintf
#include <stdlib.h>        // For malloc, free
#include <string.h>        // For strlen, strcpy, strstr, strchr, strncpy
#include <wchar.h>         // For wide character string operations, wcscmp, wcsstr etc.
#include <time.h>          // For time-related functions
#include <io.h>            // For _commit (for INI file flush)

// For Resource Hacker icon embedding
#define IDI_APPICON 101 // 您通常会将其定义在 resource.h 中并包含它

// --- 常量和全局变量 ---
// 窗口类名
const WCHAR *MAIN_WINDOW_CLASS = L"ShutdownAssistantMainWindowClass";
const WCHAR *HIDDEN_WINDOW_CLASS = L"ShutdownAssistantHiddenWindowClass";
const WCHAR *MUTEX_NAME = L"Global\\ShutdownAssistantMutex"; // 全局互斥锁名称

// 配置文件名
const WCHAR *CONFIG_FILE_BASE_NAME = L"config.ini";
const WCHAR *CONFIG_SECTION_NAME = L"Settings"; // INI 文件中的节名称

// 配置文件路径（全局，将存储完整绝对路径）
WCHAR g_config_file_path[MAX_PATH];

// 定时器 ID
#define IDT_TIMER_CHECK_IDLE 2001
#define IDT_TIMER_CHECK_TIMED_SHUTDOWN 2002
#define IDT_TIMER_SHUTDOWN_COUNTDOWN 2003

// GUI 控件 ID
#define IDC_CHK_AUTORUN             100
#define IDC_CHK_TIMED_SHUTDOWN      101
#define IDC_EDIT_SHUTDOWN_HOUR      102
#define IDC_EDIT_SHUTDOWN_MINUTE    103
#define IDC_CHK_IDLE_SHUTDOWN       104
#define IDC_EDIT_IDLE_MINUTES       105
#define IDC_EDIT_COUNTDOWN_SECONDS  106
#define IDC_CHK_HIDE_MAIN_WINDOW    108 // 下次启动时隐藏主窗口
#define IDC_BTN_SAVE_SETTINGS       109
#define IDC_BTN_SHUTDOWN_NOW        110
#define IDC_BTN_EXIT_APP            112 // 退出应用程序按钮
#define IDC_BTN_HIDE_PROGRAM        113 // 新增：隐藏主窗口按钮（当前启动）


// 全局变量
HWND g_hMainWindow = NULL; // 主 GUI 窗口句柄
HWND g_hHiddenWindow = NULL; // 用于定时器的隐藏窗口句柄
HANDLE g_hMutex = NULL; // 用于单实例运行的全局互斥锁句柄
BOOL g_shutdown_executed_today = FALSE; // 标记今天是否已执行过定时关机
BOOL g_is_shutdown_pending = FALSE; // 标记是否已启动关机倒计时

// GetLastInputInfo 函数指针类型定义
typedef BOOL (WINAPI *PFN_GetLastInputInfo)(LPLASTINPUTINFO);
PFN_GetLastInputInfo pfnGetLastInputInfo = NULL; // 用于存储 GetLastInputInfo 的函数地址
HMODULE hUser32 = NULL; // 用于存储 User32.dll 模块的句柄


// --- 配置结构体 ---
typedef struct {
    BOOL enable_autorun;        // 是否随 Windows 自动启动
    BOOL enable_timed_shutdown; // 是否启用定时关机
    int shutdown_hour;          // 定时关机小时 (0-23)
    int shutdown_minute;        // 定时关机分钟 (0-59)
    BOOL enable_idle_shutdown;  // 是否启用空闲关机
    int idle_minutes;           // 空闲多久（分钟）后关机
    int countdown_seconds;      // 执行关机命令前的倒计时秒数
    BOOL hide_main_window;      // 下次启动时是否隐藏主窗口
} AppConfig;

AppConfig g_config; // 全局配置实例

// --- 函数原型 ---
// 窗口过程
LRESULT CALLBACK MainWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK HiddenWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// 辅助函数：从 INI 文件读取布尔值
BOOL GetPrivateProfileBoolW(LPCWSTR lpAppName, LPCWSTR lpKeyName, BOOL fDefault, LPCWSTR lpFileName);
// 辅助函数：向 INI 文件写入布尔值
BOOL WritePrivateProfileBoolW(LPCWSTR lpAppName, LPCWSTR lpKeyName, BOOL fValue, LPCWSTR lpFileName);
// 辅助函数：向 INI 文件写入整数值 (使用 WritePrivateProfileStringW)
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
DWORD GetIdleTime(); // 保持不变，内部实现将使用函数指针

// 新增函数原型 (用于动态加载 GetLastInputInfo)
void InitializeGetLastInputInfo();
void CleanupGetLastInputInfo();


// --- 辅助函数 ---
BOOL GetPrivateProfileBoolW(LPCWSTR lpAppName, LPCWSTR lpKeyName, BOOL fDefault, LPCWSTR lpFileName) {
    WCHAR szRet[8]; // "true" 或 "false" + null 终止符
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

// 动态加载 GetLastInputInfo 函数
void InitializeGetLastInputInfo() {
    hUser32 = LoadLibraryW(L"User32.dll");
    if (hUser32) {
        pfnGetLastInputInfo = (PFN_GetLastInputInfo)GetProcAddress(hUser32, "GetLastInputInfo");
        if (!pfnGetLastInputInfo) {
            FreeLibrary(hUser32);
            hUser32 = NULL;
            // 可以选择在这里给用户一个警告，但通常不显示这种底层错误
            // MessageBoxW(NULL, L"警告：无法加载 GetLastInputInfo 函数。空闲关机可能无法正常工作。", L"加载错误", MB_OK | MB_ICONWARNING);
        }
    } else {
        // 可以选择在这里给用户一个警告
        // MessageBoxW(NULL, L"警告：无法加载 User32.dll。空闲关机可能无法正常工作。", L"加载错误", MB_OK | MB_ICONWARNING);
    }
}

// 释放 User32.dll
void CleanupGetLastInputInfo() {
    if (hUser32) {
        FreeLibrary(hUser32);
        hUser32 = NULL;
        pfnGetLastInputInfo = NULL;
    }
}


// --- 配置读/写函数 ---
BOOL LoadConfig(const WCHAR* configPath) {
    // 使用默认值初始化。这些值将被成功读取的数据覆盖。
    g_config.enable_autorun = FALSE;
    g_config.enable_timed_shutdown = FALSE;
    g_config.shutdown_hour = 0;
    g_config.shutdown_minute = 0;
    g_config.enable_idle_shutdown = FALSE;
    g_config.idle_minutes = 0;
    g_config.countdown_seconds = 30; // 默认 30 秒倒计时
    g_config.hide_main_window = FALSE;    // 默认：显示 GUI

    // 使用 FindFirstFileW 检查配置文件是否存在
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(configPath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        // config.ini 未找到。生成默认配置。
        FindClose(hFind); // 关闭句柄（即使无效）
        
        // --- 使用 _wfopen 手动创建并写入默认配置 ---
        FILE* f_create = _wfopen(configPath, L"w,ccs=UTF-16LE");
        if (!f_create) { // 如果 _wfopen 失败，则是文件创建的严重错误
             MessageBoxW(NULL, L"致命错误：无法创建配置文件！请检查程序权限。\n尝试将程序放在桌面或“文档”等有写入权限的目录。\n错误码: %lu", L"配置文件创建失败", MB_OK | MB_ICONERROR);
             return FALSE; // 如果创建失败则返回 FALSE
        }
        
        // 写入默认 INI 内容
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
    } else {
        // config.ini 已找到。
    }
    FindClose(hFind); // 确保在找到文件时也关闭句柄

    // 从 INI 文件读取值（无论是刚创建的还是已存在的）
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
    
    // 向 INI 文件写入值
    WritePrivateProfileBoolW(CONFIG_SECTION_NAME, L"EnableAutorun", g_config.enable_autorun, configPath);
    WritePrivateProfileBoolW(CONFIG_SECTION_NAME, L"EnableTimedShutdown", g_config.enable_timed_shutdown, configPath);
    WritePrivateProfileIntW(CONFIG_SECTION_NAME, L"ShutdownHour", g_config.shutdown_hour, configPath);
    WritePrivateProfileIntW(CONFIG_SECTION_NAME, L"ShutdownMinute", g_config.shutdown_minute, configPath);
    WritePrivateProfileBoolW(CONFIG_SECTION_NAME, L"EnableIdleShutdown", g_config.enable_idle_shutdown, configPath);
    WritePrivateProfileIntW(CONFIG_SECTION_NAME, L"IdleMinutes", g_config.idle_minutes, configPath);
    WritePrivateProfileIntW(CONFIG_SECTION_NAME, L"CountdownSeconds", g_config.countdown_seconds, configPath);
    WritePrivateProfileBoolW(CONFIG_SECTION_NAME, L"HideMainWindow", g_config.hide_main_window, configPath);

    // 强制将 INI 文件写入磁盘
    if (WritePrivateProfileStringW(NULL, NULL, NULL, configPath) == 0) {
        return FALSE;
    }

    return TRUE;
}

// --- 自动运行函数 ---
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

// --- 关机函数 ---
void InitiateShutdown(UINT countdown) {
    // 如果已经有倒计时在进行，则不重复启动
    if (g_is_shutdown_pending) {
        return;
    }

    // 标记关机倒计时已启动
    g_is_shutdown_pending = TRUE;

    // 设置倒计时定时器 (先设置定时器，再显示弹窗)
    // 即使弹窗阻塞了，定时器也已经在后台独立运行
    SetTimer(g_hHiddenWindow, IDT_TIMER_SHUTDOWN_COUNTDOWN, countdown * 1000, NULL);

    // 显示倒计时开始提示
    WCHAR szMessage[256];
    swprintf_s(szMessage, ARRAYSIZE(szMessage), L"系统将在 %d 秒后关机。请保存您的工作。", countdown);
    MessageBoxW(g_hMainWindow, szMessage, L"关机提示", MB_OK | MB_ICONWARNING); 
}

void StopShutdownCountdown() {
    KillTimer(g_hHiddenWindow, IDT_TIMER_SHUTDOWN_COUNTDOWN);
    g_is_shutdown_pending = FALSE; // 取消倒计时后，重置标志
    // 可选：停止倒计时时，可以考虑在这里给用户一个提示
    // MessageBoxW(g_hMainWindow, L"关机倒计时已取消。", L"提示", MB_OK | MB_ICONINFORMATION);
}

// 获取系统空闲时间 (使用动态加载的 GetLastInputInfo)
DWORD GetIdleTime() {
    // 检查函数指针是否有效
    if (pfnGetLastInputInfo) {
        LASTINPUTINFO lii = { sizeof(LASTINPUTINFO) };
        // 调用动态加载的 GetLastInputInfo 函数
        if (pfnGetLastInputInfo(&lii)) {
            return (GetTickCount() - lii.dwTime);
        }
    }
    // 如果函数未加载或调用失败，返回0 (表示没有空闲，或者无法检测)
    return 0; 
}


// --- 定时器管理函数 ---
void SetShutdownTimers() {
    // 确保在重新设置定时器时，停止所有旧的定时器，特别是防止重复触发关机
    KillTimer(g_hHiddenWindow, IDT_TIMER_CHECK_IDLE);
    KillTimer(g_hHiddenWindow, IDT_TIMER_CHECK_TIMED_SHUTDOWN);
    StopShutdownCountdown(); // 确保也停止了任何待处理的关机倒计时

    if (g_config.enable_idle_shutdown) {
        if (g_config.idle_minutes <= 0) {
            g_config.enable_idle_shutdown = FALSE; // 无效值则禁用空闲关机
        } else {
            // 空闲检查定时器设置为每 30 秒检查一次，更频繁地监控空闲状态
            SetTimer(g_hHiddenWindow, IDT_TIMER_CHECK_IDLE, 30 * 1000, NULL); 
        }
    }

    if (g_config.enable_timed_shutdown) {
        if (g_config.shutdown_hour < 0 || g_config.shutdown_hour > 23 ||
            g_config.shutdown_minute < 0 || g_config.shutdown_minute > 59) {
            g_config.enable_timed_shutdown = FALSE; // 无效值则禁用定时关机
        } else {
            // 定时器每分钟检查一次定时关机条件
            SetTimer(g_hHiddenWindow, IDT_TIMER_CHECK_TIMED_SHUTDOWN, 60 * 1000, NULL);
        }
    }
}

// --- GUI 函数 ---
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

// --- 主 GUI 窗口过程 ---
LRESULT CALLBACK MainWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hMainWindow = hWnd;
            int yPos = 20;
            const int lineSpacing = 28; // 更紧凑的行间距
            const int labelWidth = 120; // 稍微窄的标签宽度
            const int checkboxColX = 145; // 复选框/编辑框的统一列
            const int editWidthSmall = 35;
            const int editWidthLarge = 60;
            
            // 更小的按钮尺寸
            const int buttonWidth = 75; 
            const int buttonHeight = 25;
            const int buttonHorizontalSpacing = 8; // 稍微减少间距

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

            CreateWindowW(L"STATIC", L"下次启动隐藏界面:", WS_VISIBLE | WS_CHILD, 20, yPos, labelWidth + 30, 20, hWnd, NULL, NULL, NULL);
            CreateWindowW(L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, checkboxColX + 30, yPos, 20, 20, hWnd, (HMENU)IDC_CHK_HIDE_MAIN_WINDOW, NULL, NULL);
            yPos += lineSpacing + 15; // 按钮前的额外间距

            // 按钮布局 - 计算以居中
            RECT clientRect;
            GetClientRect(hWnd, &clientRect);
            int windowWidth = clientRect.right - clientRect.left;

            // 所有按钮加间距的总宽度
            int totalButtonsWidth = (buttonWidth * 4) + (buttonHorizontalSpacing * 3);
            int btnX = (windowWidth - totalButtonsWidth) / 2; // 计算起始 X 坐标以居中

            CreateWindowW(L"BUTTON", L"保存设置", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnX, yPos, buttonWidth, buttonHeight, hWnd, (HMENU)IDC_BTN_SAVE_SETTINGS, NULL, NULL);
            btnX += buttonWidth + buttonHorizontalSpacing;
            CreateWindowW(L"BUTTON", L"立即关机", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnX, yPos, buttonWidth, buttonHeight, hWnd, (HMENU)IDC_BTN_SHUTDOWN_NOW, NULL, NULL);
            btnX += buttonWidth + buttonHorizontalSpacing;
            CreateWindowW(L"BUTTON", L"隐藏程序", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnX, yPos, buttonWidth, buttonHeight, hWnd, (HMENU)IDC_BTN_HIDE_PROGRAM, NULL, NULL); // 新增隐藏按钮
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
                    SetShutdownTimers(); // 重新设置所有定时器
                    MessageBoxW(hWnd, L"设置已保存并应用！", L"提示", MB_OK | MB_ICONINFORMATION);
                    break;
                case IDC_BTN_SHUTDOWN_NOW:
                    // 立即关机按钮应始终尝试启动倒计时，但要确保不重复启动
                    InitiateShutdown(g_config.countdown_seconds);
                    break;
                case IDC_BTN_HIDE_PROGRAM: // 处理新的隐藏按钮
                    ShowWindow(hWnd, SW_HIDE);
                    break;
                case IDC_BTN_EXIT_APP:
                    // 销毁隐藏窗口，这将发布 WM_QUIT 消息
                    DestroyWindow(g_hHiddenWindow);
                    break;
            }
            break;
        }

        case WM_CLOSE:
            // 当用户点击“X”按钮时，隐藏窗口而不是销毁它
            ShowWindow(hWnd, SW_HIDE);
            break;

        case WM_DESTROY:
            // 此消息由 HiddenWindowProc 中的 PostQuitMessage 处理，当调用 DestroyWindow 时
            break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// --- 隐藏窗口过程 (用于定时器) ---
LRESULT CALLBACK HiddenWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g_hHiddenWindow = hWnd;
            break;

        case WM_TIMER:
            if (LOWORD(wParam) == IDT_TIMER_CHECK_IDLE) {
                // 如果空闲关机已启用且当前没有待处理的关机倒计时
                if (g_config.enable_idle_shutdown && !g_is_shutdown_pending) {
                    DWORD idle_time_ms = GetIdleTime(); // 获取当前空闲时间（毫秒）
                    // 将配置的空闲分钟转换为毫秒
                    DWORD configured_idle_time_ms = (DWORD)g_config.idle_minutes * 60 * 1000;

                    // 只有当空闲时间达到设定值并且设定的空闲分钟数大于0时才触发
                    if (configured_idle_time_ms > 0 && idle_time_ms >= configured_idle_time_ms) {
                        InitiateShutdown(g_config.countdown_seconds);
                    }
                }
            } else if (LOWORD(wParam) == IDT_TIMER_CHECK_TIMED_SHUTDOWN) {
                SYSTEMTIME st;
                GetLocalTime(&st);

                // 在午夜重置 shutdown_executed_today 标志
                // 仅当日期发生变化时才重置
                static WORD last_day = -1; // 静态变量记录上次检查的日期
                if (last_day == (WORD)-1) { // 第一次运行或程序启动
                    last_day = st.wDay;
                } else if (st.wDay != last_day) { // 日期改变了
                    g_shutdown_executed_today = FALSE;
                    last_day = st.wDay; // 更新日期
                }

                // 如果定时关机已启用，今天尚未执行，并且当前没有待处理的关机倒计时
                if (g_config.enable_timed_shutdown && !g_shutdown_executed_today && !g_is_shutdown_pending) {
                    int currentTimeInMinutes = st.wHour * 60 + st.wMinute;
                    int scheduledTimeInMinutes = g_config.shutdown_hour * 60 + g_config.shutdown_minute;

                    // 检查当前时间是否已到达或超过预定关机时间
                    if (currentTimeInMinutes >= scheduledTimeInMinutes) {
                        InitiateShutdown(g_config.countdown_seconds);
                        g_shutdown_executed_today = TRUE; // 标记今天已执行
                    }
                }
            } else if (LOWORD(wParam) == IDT_TIMER_SHUTDOWN_COUNTDOWN) {
                KillTimer(hWnd, IDT_TIMER_SHUTDOWN_COUNTDOWN); // 停止倒计时定时器
                g_is_shutdown_pending = FALSE; // 倒计时结束，重置标志

                STARTUPINFOW si = { sizeof(si) };
                PROCESS_INFORMATION pi = {0};
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE; // 隐藏命令提示符窗口

                WCHAR cmdLine[] = L"shutdown.exe -s -t 0"; // 关机命令
                
                if (CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE,
                                    CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                } else {
                    // 严重错误：未能执行 shutdown.exe
                    MessageBoxW(NULL, L"执行关机命令失败！请检查权限。", L"关机错误", MB_OK | MB_ICONERROR);
                }
                PostQuitMessage(0); // 启动关机后退出应用程序
            }
            break;

        case WM_ENDSESSION:
            // 处理系统关机/注销，清理定时器
            if (wParam == TRUE) {
                KillTimer(hWnd, IDT_TIMER_CHECK_IDLE);
                KillTimer(hWnd, IDT_TIMER_CHECK_TIMED_SHUTDOWN);
                KillTimer(hWnd, IDT_TIMER_SHUTDOWN_COUNTDOWN);
                g_is_shutdown_pending = FALSE; // 会话结束前，重置标志
            }
            return 0; // 返回 0 允许会话结束
        case WM_QUERYENDSESSION:
            return TRUE; // 允许系统关机/注销
        case WM_DESTROY:
            PostQuitMessage(0); // 发布退出消息以结束消息循环
            break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}


// --- 程序入口点 ---
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    // --- 设置当前工作目录（仍为可执行文件目录） ---
    WCHAR exeDir[MAX_PATH];
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    WCHAR* p = wcsrchr(exeDir, L'\\');
    if (p != NULL) {
        *p = L'\0'; // Null-terminate 以只获取目录路径
        if (!SetCurrentDirectoryW(exeDir)) {
            MessageBoxW(NULL, L"致命错误：无法设置程序工作目录！请检查程序权限或放置位置。", L"程序启动失败", MB_OK | MB_ICONERROR);
            return 1;
        }
    } else {
        MessageBoxW(NULL, L"致命错误：无法确定程序所在目录！请将程序放置在有效位置。", L"程序启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }

    // --- 确定配置文件路径（现在是程序目录中的绝对路径） ---
    swprintf_s(g_config_file_path, ARRAYSIZE(g_config_file_path), L"%s\\%s", exeDir, CONFIG_FILE_BASE_NAME);

    // --- 初始文件写入权限测试（针对 config.ini） ---
    FILE* testConfigFile = _wfopen(g_config_file_path, L"a"); // "a" 模式用于追加（如果不存在则创建）
    if (!testConfigFile) {
        MessageBoxW(NULL,
                    L"致命错误：程序所在目录无写入权限！\n\n"
                    L"请将本程序 (ShutdownTray.exe) 移动到桌面、文档、下载等您拥有完全写入权限的目录，然后再次运行。\n"
                    L"错误码: %lu",
                    L"权限不足，无法启动", MB_OK | MB_ICONERROR);
        return 1;
    }
    fclose(testConfigFile); // 立即关闭测试文件

    // --- 互斥锁以确保单实例运行 ---
    g_hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing_hwnd = FindWindowW(MAIN_WINDOW_CLASS, NULL);
        if (existing_hwnd) {
            ShowWindow(existing_hwnd, SW_RESTORE);
            SetWindowPos(existing_hwnd, HWND_TOPMOST, 0,0,0,0, SWP_NOMOVE | SWP_NOSIZE);
            SetWindowPos(existing_hwnd, HWND_NOTOPMOST, 0,0,0,0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(existing_hwnd);
        } else {
            // 如果未找到主窗口，则不显示重复实例消息。
        }
        if (g_hMutex) CloseHandle(g_hMutex);
        return 0;
    }
    
    // --- 加载配置（如果未找到将生成） ---
    if (!LoadConfig(g_config_file_path)) {
        // LoadConfig 中已显示致命错误消息框
        if (g_hMutex) CloseHandle(g_hMutex);
        return 1;
    }
    
    // --- 加载配置后设置自动运行 ---
    SetAutorun(g_config.enable_autorun);

    // --- 初始化 g_shutdown_executed_today 标志 (修复逻辑) ---
    SYSTEMTIME st_init;
    GetLocalTime(&st_init);
    
    // 将当前时间转换为分钟数
    int currentTimeInMinutes_init = st_init.wHour * 60 + st_init.wMinute;
    // 将设定关机时间转换为分钟数
    int scheduledTimeInMinutes_init = g_config.shutdown_hour * 60 + g_config.shutdown_minute;
    
    // 只有在启用定时关机且当前时间在设定关机时间之前时，才将标志设为 FALSE
    // 否则，认为今天的关机机会已过，或未启用定时关机，将标志设为 TRUE
    if (g_config.enable_timed_shutdown && currentTimeInMinutes_init < scheduledTimeInMinutes_init) {
        g_shutdown_executed_today = FALSE;
    } else {
        g_shutdown_executed_today = TRUE;
    }
    
    // 初始化 g_is_shutdown_pending 标志
    g_is_shutdown_pending = FALSE;

    // --- 在注册窗口类之前调用初始化 GetLastInputInfo 函数 ---
    InitializeGetLastInputInfo();

    // --- 注册窗口类 ---
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = MainWindowProc; // 主窗口过程
    wc.hInstance = hInstance;
    wc.lpszClassName = MAIN_WINDOW_CLASS;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_APPICON)); // 从资源加载图标 (用于 Resource Hacker)
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); // 标准灰色背景
    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"致命错误：无法注册主窗口类！程序将无法运行。", L"程序启动失败", MB_OK | MB_ICONERROR);
        CleanupGetLastInputInfo(); // 清理动态加载的DLL
        if (g_hMutex) CloseHandle(g_hMutex); return 1;
    }

    wc.lpfnWndProc = HiddenWindowProc; // 隐藏窗口过程
    wc.lpszClassName = HIDDEN_WINDOW_CLASS;
    wc.hIcon = NULL; // 隐藏窗口不需要图标
    wc.hbrBackground = NULL; // 隐藏窗口没有背景
    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"致命错误：无法注册隐藏窗口类！程序将无法接收定时器消息。", L"程序启动失败", MB_OK | MB_ICONERROR);
        CleanupGetLastInputInfo(); // 清理动态加载的DLL
        if (g_hMutex) CloseHandle(g_hMutex); return 1;
    }

    // --- 创建隐藏窗口（必须存在才能使用定时器） ---
    g_hHiddenWindow = CreateWindowExW(0, HIDDEN_WINDOW_CLASS, L"ShutdownAssistantHiddenWindow", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!g_hHiddenWindow) {
        MessageBoxW(NULL, L"致命错误：无法创建隐藏窗口！程序将无法运行。", L"程序启动失败", MB_OK | MB_ICONERROR);
        CleanupGetLastInputInfo(); // 清理动态加载的DLL
        if (g_hMutex) CloseHandle(g_hMutex); return 1;
    }

    // --- 决定是显示主窗口还是隐藏它 ---
    int initialCmdShow;
    if (g_config.hide_main_window) {
        initialCmdShow = SW_HIDE; // 启动时隐藏
    } else {
        initialCmdShow = SW_SHOW; // 显示主窗口
    }

    // --- 创建主 GUI 窗口 ---
    g_hMainWindow = CreateWindowExW(
        0, MAIN_WINDOW_CLASS, L"定时空闲关机助手", // 窗口标题改为简体
        WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, // 标准窗口样式
        CW_USEDEFAULT, CW_USEDEFAULT, 430, 250, // 调整宽度和高度以获得更好、更紧凑的布局
        NULL, NULL, hInstance, NULL);

    if (!g_hMainWindow) {
        MessageBoxW(NULL, L"致命错误：无法创建主界面窗口！程序将无法运行。", L"程序启动失败", MB_OK | MB_ICONERROR);
        DestroyWindow(g_hHiddenWindow); // 也清理隐藏窗口
        CleanupGetLastInputInfo(); // 清理动态加载的DLL
        if (g_hMutex) CloseHandle(g_hMutex); return 1;
    }
    
    // --- 将初始配置应用于 GUI 控件 ---
    ApplyConfigToGUI();

    ShowWindow(g_hMainWindow, initialCmdShow); // 根据配置显示或隐藏
    UpdateWindow(g_hMainWindow);

    // --- 根据加载的配置设置关机定时器 ---
    SetShutdownTimers();

    // --- 消息循环 ---
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // --- 退出时清理 ---
    CleanupGetLastInputInfo(); // 在程序退出前释放 User32.dll
    if (g_hMutex) CloseHandle(g_hMutex);
    UnregisterClassW(MAIN_WINDOW_CLASS, hInstance);
    UnregisterClassW(HIDDEN_WINDOW_CLASS, hInstance);
    return (int)msg.wParam;
}
