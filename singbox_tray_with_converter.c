#define UNICODE
#define _UNICODE

#define __STDC_WANT_LIB_EXT1__ 1
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wininet.h>
#include <commctrl.h>

#include "cJSON.h"

#define WM_TRAY (WM_USER + 1)
#define ID_TRAY_EXIT 1001
#define ID_TRAY_AUTORUN 1002
#define ID_TRAY_SYSTEM_PROXY 1003
#define ID_TRAY_OPEN_CONVERTER 1004
#define ID_TRAY_SETTINGS 1005
#define ID_TRAY_NODE_BASE 2000

#define ID_GLOBAL_HOTKEY 9001
#define ID_HOTKEY_CTRL 101

#ifndef IDR_HTML_CONVERTER
#define IDR_HTML_CONVERTER 2
#endif
#ifndef RT_HTML
#define RT_HTML L"HTML"
#endif

// 全局变量
NOTIFYICONDATAW nid;
HWND hwnd;
HMENU hMenu, hNodeSubMenu;
HANDLE hMutex = NULL;
PROCESS_INFORMATION pi = {0};

wchar_t** nodeTags = NULL;
int nodeCount = 0;
int nodeCapacity = 0;
wchar_t currentNode[64] = L"";
int httpPort = 0;

const wchar_t* REG_PATH_PROXY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";

// 新增全局变量
BOOL g_isIconVisible = TRUE;
UINT g_hotkeyModifiers = 0;
UINT g_hotkeyVk = 0;
wchar_t g_iniFilePath[MAX_PATH] = {0};

// 函数声明
void ShowTrayTip(const wchar_t* title, const wchar_t* message);
void ShowError(const wchar_t* title, const wchar_t* message);
BOOL ReadFileToBuffer(const wchar_t* filename, char** buffer, long* fileSize);
void CleanupDynamicNodes();
void LoadSettings();
void SaveSettings();
void ToggleTrayIconVisibility();
LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void OpenSettingsWindow();
BOOL ParseTags();
int GetHttpInboundPort();
void StartSingBox();
void SwitchNode(const wchar_t* tag);
void SetSystemProxy(BOOL enable);
BOOL IsSystemProxyEnabled();
void SafeReplaceOutbound(const wchar_t* newTag);
void UpdateMenu();
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void StopSingBox();
void SetAutorun(BOOL enable);
BOOL IsAutorunEnabled();
void OpenConverterHtmlFromResource();

// 辅助函数
void ShowTrayTip(const wchar_t* title, const wchar_t* message) {
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy(nid.szInfoTitle, title, ARRAYSIZE(nid.szInfoTitle) - 1);
    nid.szInfoTitle[ARRAYSIZE(nid.szInfoTitle) - 1] = L'\0';
    wcsncpy(nid.szInfo, message, ARRAYSIZE(nid.szInfo) - 1);
    nid.szInfo[ARRAYSIZE(nid.szInfo) - 1] = L'\0';
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

void ShowError(const wchar_t* title, const wchar_t* message) {
    DWORD errorCode = GetLastError();
    wchar_t* sysMsgBuf = NULL;
    wchar_t fullMessage[1024] = {0};
    wcsncpy(fullMessage, message, ARRAYSIZE(fullMessage) - 1);
    fullMessage[ARRAYSIZE(fullMessage) - 1] = L'\0';
    if (errorCode != 0) {
        FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPWSTR)&sysMsgBuf, 0, NULL);
        if (sysMsgBuf) {
            wcsncat(fullMessage, L"\n\n系统错误信息:\n", ARRAYSIZE(fullMessage) - wcslen(fullMessage) - 1);
            wcsncat(fullMessage, sysMsgBuf, ARRAYSIZE(fullMessage) - wcslen(fullMessage) - 1);
            LocalFree(sysMsgBuf);
        }
    }
    MessageBoxW(NULL, fullMessage, title, MB_OK | MB_ICONERROR);
}

BOOL ReadFileToBuffer(const wchar_t* filename, char** buffer, long* fileSize) {
    FILE* f = NULL;
    if (_wfopen_s(&f, filename, L"rb") != 0 || !f) { return FALSE; }
    fseek(f, 0, SEEK_END);
    *fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (*fileSize <= 0) { fclose(f); return FALSE; }
    *buffer = (char*)malloc(*fileSize + 1);
    if (!*buffer) { fclose(f); return FALSE; }
    fread(*buffer, 1, *fileSize, f);
    (*buffer)[*fileSize] = '\0';
    fclose(f);
    return TRUE;
}

