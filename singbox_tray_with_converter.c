#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>    // For FILE, fopen, fclose, fseek, ftell, fread, sscanf
#include <stdlib.h>   // For malloc, free, realloc
#include <string.h>   // For strstr, strchr, strncpy, strlen, strcpy
#include <wchar.h>    // For wcscpy, wcslen, MultiByteToWideChar, WideCharToMultiByte, swprintf, wcsrchr
#include <wininet.h>  // For InternetSetOptionW
#include <tchar.h>    // 为了 _tcscpy_s 等安全函数

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
#define IDR_HTML_CONVERTER 2 // 资源名称修改为整数 2
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

// --- 动态节点数组 ---
wchar_t** nodeTags = NULL; // 指向节点标签字符串指针数组的指针
int nodeCount = 0;         // 发现的节点数量
int nodeCapacity = 0;      // 节点数组的当前容量

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
void OpenConverterHtmlFromResource();
void CleanupDynamicNodes();


// --- 新增功能函数：从资源中读取 HTML 并打开 ---
void OpenConverterHtmlFromResource() {
    WCHAR tempPath[MAX_PATH];
    WCHAR tempFileName[MAX_PATH];
    FILE* f = NULL;
    void* pData = NULL; // 指向资源数据的指针
    DWORD dwSize = 0;   // 资源数据的大小

    // 1. 查找并获取嵌入的 HTML 资源句柄
    // 使用 MAKEINTRESOURCEW 宏来处理整数类型的资源ID
    HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_HTML_CONVERTER), RT_HTML);
    if (!hRes) {
        MessageBoxW(NULL, L"错误：未找到嵌入的 HTML 资源！请检查EXE文件和资源ID/类型。", L"错误", MB_OK | MB_ICONERROR);
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
        // 注意：LoadResource/LockResource 获取的资源不需要显式释放
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
        // 使用更安全的 wcscpy_s
        wcscpy_s(dot, (size_t)(tempFileName + ARRAYSIZE(tempFileName) - dot), L".html");
    } else {
        // 使用更安全的 wcscat_s
        wcscat_s(tempFileName, ARRAYSIZE(tempFileName), L".html");
    }

    // 5. 将内存中的 HTML 内容写入到这个临时文件
    // 使用更安全的 _wfopen_s
    if (_wfopen_s(&f, tempFileName, L"wb") == 0 && f != NULL) {
        fwrite(pData, 1, dwSize, f);
        fclose(f);

        // 6. 使用默认浏览器打开临时 HTML 文件
        ShellExecuteW(NULL, L"open", tempFileName, NULL, NULL, SW_SHOWNORMAL);
    } else {
        MessageBoxW(NULL, L"错误：无法写入临时 HTML 文件到磁盘。", L"错误", MB_OK | MB_ICONERROR);
    }
    // FreeResource 不是必需的，因为系统会自动处理
}

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
    // 使用更安全的 wcscpy_s
    wcscpy_s(cmdLine, ARRAYSIZE(cmdLine), L"sing-box.exe run -c config.json");

    BOOL success = CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE,
                                  CREATE_NO_WINDOW, // CREATE_NEW_CONSOLE 会闪现一个窗口，CREATE_NO_WINDOW 更彻底
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
    FILE* f = NULL;
    // 使用更安全的 _wfopen_s
    if (_wfopen_s(&f, L"config.json", L"rb") != 0 || !f) {
        MessageBoxW(NULL, L"无法打开 config.json 文件进行读取。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0) { // 增加对空文件的检查
        fclose(f);
        MessageBoxW(NULL, L"config.json 文件为空或读取大小错误。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

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

    size_t prefixLen = currentTagStartQuote - buffer;
    size_t newTagMbLen = strlen(newTagMb);
    const char* suffixStart = currentTagEndQuote;

    size_t newBufferSize = prefixLen + newTagMbLen + strlen(suffixStart) + 1;
    char* newBuffer = (char*)malloc(newBufferSize);
    if (!newBuffer) {
        MessageBoxW(NULL, L"内存分配失败。", L"错误", MB_OK | MB_ICONERROR);
        free(buffer);
        free(newTagMb);
        return;
    }

    // 使用更高效和安全的内存操作构建新内容
    memcpy(newBuffer, buffer, prefixLen);
    memcpy(newBuffer + prefixLen, newTagMb, newTagMbLen);
    // 使用 strcpy_s 替代 strcat 以复制剩余部分
    strcpy_s(newBuffer + prefixLen + newTagMbLen, newBufferSize - (prefixLen + newTagMbLen), suffixStart);

    FILE* out = NULL;
    // 使用更安全的 _wfopen_s
    if (_wfopen_s(&out, L"config.json", L"wb") != 0 || !out) {
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
    // 使用更安全的 wcscpy_s
    wcscpy_s(currentNode, ARRAYSIZE(currentNode), tag);
    StopSingBox();
    StartSingBox();
}

// Function to parse node tags from config.json
BOOL ParseTags() {
    // 每次解析前，清理旧的动态数组
    CleanupDynamicNodes();

    FILE* f = NULL;
    // 使用更安全的 _wfopen_s
    if (_wfopen_s(&f, L"config.json", L"rb") != 0 || !f) {
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) { // 增加对空文件的检查
        fclose(f);
        return FALSE;
    }

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
        while ((pos = strstr(pos, "\"tag\": \"")) != NULL) {
            // 检查容量是否足够
            if (nodeCount >= nodeCapacity) {
                // 容量不足，进行扩容（通常是翻倍）
                int newCapacity = (nodeCapacity == 0) ? 10 : nodeCapacity * 2;
                wchar_t** newTags = (wchar_t**)realloc(nodeTags, newCapacity * sizeof(wchar_t*));
                if (!newTags) {
                    // realloc 失败，内存不足
                    MessageBoxW(NULL, L"为节点列表扩容失败，内存不足。", L"错误", MB_OK | MB_ICONERROR);
                    free(buffer);
                    // 清理已分配的部分
                    CleanupDynamicNodes();
                    return FALSE;
                }
                nodeTags = newTags;
                nodeCapacity = newCapacity;
            }
            
            pos += 8; // strlen("\"tag\": \"")
            char* end = strchr(pos, '"');
            if (end) {
                // 提取标签（多字节UTF-8）
                size_t tagLen = end - pos;
                char* tempTagMb = (char*)malloc(tagLen + 1);
                if (!tempTagMb) { /* 错误处理 */ free(buffer); CleanupDynamicNodes(); return FALSE; }
                
                strncpy_s(tempTagMb, tagLen + 1, pos, tagLen);

                // 将UTF-8转换为宽字符
                int wideLen = MultiByteToWideChar(CP_UTF8, 0, tempTagMb, -1, NULL, 0);
                nodeTags[nodeCount] = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
                 if (!nodeTags[nodeCount]) { /* 错误处理 */ free(tempTagMb); free(buffer); CleanupDynamicNodes(); return FALSE; }

                MultiByteToWideChar(CP_UTF8, 0, tempTagMb, -1, nodeTags[nodeCount], wideLen);
                
                free(tempTagMb);
                nodeCount++;
            } else {
                 break; // 格式错误，找不到结束引号
            }

            pos = end;
        }
    }

    char* route_section = strstr(buffer, "\"route\":");
    if (route_section) {
        char* pos = strstr(route_section, "\"outbound\": \"");
        if (pos) {
            pos += 13; // strlen("\"outbound\": \"")
            char* end = strchr(pos, '"');
            if (end && (end - pos < (ptrdiff_t)ARRAYSIZE(currentNode))) {
                char temp[ARRAYSIZE(currentNode)];
                // 使用更安全的 strncpy_s
                strncpy_s(temp, ARRAYSIZE(temp), pos, end - pos);
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
    FILE* f = NULL;
    // 使用更安全的 _wfopen_s
    if (_wfopen_s(&f, L"config.json", L"rb") != 0 || !f) {
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) { // 增加对空文件的检查
        fclose(f);
        return 0;
    }

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
            
            while ((current_pos = strchr(current_pos, '{')) != NULL) {
                 char* object_start = current_pos;
                 char* object_end = strchr(object_start, '}'); // 简化查找，假设没有嵌套对象

                 if (!object_end) break; // 找不到配对的 '}'，退出循环

                 // 在当前 { ... } 对象范围内查找
                 char* type_key_pos = strstr(object_start, "\"type\": \"http\"");
                 if (type_key_pos && type_key_pos < object_end) {
                     char* port_key_pos = strstr(object_start, "\"listen_port\":");
                     if (port_key_pos && port_key_pos < object_end) {
                         port_key_pos += strlen("\"listen_port\":");
                         // 使用更安全的 sscanf_s
                         if (sscanf_s(port_key_pos, " %d", &port) == 1) {
                             free(buffer);
                             return port;
                         }
                     }
                 }
                 current_pos = object_end;
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
        // 使用更安全的 swprintf_s
        swprintf_s(proxyServer, ARRAYSIZE(proxyServer), L"127.0.0.1:%d", port);
    } else {
        proxyServer[0] = L'\0';
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_PROXY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, REG_VALUE_PROXY_ENABLE, 0, REG_DWORD, (const BYTE*)&dwEnable, sizeof(dwEnable));

        if (enable) {
            RegSetValueExW(hKey, REG_VALUE_PROXY_SERVER, 0, REG_SZ, (const BYTE*)proxyServer, (wcslen(proxyServer) + 1) * sizeof(WCHAR));
            RegSetValueExW(hKey, REG_VALUE_PROXY_OVERRIDE, 0, REG_SZ, (const BYTE*)L"<local>", (wcslen(L"<local>") + 1) * sizeof(WCHAR));
        } else {
            // 清理时最好使用 RegDeleteValueW
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

    if (port <= 0) return FALSE; // 如果没有有效端口，代理不可能被我们启用

    WCHAR expectedProxyServer[64];
    // 使用更安全的 swprintf_s
    swprintf_s(expectedProxyServer, ARRAYSIZE(expectedProxyServer), L"127.0.0.1:%d", port);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_PROXY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        LONG res1 = RegQueryValueExW(hKey, REG_VALUE_PROXY_ENABLE, NULL, NULL, (LPBYTE)&dwEnable, &dwSize);
        LONG res2 = RegQueryValueExW(hKey, REG_VALUE_PROXY_SERVER, NULL, NULL, (LPBYTE)proxyServer, &dwProxySize);
        RegCloseKey(hKey);

        return (res1 == ERROR_SUCCESS && dwEnable == 1 &&
                res2 == ERROR_SUCCESS && wcscmp(proxyServer, expectedProxyServer) == 0);
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
            // 在退出前清理动态分配的内存
            CleanupDynamicNodes();
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

// 新增：清理动态节点数组内存的函数
void CleanupDynamicNodes() {
    if (nodeTags) {
        for (int i = 0; i < nodeCount; i++) {
            if (nodeTags[i]) {
                free(nodeTags[i]);
                nodeTags[i] = NULL;
            }
        }
        free(nodeTags);
        nodeTags = NULL;
    }
    nodeCount = 0;
    nodeCapacity = 0;
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

    // 尝试解析标签，如果失败则直接退出
    if (!ParseTags()) {
        MessageBoxW(NULL, L"无法读取或解析 config.json 文件，请确保其存在且格式正确。程序将退出。", L"错误", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        CleanupDynamicNodes(); // 确保即使启动失败也清理内存
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
        CleanupDynamicNodes();
        return 1;
    }

    // Create a hidden window
    hwnd = CreateWindowExW(0, CLASS_NAME, L"TrayApp", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        MessageBoxW(NULL, L"创建窗口失败！", L"错误", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        CleanupDynamicNodes();
        return 1;
    }

    // Initialize NOTIFYICONDATA structure for the tray icon
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = wc.hIcon;
    // 使用更安全的 wcscpy_s
    wcscpy_s(nid.szTip, ARRAYSIZE(nid.szTip), L"sing-box 正在运行");

    // Add the tray icon to the notification area
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        MessageBoxW(NULL, L"添加托盘图标失败！", L"错误", MB_OK | MB_ICONERROR);
        DestroyWindow(hwnd);
        if (hMutex) CloseHandle(hMutex);
        CleanupDynamicNodes();
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
    CleanupDynamicNodes(); // 在主循环结束后也调用清理，确保万无一失
    if (hMutex) CloseHandle(hMutex);
    UnregisterClassW(CLASS_NAME, hInstance);
    return (int)msg.wParam;
}
