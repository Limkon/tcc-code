#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>     // For FILE, fopen, fclose, fseek, ftell, fread, sscanf
#include <stdlib.h>    // For malloc, free
#include <string.h>    // For strstr, strchr, strncpy, strlen, strcpy
#include <wchar.h>     // For wcscpy, wcslen, MultiByteToWideChar, WideCharToMultiByte, swprintf, wcsrchr (for _snwprintf)
#include <wininet.h> // For InternetSetOptionW

#define WM_TRAY (WM_USER + 1)
#define ID_TRAY_EXIT 1001
#define ID_TRAY_AUTORUN 1002
#define ID_TRAY_SYSTEM_PROXY 1003 // System Proxy menu item ID

// 新增：打开节点转换器菜单项ID
#define ID_TRAY_OPEN_CONVERTER 1004

#define ID_TRAY_NODE_BASE 2000    // Base ID for dynamic node menu items

// 宏定义放置在文件顶部，使用 #ifndef 确保只定义一次
// 这是针对“RT_HTML redefined”警告的防御性措施
#ifndef IDR_HTML_CONVERTER
#define IDR_HTML_CONVERTER L"CONVERTER_HTML" // Resource Hacker中定义的资源名称
#endif

#ifndef RT_HTML
#define RT_HTML            L"HTML"          // Resource Hacker中定义的资源类型
#endif

// Global variables
NOTIFYICONDATAW nid;
HWND hwnd;
HMENU hMenu, hNodeSubMenu;
HANDLE hMutex = NULL;
PROCESS_INFORMATION pi; // Global PROCESS_INFORMATION to track the sing-box process

wchar_t currentNode[64] = L""; // Stores the currently active node tag
int nodeCount = 0; // Number of discovered nodes
wchar_t nodeTags[10][64]; // Array to store node tags (max 10 nodes, max 64 chars per tag)

// Registry key for system proxy settings (Internet Explorer settings)
#define REG_PATH_PROXY L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings"
#define REG_VALUE_PROXY_ENABLE L"ProxyEnable"
#define REG_VALUE_PROXY_SERVER L"ProxyServer"
#define REG_VALUE_PROXY_OVERRIDE L"ProxyOverride"