void CleanupDynamicNodes() {
    if (nodeTags) {
        for (int i = 0; i < nodeCount; i++) { free(nodeTags[i]); }
        free(nodeTags);
        nodeTags = NULL;
    }
    nodeCount = 0;
    nodeCapacity = 0;
}

// 快捷键设置功能函数
void LoadSettings() {
    g_hotkeyModifiers = GetPrivateProfileIntW(L"Settings", L"Modifiers", 0, g_iniFilePath);
    g_hotkeyVk = GetPrivateProfileIntW(L"Settings", L"VK", 0, g_iniFilePath);
    g_isIconVisible = GetPrivateProfileIntW(L"Settings", L"ShowIcon", 1, g_iniFilePath);
}

void SaveSettings() {
    wchar_t buffer[16];
    wsprintfW(buffer, L"%u", g_hotkeyModifiers);
    WritePrivateProfileStringW(L"Settings", L"Modifiers", buffer, g_iniFilePath);
    wsprintfW(buffer, L"%u", g_hotkeyVk);
    WritePrivateProfileStringW(L"Settings", L"VK", buffer, g_iniFilePath);
    wsprintfW(buffer, L"%d", g_isIconVisible);
    WritePrivateProfileStringW(L"Settings", L"ShowIcon", buffer, g_iniFilePath);
}

void ToggleTrayIconVisibility() {
    if (g_isIconVisible) { Shell_NotifyIconW(NIM_DELETE, &nid); }
    else { Shell_NotifyIconW(NIM_ADD, &nid); }
    g_isIconVisible = !g_isIconVisible;
    SaveSettings();
}

UINT HotkeyfToMod(UINT flags) {
    UINT mods = 0;
    if (flags & HOTKEYF_ALT) mods |= MOD_ALT;
    if (flags & HOTKEYF_CONTROL) mods |= MOD_CONTROL;
    if (flags & HOTKEYF_SHIFT) mods |= MOD_SHIFT;
    if (flags & HOTKEYF_EXT) mods |= MOD_WIN;
    return mods;
}

UINT ModToHotkeyf(UINT mods) {
    UINT flags = 0;
    if (mods & MOD_ALT) flags |= HOTKEYF_ALT;
    if (mods & MOD_CONTROL) flags |= HOTKEYF_CONTROL;
    if (mods & MOD_SHIFT) flags |= HOTKEYF_SHIFT;
    if (mods & MOD_WIN) flags |= HOTKEYF_EXT;
    return flags;
}

LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hHotkey;
    switch (msg) {
        case WM_CREATE: {
            CreateWindowW(L"STATIC", L"请按下快捷键:", WS_CHILD | WS_VISIBLE, 20, 20, 150, 20, hWnd, NULL, NULL, NULL);
            hHotkey = CreateWindowExW(0, HOTKEY_CLASSW, NULL, WS_CHILD | WS_VISIBLE | WS_BORDER, 20, 45, 240, 25, hWnd, (HMENU)ID_HOTKEY_CTRL, NULL, NULL);
            SendMessageW(hHotkey, HKM_SETHOTKEY, MAKEWORD(g_hotkeyVk, ModToHotkeyf(g_hotkeyModifiers)), 0);
            CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 60, 85, 80, 25, hWnd, (HMENU)IDOK, NULL, NULL);
            CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 160, 85, 80, 25, hWnd, (HMENU)IDCANCEL, NULL, NULL);
            break;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDOK: {
                    LRESULT result = SendMessageW(hHotkey, HKM_GETHOTKEY, 0, 0);
                    UINT newVk = LOBYTE(result);
                    UINT newModsFlags = HIBYTE(result);
                    UINT newMods = HotkeyfToMod(newModsFlags);
                    UnregisterHotKey(hwnd, ID_GLOBAL_HOTKEY);
                    if (RegisterHotKey(hwnd, ID_GLOBAL_HOTKEY, newMods, newVk)) {
                        g_hotkeyModifiers = newMods; g_hotkeyVk = newVk;
                        SaveSettings();
                        MessageBoxW(hWnd, L"快捷键设置成功！", L"提示", MB_OK);
                    } else if (newVk != 0 || newMods != 0) {
                        MessageBoxW(hWnd, L"快捷键设置失败，可能已被其他程序占用。", L"错误", MB_OK | MB_ICONERROR);
                        if (g_hotkeyVk != 0 || g_hotkeyModifiers != 0) { RegisterHotKey(hwnd, ID_GLOBAL_HOTKEY, g_hotkeyModifiers, g_hotkeyVk); }
                    } else {
                        g_hotkeyModifiers = 0; g_hotkeyVk = 0;
                        SaveSettings();
                        MessageBoxW(hWnd, L"快捷键已清除。", L"提示", MB_OK);
                    }
                    DestroyWindow(hWnd);
                    break;
                }
                case IDCANCEL: DestroyWindow(hWnd); break;
            }
            break;
        }
        case WM_CLOSE: DestroyWindow(hWnd); break;
        case WM_DESTROY: EnableWindow(hwnd, TRUE); SetForegroundWindow(hwnd); break;
        default: return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