// Function declarations
BOOL IsAutorunEnabled();
void SetAutorun(BOOL enable);
void StartSingBox();
void StopSingBox();
void SafeReplaceOutbound(const wchar_t* newTagW);
void SwitchNode(const wchar_t* tag);
BOOL ParseTags();
int GetHttpInboundPort();
void SetSystemProxy(BOOL enable);
BOOL IsSystemProxyEnabled();
void UpdateMenu();
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// --- 新增功能函数：从资源中读取 HTML 并打开 ---
void OpenConverterHtmlFromResource() {
    WCHAR tempPath[MAX_PATH];
    WCHAR tempFileName[MAX_PATH];
    FILE* f = NULL;
    void* pData = NULL; // 指向资源数据的指针
    DWORD dwSize = 0;   // 资源数据的大小

    // 1. 查找并获取嵌入的 HTML 资源句柄
    HRSRC hRes = FindResourceW(NULL, IDR_HTML_CONVERTER, RT_HTML);
    if (!hRes) {
        MessageBoxW(NULL, L"错误：未找到嵌入的 HTML 资源！请检查EXE文件和资源名称/类型。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    // 2. 加载资源到内存
    HGLOBAL hMem = LoadResource(NULL, hRes);
    if (!hMem) {
        MessageBoxW(NULL, L"错误：加载 HTML 资源失败！", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    // 3. 锁定资源并获取数据指针及大小
    pData = LockResource(hMem);
    dwSize = SizeofResource(NULL, hRes);

    if (!pData || dwSize == 0) {
        MessageBoxW(NULL, L"错误：获取 HTML 资源数据失败（数据为空或指针无效）！", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    // 4. 获取系统临时文件路径和创建一个唯一的临时文件名
    if (GetTempPathW(ARRAYSIZE(tempPath), tempPath) == 0) {
        MessageBoxW(NULL, L"错误：无法获取临时路径。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    if (GetTempFileNameW(tempPath, L"sbx", 0, tempFileName) == 0) {
        MessageBoxW(NULL, L"错误：无法创建临时文件名。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    // 将临时文件的后缀名从 .tmp 修改为 .html，这样浏览器才能正确识别
    WCHAR* dot = wcsrchr(tempFileName, L'.');
    if (dot) {
        wcscpy(dot, L".html"); // 替换为 wcscpy，并假设目标缓冲区足够大
    } else {
        wcscat(tempFileName, L".html"); // 替换为 wcscat，并假设目标缓冲区足够大
    }

    // 5. 将内存中的 HTML 内容写入到这个临时文件
    f = _wfopen(tempFileName, L"wb");
    if (f) {
        fwrite(pData, 1, dwSize, f);
        fclose(f);

        // 6. 使用默认浏览器打开临时 HTML 文件
        ShellExecuteW(NULL, L"open", tempFileName, NULL, NULL, SW_SHOWNORMAL);
    } else {
        MessageBoxW(NULL, L"错误：无法写入临时 HTML 文件到磁盘。", L"错误", MB_OK | MB_ICONERROR);
    }
}
// --- 结束新增功能函数 ---


// Function to check if the application is set to autorun on startup
BOOL IsAutorunEnabled() {
    HKEY hKey;
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                     0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR value[MAX_PATH];
        DWORD size = sizeof(value);
        LONG res = RegQueryValueExW(hKey, L"singbox_tray", NULL, NULL, (LPBYTE)value, &size);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS && wcscmp(value, path) == 0);
    }
    return FALSE;
}

// Function to enable or disable autorun
void SetAutorun(BOOL enable) {
    HKEY hKey;
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);

    RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                    0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (hKey) {
        if (enable) {
            RegSetValueExW(hKey, L"singbox_tray", 0, REG_SZ, (BYTE*)path,
                           (lstrlenW(path) + 1) * sizeof(WCHAR));
        } else {
            RegDeleteValueW(hKey, L"singbox_tray");
        }
        RegCloseKey(hKey);
    }
}

// Function to start the sing-box process
void StartSingBox() {
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    WCHAR cmdLine[MAX_PATH];
    wcscpy(cmdLine, L"sing-box.exe run -c config.json"); // 替换为 wcscpy
    // 注意：wcscpy 不进行边界检查，确保 cmdLine 缓冲区足够大

    BOOL success = CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE,
                                  CREATE_NO_WINDOW | CREATE_NEW_CONSOLE,
                                  NULL, NULL, &si, &pi);

    if (!success) {
        MessageBoxW(NULL, L"启动 sing-box 失败！请确保 sing-box.exe 和 config.json 存在于同一目录下。", L"错误", MB_OK | MB_ICONERROR);
        ZeroMemory(&pi, sizeof(pi));
    }
}

// Function to stop the sing-box process
void StopSingBox() {
    if (pi.hProcess) {
        if (TerminateProcess(pi.hProcess, 0)) {
              WaitForSingleObject(pi.hProcess, 5000);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ZeroMemory(&pi, sizeof(pi));
    }
}

// Corrected function to safely replace the "outbound" tag in config.json
void SafeReplaceOutbound(const wchar_t* newTagW) {
    FILE* f = _wfopen(L"config.json", L"rb");
    if (!f) {
        MessageBoxW(NULL, L"无法打开 config.json 文件进行读取。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
        MessageBoxW(NULL, L"内存分配失败。", L"错误", MB_OK | MB_ICONERROR);
        fclose(f);
        return;
    }
    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);

    int newTagLenMb = WideCharToMultiByte(CP_UTF8, 0, newTagW, -1, NULL, 0, NULL, NULL);
    char* newTagMb = (char*)malloc(newTagLenMb);
    if (!newTagMb) {
        MessageBoxW(NULL, L"内存分配失败。", L"错误", MB_OK | MB_ICONERROR);
        free(buffer);
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, newTagW, -1, newTagMb, newTagLenMb, NULL, NULL);

    char* outboundKeyPos = strstr(buffer, "\"outbound\":");
    if (!outboundKeyPos) {
        MessageBoxW(NULL, L"在 config.json 中未找到 \"outbound\" 键。", L"错误", MB_OK | MB_ICONERROR);
        free(buffer);
        free(newTagMb);
        return;
    }

    char* currentTagStartQuote = strchr(outboundKeyPos + strlen("\"outbound\":"), '"');
    if (!currentTagStartQuote) {
        MessageBoxW(NULL, L"在 \"outbound\" 键后未找到起始引号。", L"错误", MB_OK | MB_ICONERROR);
        free(buffer);
        free(newTagMb);
        return;
    }
    currentTagStartQuote++;

    char* currentTagEndQuote = strchr(currentTagStartQuote, '"');
    if (!currentTagEndQuote) {
        MessageBoxW(NULL, L"在 \"outbound\" 键后未找到结束引号。", L"错误", MB_OK | MB_ICONERROR);
        free(buffer);
        free(newTagMb);
        return;
    }

    long prefixLen = currentTagStartQuote - buffer;
    long suffixLen = size - (currentTagEndQuote - buffer);

    long newBufferSize = prefixLen + strlen(newTagMb) + suffixLen + 1;
    char* newBuffer = (char*)malloc(newBufferSize);
    if (!newBuffer) {
        MessageBoxW(NULL, L"内存分配失败。", L"错误", MB_OK | MB_ICONERROR);
        free(buffer);
        free(newTagMb);
        return;
    }

    memcpy(newBuffer, buffer, prefixLen);
    newBuffer[prefixLen] = '\0';

    strcat(newBuffer, newTagMb); // 替换为 strcat
    strcat(newBuffer, currentTagEndQuote); // 替换为 strcat
    // 注意：strcat 不进行边界检查，确保 newBuffer 足够大

    FILE* out = _wfopen(L"config.json", L"wb");
    if (!out) {
        MessageBoxW(NULL, L"无法打开 config.json 文件进行写入。", L"错误", MB_OK | MB_ICONERROR);
        free(buffer);
        free(newTagMb);
        free(newBuffer);
        return;
    }

    fwrite(newBuffer, 1, strlen(newBuffer), out);
    fclose(out);

    free(buffer);
    free(newTagMb);
    free(newBuffer);
}

// Function to switch the active node
void SwitchNode(const wchar_t* tag) {
    SafeReplaceOutbound(tag);
    wcscpy(currentNode, tag); // 替换为 wcscpy
    // 注意：wcscpy 不进行边界检查，确保 currentNode 缓冲区足够大
    StopSingBox();
    StartSingBox();
}

// Function to parse node tags from config.json
BOOL ParseTags() {
    nodeCount = 0;
    FILE* f = _wfopen(L"config.json", L"rb");
    if (!f) {
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
        fclose(f);
        return FALSE;
    }
    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);

    char* outbounds = strstr(buffer, "\"outbounds\":");
    if (outbounds) {
        char* pos = outbounds;
        while ((pos = strstr(pos, "\"tag\": \"")) != NULL && nodeCount < ARRAYSIZE(nodeTags)) {
            pos += 8;
            char* end = strchr(pos, '"');
            if (end && (end - pos < ARRAYSIZE(nodeTags[0]))) {
                char temp[ARRAYSIZE(nodeTags[0])];
                strncpy(temp, pos, end - pos); // 替换为 strncpy
                temp[end - pos] = '\0'; // strncpy 不会自动添加空终止符，所以这一行很重要
                MultiByteToWideChar(CP_UTF8, 0, temp, -1, nodeTags[nodeCount], ARRAYSIZE(nodeTags[0]));
                nodeCount++;
            }
            pos = end;
        }
    }

    char* route_section = strstr(buffer, "\"route\":");
    if (route_section) {
        char* pos = strstr(route_section, "\"outbound\": \"");
        if (pos) {
            pos += 13;
            char* end = strchr(pos, '"');
            if (end && (end - pos < ARRAYSIZE(currentNode))) {
                char temp[ARRAYSIZE(currentNode)];
                strncpy(temp, pos, end - pos); // 替换为 strncpy
                temp[end - pos] = '\0'; // strncpy 不会自动添加空终止符，所以这一行很重要
                MultiByteToWideChar(CP_UTF8, 0, temp, -1, currentNode, ARRAYSIZE(currentNode));
            }
        }
    }

    free(buffer);
    return TRUE;
}

// Function to read the HTTP inbound listen_port from config.json
int GetHttpInboundPort() {
    int port = 0;
    FILE* f = _wfopen(L"config.json", L"rb");
    if (!f) {
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
        MessageBoxW(NULL, L"内存分配失败。", L"错误", MB_OK | MB_ICONERROR);
        fclose(f);
        return 0;
    }
    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);

    char* inbounds_arr_start = strstr(buffer, "\"inbounds\":");
    if (inbounds_arr_start) {
        char* arr_open_bracket = strchr(inbounds_arr_start, '[');
        if (arr_open_bracket) {
            char* current_pos = arr_open_bracket;
            int brace_count = 0;

            while ((current_pos = strchr(current_pos + 1, '{')) != NULL) {
                brace_count = 1;
                char* search_end_for_object = current_pos;

                while (brace_count > 0 && search_end_for_object < buffer + size) {
                    search_end_for_object++;
                    if (*search_end_for_object == '{') brace_count++;
                    else if (*search_end_for_object == '}') brace_count--;
                }

                if (brace_count == 0 && search_end_for_object > current_pos) {
                    char* type_key_pos = strstr(current_pos, "\"type\": \"http\"");
                    if (type_key_pos && type_key_pos < search_end_for_object) {
                        char* port_key_pos = strstr(current_pos, "\"listen_port\":");
                        if (port_key_pos && port_key_pos < search_end_for_object) {
                            port_key_pos += strlen("\"listen_port\":");
                            if (sscanf(port_key_pos, "%d", &port) == 1) {
                                free(buffer);
                                return port;
                            }
                        }
                    }
                }
                current_pos = search_end_for_object;
            }
        }
    }

    free(buffer);
    return 0;
}

// Function to set or unset system proxy
void SetSystemProxy(BOOL enable) {
    HKEY hKey;
    DWORD dwEnable = enable ? 1 : 0;
    int port = GetHttpInboundPort();
    WCHAR proxyServer[64];

    if (port == 0 && enable) {
        MessageBoxW(NULL, L"在 config.json 中未找到 HTTP 入站端口，无法设置系统代理。\n请确保 inbounds 配置了 \"type\": \"http\" 和 \"listen_port\"。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    if (enable) {
        _snwprintf(proxyServer, ARRAYSIZE(proxyServer), L"127.0.0.1:%d", port); // 替换为 _snwprintf
    } else {
        proxyServer[0] = L'\0';
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_PROXY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, REG_VALUE_PROXY_ENABLE, 0, REG_DWORD, (const BYTE*)&dwEnable, sizeof(dwEnable));

        if (enable) {
            RegSetValueExW(hKey, REG_VALUE_PROXY_SERVER, 0, REG_SZ, (const BYTE*)proxyServer, (wcslen(proxyServer) + 1) * sizeof(WCHAR));
            RegSetValueExW(hKey, REG_VALUE_PROXY_OVERRIDE, 0, REG_SZ, (const BYTE*)L"<local>", (wcslen(L"<local>") + 1) * sizeof(WCHAR));
        } else {
            RegDeleteValueW(hKey, REG_VALUE_PROXY_SERVER);
            RegDeleteValueW(hKey, REG_VALUE_PROXY_OVERRIDE);
        }
        RegCloseKey(hKey);

        InternetSetOptionW(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
        InternetSetOptionW(NULL, INTERNET_OPTION_REFRESH, NULL, 0);
    } else {
        MessageBoxW(NULL, L"无法打开注册表以设置系统代理。", L"错误", MB_OK | MB_ICONERROR);
    }
}

// Function to check if system proxy is currently enabled by sing-box_tray
BOOL IsSystemProxyEnabled() {
    HKEY hKey;
    DWORD dwEnable = 0;
    DWORD dwSize = sizeof(dwEnable);
    WCHAR proxyServer[MAX_PATH];
    DWORD dwProxySize = sizeof(proxyServer);
    int port = GetHttpInboundPort();
    WCHAR expectedProxyServer[64];
    _snwprintf(expectedProxyServer, ARRAYSIZE(expectedProxyServer), L"127.0.0.1:%d", port); // 替换为 _snwprintf

    dwProxySize = sizeof(proxyServer);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_PROXY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        LONG res1 = RegQueryValueExW(hKey, REG_VALUE_PROXY_ENABLE, NULL, NULL, (LPBYTE)&dwEnable, &dwSize);
        LONG res2 = RegQueryValueExW(hKey, REG_VALUE_PROXY_SERVER, NULL, NULL, (LPBYTE)proxyServer, &dwProxySize);
        RegCloseKey(hKey);

        if (port > 0) {
            return (res1 == ERROR_SUCCESS && dwEnable == 1 &&
                                            res2 == ERROR_SUCCESS && wcscmp(proxyServer, expectedProxyServer) == 0);
        }
    }
    return FALSE;
}

// Function to update the tray icon context menu
void UpdateMenu() {
    if (hMenu) DestroyMenu(hMenu);
    if (hNodeSubMenu) DestroyMenu(hNodeSubMenu);

    hMenu = CreatePopupMenu();
    hNodeSubMenu = CreatePopupMenu();

    for (int i = 0; i < nodeCount; i++) {
        UINT flags = MF_STRING;
        if (wcscmp(nodeTags[i], currentNode) == 0) {
            flags |= MF_CHECKED;
        }
        AppendMenuW(hNodeSubMenu, flags, ID_TRAY_NODE_BASE + i, nodeTags[i]);
    }
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hNodeSubMenu, L"切换节点");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    // 新增：打开节点转换器菜单项
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN_CONVERTER, L"节点转换");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    AppendMenuW(hMenu, MF_STRING, ID_TRAY_AUTORUN, L"开机启动");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SYSTEM_PROXY, L"系统代理");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出");
}

// Window procedure to handle messages
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TRAY && lParam == WM_RBUTTONUP) {
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hWnd);

        ParseTags();
        UpdateMenu();

        CheckMenuItem(hMenu, ID_TRAY_AUTORUN, IsAutorunEnabled() ? MF_CHECKED : MF_UNCHECKED);
        CheckMenuItem(hMenu, ID_TRAY_SYSTEM_PROXY, IsSystemProxyEnabled() ? MF_CHECKED : MF_UNCHECKED);

        TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
    }
    else if (msg == WM_COMMAND) {
        int id = LOWORD(wParam);
        if (id == ID_TRAY_EXIT) {
            Shell_NotifyIconW(NIM_DELETE, &nid);
            StopSingBox();

            if (IsSystemProxyEnabled()) {
                SetSystemProxy(FALSE);
            }
            PostQuitMessage(0);
        } else if (id == ID_TRAY_AUTORUN) {
            SetAutorun(!IsAutorunEnabled());
        } else if (id == ID_TRAY_SYSTEM_PROXY) {
            SetSystemProxy(!IsSystemProxyEnabled());
        } else if (id == ID_TRAY_OPEN_CONVERTER) { // 处理打开节点转换器菜单项
            OpenConverterHtmlFromResource();
        } else if (id >= ID_TRAY_NODE_BASE && id < ID_TRAY_NODE_BASE + nodeCount) {
            SwitchNode(nodeTags[id - ID_TRAY_NODE_BASE]);
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Entry point for the Windows application
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    // Ensure only one instance of the application is running
    hMutex = CreateMutexW(NULL, TRUE, L"Global\\SingBoxTrayMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"sing-box 托盘程序已在运行。", L"提示", MB_OK | MB_ICONINFORMATION);
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // --- 设置程序工作目录 ---
    WCHAR szPath[MAX_PATH];
    GetModuleFileNameW(NULL, szPath, MAX_PATH);

    WCHAR* p = wcsrchr(szPath, L'\\');
    if (p != NULL) {
        *p = L'\0';
        if (!SetCurrentDirectoryW(szPath)) {
            MessageBoxW(NULL, L"无法设置程序工作目录。请确保程序拥有访问其所在目录的权限。", L"错误", MB_OK | MB_ICONERROR);
            if (hMutex) CloseHandle(hMutex);
            return 1;
        }
    } else {
        MessageBoxW(NULL, L"无法获取程序所在目录。请将 sing-box-tray.exe 放置在一个有效的目录下。", L"错误", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }
    // --- 工作目录设置结束 ---

    ZeroMemory(&pi, sizeof(pi));

    // Attempt to parse tags with retries
    int retry_count = 0;
    const int MAX_RETRIES = 3;
    const int RETRY_DELAY_MS = 10000; // 10 seconds

    while (retry_count < MAX_RETRIES) {
        if (ParseTags()) {
            break;
        }
        retry_count++;
        if (retry_count < MAX_RETRIES) {
            Sleep(RETRY_DELAY_MS);
        }
    }

    if (retry_count == MAX_RETRIES && !ParseTags()) {
        MessageBoxW(NULL, L"无法读取 config.json 文件，请确保其存在且格式正确。程序将退出。", L"错误", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // Register window class
    const wchar_t *CLASS_NAME = L"TrayWindowClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(1));
    if (!wc.hIcon) {
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    }

    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"注册窗口类失败！", L"错误", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // Create a hidden window
    hwnd = CreateWindowExW(0, CLASS_NAME, L"TrayApp", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        MessageBoxW(NULL, L"创建窗口失败！", L"错误", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // Initialize NOTIFYICONDATA structure for the tray icon
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = wc.hIcon;
    lstrcpyW(nid.szTip, L"sing-box 正在运行");

    // Add the tray icon to the notification area
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        MessageBoxW(NULL, L"添加托盘图标失败！", L"错误", MB_OK | MB_ICONERROR);
        DestroyWindow(hwnd);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    StartSingBox();

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup on exit
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (hMutex) CloseHandle(hMutex);
    UnregisterClassW(CLASS_NAME, hInstance);
    return (int)msg.wParam;
}