void OpenSettingsWindow() {
    const wchar_t* SETTINGS_CLASS_NAME = L"SingboxSettingsWindowClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = SETTINGS_CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (!GetClassInfoW(wc.hInstance, SETTINGS_CLASS_NAME, &wc)) { RegisterClassW(&wc); }
    HWND hSettingsWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, SETTINGS_CLASS_NAME, L"设置快捷键", WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 300, 160, hwnd, NULL, wc.hInstance, NULL);
    if (hSettingsWnd) {
        EnableWindow(hwnd, FALSE);
        RECT rc, rcOwner;
        GetWindowRect(hSettingsWnd, &rc);
        GetWindowRect(GetDesktopWindow(), &rcOwner);
        SetWindowPos(hSettingsWnd, HWND_TOP, (rcOwner.right - (rc.right - rc.left)) / 2, (rcOwner.bottom - (rc.bottom - rc.top)) / 2, 0, 0, SWP_NOSIZE);
        ShowWindow(hSettingsWnd, SW_SHOW);
        UpdateWindow(hSettingsWnd);
    }
}

// 核心功能函数
BOOL ParseTags() {
    CleanupDynamicNodes();
    currentNode[0] = L'\0';
    httpPort = 0;
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(L"config.json", &buffer, &size)) {
        return FALSE;
    }
    cJSON* root = cJSON_Parse(buffer);
    if (!root) {
        free(buffer);
        return FALSE;
    }
    cJSON* outbounds = cJSON_GetObjectItem(root, "outbounds");
    cJSON* outbound = NULL;
    cJSON_ArrayForEach(outbound, outbounds) {
        cJSON* tag = cJSON_GetObjectItem(outbound, "tag");
        if (cJSON_IsString(tag) && tag->valuestring) {
            if (nodeCount >= nodeCapacity) {
                int newCapacity = (nodeCapacity == 0) ? 10 : nodeCapacity * 2;
                wchar_t** newTags = (wchar_t**)realloc(nodeTags, newCapacity * sizeof(wchar_t*));
                if (!newTags) {
                    cJSON_Delete(root);
                    free(buffer);
                    CleanupDynamicNodes();
                    return FALSE;
                }
                nodeTags = newTags;
                nodeCapacity = newCapacity;
            }
            const char* utf8_str = tag->valuestring;
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, NULL, 0);
            nodeTags[nodeCount] = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
            if (nodeTags[nodeCount]) {
                MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, nodeTags[nodeCount], wideLen);
                nodeCount++;
            }
        }
    }
    cJSON* route = cJSON_GetObjectItem(root, "route");
    if (route) {
        cJSON* rules = cJSON_GetObjectItem(route, "rules");
        if (cJSON_IsArray(rules) && cJSON_GetArraySize(rules) > 0) {
            cJSON* first_rule = cJSON_GetArrayItem(rules, 0);
            if (first_rule) {
                cJSON* rule_outbound = cJSON_GetObjectItem(first_rule, "outbound");
                if (cJSON_IsString(rule_outbound) && rule_outbound->valuestring) {
                    MultiByteToWideChar(CP_UTF8, 0, rule_outbound->valuestring, -1, currentNode, ARRAYSIZE(currentNode));
                }
            }
        }
        if (currentNode[0] == L'\0') {
            cJSON* final_outbound = cJSON_GetObjectItem(route, "final");
            if (cJSON_IsString(final_outbound) && final_outbound->valuestring) {
                MultiByteToWideChar(CP_UTF8, 0, final_outbound->valuestring, -1, currentNode, ARRAYSIZE(currentNode));
            }
        }
    }
    cJSON* inbounds = cJSON_GetObjectItem(root, "inbounds");
    cJSON* inbound = NULL;
    cJSON_ArrayForEach(inbound, inbounds) {
        cJSON* type = cJSON_GetObjectItem(inbound, "type");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "http") == 0) {
            cJSON* listenPort = cJSON_GetObjectItem(inbound, "listen_port");
            if (cJSON_IsNumber(listenPort)) {
                httpPort = listenPort->valueint;
                break;
            }
        }
    }
    cJSON_Delete(root);
    free(buffer);
    return TRUE;
}

int GetHttpInboundPort() {
    return httpPort;
}

void StartSingBox() {
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    wchar_t cmdLine[MAX_PATH];
    wcsncpy(cmdLine, L"sing-box.exe run -c config.json", ARRAYSIZE(cmdLine));
    cmdLine[ARRAYSIZE(cmdLine) - 1] = L'\0';

    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        ShowError(L"核心程序启动失败", L"无法启动 sing-box.exe 核心程序。");
        ZeroMemory(&pi, sizeof(pi));
    }
}

void SwitchNode(const wchar_t* tag) {
    SafeReplaceOutbound(tag);
    wcsncpy(currentNode, tag, ARRAYSIZE(currentNode) - 1);
    currentNode[ARRAYSIZE(currentNode)-1] = L'\0';
    StopSingBox();
    StartSingBox();
    wchar_t message[256];
    wsprintfW(message, L"当前节点: %s", tag);
    ShowTrayTip(L"切换成功", message);
}

void SetSystemProxy(BOOL enable) {
    int port = GetHttpInboundPort();
    if (port == 0 && enable) {
        MessageBoxW(NULL, L"未找到HTTP入站端口，无法设置系统代理。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    INTERNET_PER_CONN_OPTION_LISTW list;
    INTERNET_PER_CONN_OPTIONW options[3];
    DWORD dwBufSize = sizeof(list);
    options[0].dwOption = INTERNET_PER_CONN_FLAGS;
    options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
    options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
    if (enable) {
        wchar_t proxyServer[64];
        wsprintfW(proxyServer, L"127.0.0.1:%d", port);
        options[0].Value.dwValue = PROXY_TYPE_PROXY;
        options[1].Value.pszValue = proxyServer;
        options[2].Value.pszValue = L"<local>";
    } else {
        options[0].Value.dwValue = PROXY_TYPE_DIRECT;
        options[1].Value.pszValue = L"";
        options[2].Value.pszValue = L"";
    }
    list.dwSize = sizeof(list);
    list.pszConnection = NULL;
    list.dwOptionCount = 3;
    list.dwOptionError = 0;
    list.pOptions = options;
    if (!InternetSetOptionW(NULL, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, dwBufSize)) {
        ShowError(L"代理设置失败", L"调用 InternetSetOptionW 失败。");
        return;
    }
    InternetSetOptionW(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
    InternetSetOptionW(NULL, INTERNET_OPTION_REFRESH, NULL, 0);
}

BOOL IsSystemProxyEnabled() {
    HKEY hKey;
    DWORD dwEnable = 0;
    DWORD dwSize = sizeof(dwEnable);
    wchar_t proxyServer[MAX_PATH] = {0};
    DWORD dwProxySize = sizeof(proxyServer);
    int port = GetHttpInboundPort();
    BOOL isEnabled = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_PROXY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"ProxyEnable", NULL, NULL, (LPBYTE)&dwEnable, &dwSize) == ERROR_SUCCESS) {
            if (dwEnable == 1) {
                if (port > 0) {
                    wchar_t expectedProxyServer[64];
                    wsprintfW(expectedProxyServer, L"127.0.0.1:%d", port);
                    if (RegQueryValueExW(hKey, L"ProxyServer", NULL, NULL, (LPBYTE)proxyServer, &dwProxySize) == ERROR_SUCCESS) {
                        if (wcscmp(proxyServer, expectedProxyServer) == 0) {
                            isEnabled = TRUE;
                        }
                    }
                }
            }
        }
        RegCloseKey(hKey);
    }
    return isEnabled;
}

void SafeReplaceOutbound(const wchar_t* newTag) {
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(L"config.json", &buffer, &size)) {
        MessageBoxW(NULL, L"无法打开 config.json", L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    int mbLen = WideCharToMultiByte(CP_UTF8, 0, newTag, -1, NULL, 0, NULL, NULL);
    char* newTagMb = (char*)malloc(mbLen);
    if (!newTagMb) {
        free(buffer);
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, newTag, -1, newTagMb, mbLen, NULL, NULL);
    cJSON* root = cJSON_Parse(buffer);
    if (!root) {
        free(buffer);
        free(newTagMb);
        return;
    }
    BOOL updated = FALSE;
    cJSON* route = cJSON_GetObjectItem(root, "route");
    if (route) {
        cJSON* rules = cJSON_GetObjectItem(route, "rules");
        if (cJSON_IsArray(rules) && cJSON_GetArraySize(rules) > 0) {
            cJSON* first_rule = cJSON_GetArrayItem(rules, 0);
            if (first_rule) {
                cJSON* rule_outbound = cJSON_GetObjectItem(first_rule, "outbound");
                if (rule_outbound) {
                    cJSON_SetValuestring(rule_outbound, newTagMb);
                    updated = TRUE;
                }
            }
        }
        if (!updated) {
            cJSON* final_outbound = cJSON_GetObjectItem(route, "final");
            if (final_outbound) {
                cJSON_SetValuestring(final_outbound, newTagMb);
            }
        }
    }
    char* newContent = cJSON_Print(root);
    if (newContent) {
        FILE* out = NULL;
        if (_wfopen_s(&out, L"config.json", L"wb") == 0 && out != NULL) {
            fwrite(newContent, 1, strlen(newContent), out);
            fclose(out);
        }
        free(newContent);
    }
    cJSON_Delete(root);
    free(buffer);
    free(newTagMb);
}

void UpdateMenu() {
    if (hMenu) DestroyMenu(hMenu);
    if (hNodeSubMenu) DestroyMenu(hNodeSubMenu);
    hMenu = CreatePopupMenu();
    hNodeSubMenu = CreatePopupMenu();
    for (int i = 0; i < nodeCount; ++i) {
        UINT flags = MF_STRING;
        if (wcscmp(nodeTags[i], currentNode) == 0) { flags |= MF_CHECKED; }
        AppendMenuW(hNodeSubMenu, flags, ID_TRAY_NODE_BASE + i, nodeTags[i]);
    }
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hNodeSubMenu, L"切换节点");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN_CONVERTER, L"节点转换");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_AUTORUN, L"开机启动");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SYSTEM_PROXY, L"系统代理");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"设置快捷键");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出");
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TRAY && lParam == WM_RBUTTONUP) {
        POINT pt; GetCursorPos(&pt); SetForegroundWindow(hWnd);
        ParseTags(); UpdateMenu();
        CheckMenuItem(hMenu, ID_TRAY_AUTORUN, IsAutorunEnabled() ? MF_CHECKED : MF_UNCHECKED);
        CheckMenuItem(hMenu, ID_TRAY_SYSTEM_PROXY, IsSystemProxyEnabled() ? MF_CHECKED : MF_UNCHECKED);
        TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
    } else if (msg == WM_COMMAND) {
        int id = LOWORD(wParam);
        if (id == ID_TRAY_EXIT) {
            UnregisterHotKey(hWnd, ID_GLOBAL_HOTKEY);
            if(g_isIconVisible) Shell_NotifyIconW(NIM_DELETE, &nid);
            if (IsSystemProxyEnabled()) SetSystemProxy(FALSE);
            StopSingBox(); CleanupDynamicNodes(); PostQuitMessage(0);
        } else if (id == ID_TRAY_AUTORUN) { SetAutorun(!IsAutorunEnabled());
        } else if (id == ID_TRAY_SYSTEM_PROXY) {
            BOOL isEnabled = IsSystemProxyEnabled();
            SetSystemProxy(!isEnabled);
            ShowTrayTip(L"系统代理", isEnabled ? L"系统代理已关闭" : L"系统代理已开启");
        } else if (id == ID_TRAY_OPEN_CONVERTER) { OpenConverterHtmlFromResource();
        } else if (id == ID_TRAY_SETTINGS) { OpenSettingsWindow();
        } else if (id >= ID_TRAY_NODE_BASE && id < ID_TRAY_NODE_BASE + nodeCount) {
            SwitchNode(nodeTags[id - ID_TRAY_NODE_BASE]);
        }
    } else if (msg == WM_HOTKEY) {
        if (wParam == ID_GLOBAL_HOTKEY) { ToggleTrayIconVisibility(); }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void StopSingBox() {
    if (pi.hProcess) {
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ZeroMemory(&pi, sizeof(pi));
    }
}

void SetAutorun(BOOL enable) {
    HKEY hKey;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (hKey) {
        if (enable) {
            RegSetValueExW(hKey, L"singbox_tray", 0, REG_SZ, (BYTE*)path, (wcslen(path) + 1) * sizeof(wchar_t));
        } else {
            RegDeleteValueW(hKey, L"singbox_tray");
        }
        RegCloseKey(hKey);
    }
}

BOOL IsAutorunEnabled() {
    HKEY hKey;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t value[MAX_PATH];
        DWORD size = sizeof(value);
        LONG res = RegQueryValueExW(hKey, L"singbox_tray", NULL, NULL, (LPBYTE)value, &size);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS && wcscmp(value, path) == 0);
    }
    return FALSE;
}

void OpenConverterHtmlFromResource() {
    HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_HTML_CONVERTER), RT_HTML);
    if (!hRes) {
        MessageBoxW(NULL, L"找不到HTML资源。请在使用ResourceHacker手动嵌入后重试。", L"资源错误", MB_OK|MB_ICONERROR);
        return;
    }
    HGLOBAL hMem = LoadResource(NULL, hRes);
    if (!hMem) return;
    void* pData = LockResource(hMem);
    DWORD dwSize = SizeofResource(NULL, hRes);
    if (!pData || dwSize == 0) return;
    wchar_t tempPath[MAX_PATH], tempFileName[MAX_PATH];
    GetTempPathW(ARRAYSIZE(tempPath), tempPath);
    GetTempFileNameW(tempPath, L"sbx", 0, tempFileName);
    wchar_t* dot = wcsrchr(tempFileName, L'.');
    if (dot) {
        wcsncpy(dot, L".html", (size_t)(tempFileName + ARRAYSIZE(tempFileName) - dot));
    } else {
        wcsncat(tempFileName, L".html", ARRAYSIZE(tempFileName) - wcslen(tempFileName) - 1);
    }
    FILE* f = NULL;
    if (_wfopen_s(&f, tempFileName, L"wb") == 0 && f != NULL) {
        fwrite(pData, 1, dwSize, f);
        fclose(f);
        ShellExecuteW(NULL, L"open", tempFileName, NULL, NULL, SW_SHOWNORMAL);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    hMutex = CreateMutexW(NULL, TRUE, L"Global\\SingBoxTrayMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"程序已在运行。", L"提示", MB_OK | MB_ICONINFORMATION);
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_HOTKEY_CLASS;
    InitCommonControlsEx(&icex);
    wchar_t szPath[MAX_PATH];
    GetModuleFileNameW(NULL, szPath, MAX_PATH);
    wchar_t* p = wcsrchr(szPath, L'\\');
    if (p) {
        *p = L'\0';
        SetCurrentDirectoryW(szPath);
        wcsncpy(g_iniFilePath, szPath, MAX_PATH - 1);
        g_iniFilePath[MAX_PATH - 1] = L'\0';
        wcsncat(g_iniFilePath, L"\\set.ini", MAX_PATH - wcslen(g_iniFilePath) - 1);
    } else {
        wcsncpy(g_iniFilePath, L"set.ini", MAX_PATH - 1);
    }
    LoadSettings();
    if (!ParseTags()) {
        MessageBoxW(NULL, L"无法读取或解析 config.json 文件。", L"错误", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }
    const wchar_t* CLASS_NAME = L"TrayWindowClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(1));
    if (!wc.hIcon) {
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    }
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(0, CLASS_NAME, L"TrayApp", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!hwnd) return 1;
    if (g_hotkeyVk != 0 || g_hotkeyModifiers != 0) {
        RegisterHotKey(hwnd, ID_GLOBAL_HOTKEY, g_hotkeyModifiers, g_hotkeyVk);
    }
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = wc.hIcon;
    wcsncpy(nid.szTip, L"程序正在运行...", ARRAYSIZE(nid.szTip) - 1);
    nid.szTip[ARRAYSIZE(nid.szTip) - 1] = L'\0';
    if (g_isIconVisible) {
        Shell_NotifyIconW(NIM_ADD, &nid);
    }
    StartSingBox();
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_isIconVisible) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    CleanupDynamicNodes();
    if (hMutex) CloseHandle(hMutex);
    UnregisterClassW(CLASS_NAME, hInstance);
    return (int)msg.wParam;
}