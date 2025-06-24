#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>    // For FILE, fopen, fclose, fseek, ftell, fread, sscanf
#include <stdlib.h>    // For malloc, free
#include <string.h>    // For strstr, strchr, strncpy, strlen, strcpy
#include <wchar.h>     // For wcscmp, wcscpy, wcslen, MultiByteToWideChar, WideCharToMultiByte, swprintf, wcsrchr
#include <wininet.h> // For InternetSetOptionW

#define WM_TRAY (WM_USER + 1)
#define ID_TRAY_EXIT 1001
#define ID_TRAY_AUTORUN 1002
#define ID_TRAY_SYSTEM_PROXY 1003 // New: System Proxy menu item ID
#define ID_TRAY_NODE_BASE 2000    // Base ID for dynamic node menu items

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

// Function to check if the application is set to autorun on startup
BOOL IsAutorunEnabled() {
    HKEY hKey;
    WCHAR path[MAX_PATH];
    // 使用当前模块的路径，而不是工作目录，因为开机启动路径是固定的
    GetModuleFileNameW(NULL, path, MAX_PATH);

    // Open the Run registry key for the current user
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                     0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR value[MAX_PATH];
        DWORD size = sizeof(value);
        // Query the value named "singbox_tray"
        LONG res = RegQueryValueExW(hKey, L"singbox_tray", NULL, NULL, (LPBYTE)value, &size);
        RegCloseKey(hKey);
        // Return TRUE if the value exists and matches the current executable path
        return (res == ERROR_SUCCESS && wcscmp(value, path) == 0);
    }
    return FALSE; // Autorun is not enabled or registry key could not be opened
}

// Function to enable or disable autorun
void SetAutorun(BOOL enable) {
    HKEY hKey;
    WCHAR path[MAX_PATH];
    // 使用当前模块的路径，而不是工作目录，因为开机启动路径是固定的
    GetModuleFileNameW(NULL, path, MAX_PATH);

    // Create or open the Run registry key with write access
    RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                    0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (hKey) {
        if (enable) {
            // Set the registry value to enable autorun
            RegSetValueExW(hKey, L"singbox_tray", 0, REG_SZ, (BYTE*)path,
                           (lstrlenW(path) + 1) * sizeof(WCHAR));
        } else {
            // Delete the registry value to disable autorun
            RegDeleteValueW(hKey, L"singbox_tray");
        }
        RegCloseKey(hKey);
    }
}

// Function to start the sing-box process
void StartSingBox() {
    // Initialize STARTUPINFOW structure
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW; // Specify that wShowWindow will be used
    si.wShowWindow = SW_HIDE; // Hide the new process window

    // Command line for sing-box
    // Use an editable buffer for cmdLine in CreateProcessW
    WCHAR cmdLine[MAX_PATH];
    // 由于已经设置了工作目录，直接使用相对路径即可
    wcscpy_s(cmdLine, ARRAYSIZE(cmdLine), L"sing-box.exe run -c config.json");

    // Create the process
    BOOL success = CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE,
                                  CREATE_NO_WINDOW | CREATE_NEW_CONSOLE, // Create with no window (but new console for background)
                                  NULL, NULL, &si, &pi); // Store process info in global pi

    if (!success) {
        MessageBoxW(NULL, L"启动 sing-box 失败！请确保 sing-box.exe 和 config.json 存在于同一目录下。", L"错误", MB_OK | MB_ICONERROR);
        ZeroMemory(&pi, sizeof(pi)); // Clear pi if process creation fails
    }
}

// Function to stop the sing-box process
void StopSingBox() {
    if (pi.hProcess) { // Check if a process handle exists
        // Use a timeout for graceful termination, then force kill if necessary
        if (TerminateProcess(pi.hProcess, 0)) { // Terminate the process
              // Wait for the process to exit (optional, but good practice)
              WaitForSingleObject(pi.hProcess, 5000); // Wait up to 5 seconds
        } else {
            // If termination failed, it might already be gone or permissions issue
            // For a simple tray app, TerminateProcess is usually sufficient.
            // MessageBoxW(NULL, L"Failed to terminate sing-box process.", L"Error", MB_OK | MB_ICONERROR);
        }
        CloseHandle(pi.hProcess); // Close the process handle
        CloseHandle(pi.hThread);  // Close the thread handle
        ZeroMemory(&pi, sizeof(pi)); // Clear pi to indicate no process is running
    }
}

// Corrected function to safely replace the "outbound" tag in config.json
// This function ensures that only the tag value within the "outbound": "tag" string
// is replaced, preserving the rest of the JSON structure.
void SafeReplaceOutbound(const wchar_t* newTagW) {
    FILE* f = _wfopen(L"config.json", L"rb"); // 文件路径相对于当前工作目录
    if (!f) {
        MessageBoxW(NULL, L"无法打开 config.json 文件进行读取。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = (char*)malloc(size + 1); // Buffer to hold entire file content
    if (!buffer) {
        MessageBoxW(NULL, L"内存分配失败。", L"错误", MB_OK | MB_ICONERROR);
        fclose(f);
        return;
    }
    fread(buffer, 1, size, f);
    buffer[size] = '\0'; // Null-terminate the buffer
    fclose(f);

    // Convert newTagW (wchar_t*) to char* (UTF-8) for searching and inserting
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

    // Find the start quote of the current tag after "outbound":
    char* currentTagStartQuote = strchr(outboundKeyPos + strlen("\"outbound\":"), '"');
    if (!currentTagStartQuote) {
        MessageBoxW(NULL, L"在 \"outbound\" 键后未找到起始引号。", L"错误", MB_OK | MB_ICONERROR);
        free(buffer);
        free(newTagMb);
        return;
    }
    currentTagStartQuote++; // Move past the opening quote to the actual tag content

    // Find the end quote of the current tag
    char* currentTagEndQuote = strchr(currentTagStartQuote, '"');
    if (!currentTagEndQuote) {
        MessageBoxW(NULL, L"在 \"outbound\" 键后未找到结束引号。", L"错误", MB_OK | MB_ICONERROR);
        free(buffer);
        free(newTagMb);
        return;
    }

    // Calculate lengths of the three parts:
    // 1. Prefix: from start of file to the character *before* the current tag's opening quote
    long prefixLen = currentTagStartQuote - buffer;
    // 2. Suffix: from the character *at* the current tag's closing quote to the end of the file
    long suffixLen = size - (currentTagEndQuote - buffer);

    // Calculate required size for the new buffer:
    // prefix length + length of new tag + suffix length + null terminator
    long newBufferSize = prefixLen + strlen(newTagMb) + suffixLen + 1;
    char* newBuffer = (char*)malloc(newBufferSize);
    if (!newBuffer) {
        MessageBoxW(NULL, L"内存分配失败。", L"错误", MB_OK | MB_ICONERROR);
        free(buffer);
        free(newTagMb);
        return;
    }

    // Construct the new buffer by copying parts and inserting the new tag
    // Copy the prefix part
    memcpy(newBuffer, buffer, prefixLen);
    newBuffer[prefixLen] = '\0'; // Null-terminate after prefix

    // Append the new tag
    strcat_s(newBuffer, newBufferSize, newTagMb);

    // Append the suffix part (starting from the closing quote of the old tag)
    strcat_s(newBuffer, newBufferSize, currentTagEndQuote);

    // Open config.json for writing (truncates existing file)
    FILE* out = _wfopen(L"config.json", L"wb"); // 文件路径相对于当前工作目录
    if (!out) {
        MessageBoxW(NULL, L"无法打开 config.json 文件进行写入。", L"错误", MB_OK | MB_ICONERROR);
        free(buffer);
        free(newTagMb);
        free(newBuffer);
        return;
    }

    // Write the entire new buffer content to the file
    fwrite(newBuffer, 1, strlen(newBuffer), out);
    fclose(out);

    // Free dynamically allocated memory
    free(buffer);
    free(newTagMb);
    free(newBuffer);
}

// Function to switch the active node
void SwitchNode(const wchar_t* tag) {
    SafeReplaceOutbound(tag); // Update config.json with the new node tag
    wcscpy_s(currentNode, ARRAYSIZE(currentNode), tag); // Update the global current node variable
    StopSingBox(); // Stop the currently running sing-box
    StartSingBox(); // Start sing-box with the updated configuration
}

// Function to parse node tags from config.json
// Returns TRUE on success, FALSE on failure
BOOL ParseTags() {
    nodeCount = 0; // Reset node count
    FILE* f = _wfopen(L"config.json", L"rb"); // 文件路径相对于当前工作目录
    if (!f) {
        return FALSE; // Indicate failure
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = (char*)malloc(size + 1); // Buffer to hold entire file content
    if (!buffer) {
        // Removed MessageBoxW here, as it will be handled by the retry logic in wWinMain
        fclose(f);
        return FALSE; // Indicate failure
    }
    fread(buffer, 1, size, f);
    buffer[size] = '\0'; // Null-terminate the buffer
    fclose(f);

    // Parse "outbounds" array to find all available node tags
    char* outbounds = strstr(buffer, "\"outbounds\":");
    if (outbounds) {
        char* pos = outbounds;
        // Search for all "tag": "..." within the "outbounds" array
        // We look for "tag": " to skip the type and find actual tags
        while ((pos = strstr(pos, "\"tag\": \"")) != NULL && nodeCount < ARRAYSIZE(nodeTags)) {
            pos += 8; // Move past "\"tag\": \""
            char* end = strchr(pos, '"'); // Find the closing quote
            if (end && (end - pos < ARRAYSIZE(nodeTags[0]))) { // Ensure tag fits in buffer
                char temp[ARRAYSIZE(nodeTags[0])];
                strncpy_s(temp, ARRAYSIZE(temp), pos, end - pos);
                temp[end - pos] = '\0'; // Null-terminate the extracted tag
                // Convert UTF-8 char* to WideChar wchar_t*
                MultiByteToWideChar(CP_UTF8, 0, temp, -1, nodeTags[nodeCount], ARRAYSIZE(nodeTags[0]));
                nodeCount++;
            }
            pos = end; // Continue search from after the current tag's closing quote
        }
    }

    // Parse the currently active "outbound" tag from "route" section
    // This is typically in the "route" section under "outbound"
    char* route_section = strstr(buffer, "\"route\":");
    if (route_section) {
        char* pos = strstr(route_section, "\"outbound\": \"");
        if (pos) {
            pos += 13; // Move past "\"outbound\": \""
            char* end = strchr(pos, '"'); // Find the closing quote
            if (end && (end - pos < ARRAYSIZE(currentNode))) { // Ensure tag fits in buffer
                char temp[ARRAYSIZE(currentNode)];
                strncpy_s(temp, ARRAYSIZE(temp), pos, end - pos);
                temp[end - pos] = '\0'; // Null-terminate the extracted tag
                // Convert UTF-8 char* to WideChar wchar_t*
                MultiByteToWideChar(CP_UTF8, 0, temp, -1, currentNode, ARRAYSIZE(currentNode));
            }
        }
    }

    free(buffer); // Free the buffer
    return TRUE; // Indicate success
}

// Function to read the HTTP inbound listen_port from config.json
int GetHttpInboundPort() {
    int port = 0;
    FILE* f = _wfopen(L"config.json", L"rb"); // 文件路径相对于当前工作目录
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

    // --- IMPROVED LOGIC FOR FINDING HTTP INBOUND PORT ---
    char* inbounds_arr_start = strstr(buffer, "\"inbounds\":");
    if (inbounds_arr_start) {
        // Find the start of the inbounds array, which is usually '['
        char* arr_open_bracket = strchr(inbounds_arr_start, '[');
        if (arr_open_bracket) {
            char* current_pos = arr_open_bracket;
            int brace_count = 0; // To track nested JSON objects

            // Loop through potential inbound objects
            while ((current_pos = strchr(current_pos + 1, '{')) != NULL) { // Find next object start
                brace_count = 1; // Count current '{'
                char* search_end_for_object = current_pos;

                // Find the end of the current object
                while (brace_count > 0 && search_end_for_object < buffer + size) {
                    search_end_for_object++;
                    if (*search_end_for_object == '{') brace_count++;
                    else if (*search_end_for_object == '}') brace_count--;
                }

                if (brace_count == 0 && search_end_for_object > current_pos) { // Found a complete object
                    // Check if this object is "type": "http"
                    char* type_key_pos = strstr(current_pos, "\"type\": \"http\"");
                    if (type_key_pos && type_key_pos < search_end_for_object) { // Ensure within current object
                        // Now find "listen_port" within this same object
                        char* port_key_pos = strstr(current_pos, "\"listen_port\":");
                        if (port_key_pos && port_key_pos < search_end_for_object) { // Ensure within current object
                            port_key_pos += strlen("\"listen_port\":");
                            // Safely read the integer
                            if (sscanf(port_key_pos, "%d", &port) == 1) {
                                free(buffer);
                                return port; // Found it, return the port
                            }
                        }
                    }
                }
                current_pos = search_end_for_object; // Continue search after this object
            }
        }
    }
    // --- END IMPROVED LOGIC ---

    free(buffer);
    return 0; // Port not found
}

// Function to set or unset system proxy
void SetSystemProxy(BOOL enable) {
    HKEY hKey;
    DWORD dwEnable = enable ? 1 : 0;
    int port = GetHttpInboundPort();
    WCHAR proxyServer[64]; // e.g., L"127.0.0.1:10809"

    if (port == 0 && enable) {
        MessageBoxW(NULL, L"在 config.json 中未找到 HTTP 入站端口，无法设置系统代理。\n请确保 inbounds 配置了 \"type\": \"http\" 和 \"listen_port\"。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    if (enable) {
        swprintf_s(proxyServer, ARRAYSIZE(proxyServer), L"127.0.0.1:%d", port);
    } else {
        proxyServer[0] = L'\0'; // Clear the string for unsetting
    }


    // Open the Internet Settings registry key with write access
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_PROXY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, REG_VALUE_PROXY_ENABLE, 0, REG_DWORD, (const BYTE*)&dwEnable, sizeof(dwEnable));

        if (enable) {
            RegSetValueExW(hKey, REG_VALUE_PROXY_SERVER, 0, REG_SZ, (const BYTE*)proxyServer, (wcslen(proxyServer) + 1) * sizeof(WCHAR));
            // Set bypass list (optional, but good practice for local addresses)
            RegSetValueExW(hKey, REG_VALUE_PROXY_OVERRIDE, 0, REG_SZ, (const BYTE*)L"<local>", (wcslen(L"<local>") + 1) * sizeof(WCHAR));
        } else {
            RegDeleteValueW(hKey, REG_VALUE_PROXY_SERVER); // Remove proxy server setting
            RegDeleteValueW(hKey, REG_VALUE_PROXY_OVERRIDE); // Remove override setting
        }
        RegCloseKey(hKey);

        // Inform other applications about the proxy change
        // These options force Internet Explorer and other apps to refresh proxy settings
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
    DWORD dwProxySize = sizeof(proxyServer); // Use bytes, not count of WCHARs for size
    int port = GetHttpInboundPort();
    WCHAR expectedProxyServer[64];
    swprintf_s(expectedProxyServer, ARRAYSIZE(expectedProxyServer), L"127.0.0.1:%d", port);

    // Initial value for dwProxySize should be the buffer size in bytes
    dwProxySize = sizeof(proxyServer);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_PROXY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        LONG res1 = RegQueryValueExW(hKey, REG_VALUE_PROXY_ENABLE, NULL, NULL, (LPBYTE)&dwEnable, &dwSize);
        LONG res2 = RegQueryValueExW(hKey, REG_VALUE_PROXY_SERVER, NULL, NULL, (LPBYTE)proxyServer, &dwProxySize);
        RegCloseKey(hKey);

        // Check if proxy is enabled and the server matches our expected sing-box proxy
        // Only consider it enabled by us if the port is valid and matches
        if (port > 0) {
            return (res1 == ERROR_SUCCESS && dwEnable == 1 &&
                                 res2 == ERROR_SUCCESS && wcscmp(proxyServer, expectedProxyServer) == 0);
        }
    }
    return FALSE; // Proxy is not enabled or not set by this application
}

// Function to update the tray icon context menu
void UpdateMenu() {
    if (hMenu) DestroyMenu(hMenu); // Destroy existing menu if any
    if (hNodeSubMenu) DestroyMenu(hNodeSubMenu); // Destroy existing submenu if any

    hMenu = CreatePopupMenu(); // Create main popup menu
    hNodeSubMenu = CreatePopupMenu(); // Create submenu for nodes

    // Populate the node submenu with discovered nodes
    for (int i = 0; i < nodeCount; i++) {
        UINT flags = MF_STRING;
        // Check the currently active node
        if (wcscmp(nodeTags[i], currentNode) == 0) {
            flags |= MF_CHECKED; // Add a checkmark if it's the current node
        }
        AppendMenuW(hNodeSubMenu, flags, ID_TRAY_NODE_BASE + i, nodeTags[i]);
    }
    // Add the "Switch Node" submenu to the main menu
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hNodeSubMenu, L"切换节点");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL); // Add a separator

    // Add "Autorun" option
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_AUTORUN, L"开机启动");
    // Add "System Proxy" option
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SYSTEM_PROXY, L"系统代理");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL); // Add a separator
    // Add "Exit" option
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出");
}

// Window procedure to handle messages
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Handle right-click on the tray icon
    if (msg == WM_TRAY && lParam == WM_RBUTTONUP) {
        POINT pt;
        GetCursorPos(&pt); // Get current cursor position
        SetForegroundWindow(hWnd); // Bring our window to foreground to receive menu messages

        ParseTags(); // Re-parse tags every time the menu is opened to get latest config
        UpdateMenu(); // Update the menu dynamically

        // Check/uncheck "Autorun" based on its current state
        CheckMenuItem(hMenu, ID_TRAY_AUTORUN, IsAutorunEnabled() ? MF_CHECKED : MF_UNCHECKED);
        // Check/uncheck "System Proxy" based on its current state
        CheckMenuItem(hMenu, ID_TRAY_SYSTEM_PROXY, IsSystemProxyEnabled() ? MF_CHECKED : MF_UNCHECKED);

        // Display the context menu
        // TPM_BOTTOMALIGN | TPM_LEFTALIGN ensures the menu appears below and to the left of the click
        TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
    }
    // Handle menu item clicks
    else if (msg == WM_COMMAND) {
        int id = LOWORD(wParam); // Get the ID of the clicked menu item
        if (id == ID_TRAY_EXIT) {
            Shell_NotifyIconW(NIM_DELETE, &nid); // Remove tray icon
            StopSingBox(); // Stop sing-box before exiting

            // Unset proxy on exit if it was set by this app
            if (IsSystemProxyEnabled()) {
                SetSystemProxy(FALSE);
            }
            PostQuitMessage(0); // Post a quit message to terminate the message loop
        } else if (id == ID_TRAY_AUTORUN) {
            SetAutorun(!IsAutorunEnabled()); // Toggle autorun setting
        } else if (id == ID_TRAY_SYSTEM_PROXY) {
            SetSystemProxy(!IsSystemProxyEnabled()); // Toggle system proxy
        } else if (id >= ID_TRAY_NODE_BASE && id < ID_TRAY_NODE_BASE + nodeCount) {
            // A node from the submenu was selected
            SwitchNode(nodeTags[id - ID_TRAY_NODE_BASE]); // Switch to the selected node
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam); // Default message processing
}

// Entry point for the Windows application
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    // Ensure only one instance of the application is running
    hMutex = CreateMutexW(NULL, TRUE, L"Global\\SingBoxTrayMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"sing-box 托盘程序已在运行。", L"提示", MB_OK | MB_ICONINFORMATION);
        if (hMutex) CloseHandle(hMutex); // Close handle if we acquired it but another exists
        return 0; // Exit if another instance is already running
    }

    // --- 设置程序工作目录 ---
    WCHAR szPath[MAX_PATH];
    // 获取当前可执行文件的完整路径
    GetModuleFileNameW(NULL, szPath, MAX_PATH);

    // 查找最后一个反斜杠的位置，以截断文件名，只保留目录部分
    WCHAR* p = wcsrchr(szPath, L'\\');
    if (p != NULL) {
        *p = L'\0'; // 在文件名之前截断字符串，使其成为目录路径
        // 设置当前进程的工作目录
        if (!SetCurrentDirectoryW(szPath)) {
            MessageBoxW(NULL, L"无法设置程序工作目录。请确保程序拥有访问其所在目录的权限。", L"错误", MB_OK | MB_ICONERROR);
            if (hMutex) CloseHandle(hMutex);
            return 1; // 设置工作目录失败，程序退出
        }
    } else {
        // 这通常不会发生，除非GetModuleFileNameW返回了一个不带路径的文件名
        MessageBoxW(NULL, L"无法获取程序所在目录。请将 sing-box-tray.exe 放置在一个有效的目录下。", L"错误", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }
    // --- 工作目录设置结束 ---

    ZeroMemory(&pi, sizeof(pi)); // Initialize PROCESS_INFORMATION structure

    // Attempt to parse tags with retries
    int retry_count = 0;
    const int MAX_RETRIES = 3;
    const int RETRY_DELAY_MS = 10000; // 10 seconds

    while (retry_count < MAX_RETRIES) {
        if (ParseTags()) {
            break; // Success, exit loop
        }
        retry_count++;
        if (retry_count < MAX_RETRIES) {
            Sleep(RETRY_DELAY_MS); // Wait before retrying
        }
    }

    if (retry_count == MAX_RETRIES && !ParseTags()) { // Final check after retries
        MessageBoxW(NULL, L"无法读取 config.json 文件，请确保其存在且格式正确。程序将退出。", L"错误", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        return 1; // Exit if config cannot be parsed after retries
    }

    // Register window class
    const wchar_t *CLASS_NAME = L"TrayWindowClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;         // Our window procedure
    wc.hInstance = hInstance;         // Current instance handle
    wc.lpszClassName = CLASS_NAME;    // Class name
    // Load icon from resources (assuming ID 1 for your icon in the .rc file)
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(1));
    // If you don't have a resource file, use LoadIconW(NULL, IDI_APPLICATION) for default icon
    if (!wc.hIcon) {
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION); // Fallback to default application icon
    }

    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"注册窗口类失败！", L"错误", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // Create a hidden window (not visible on screen)
    // This window receives messages from the tray icon
    hwnd = CreateWindowExW(0, CLASS_NAME, L"TrayApp", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        MessageBoxW(NULL, L"创建窗口失败！", L"错误", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // Initialize NOTIFYICONDATA structure for the tray icon
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1; // Unique ID for the tray icon
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; // Flags: custom message, icon, tooltip
    nid.uCallbackMessage = WM_TRAY; // Custom message for tray icon events
    nid.hIcon = wc.hIcon; // Use the icon loaded for the window class
    lstrcpyW(nid.szTip, L"sing-box 正在运行"); // Tooltip text

    // Add the tray icon to the notification area
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        MessageBoxW(NULL, L"添加托盘图标失败！", L"错误", MB_OK | MB_ICONERROR);
        DestroyWindow(hwnd);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    StartSingBox(); // Start sing-box when the application launches

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) { // Retrieve messages from the message queue
        TranslateMessage(&msg); // Translate virtual-key messages into character messages
        DispatchMessageW(&msg); // Dispatch the message to the window procedure
    }

    // Cleanup on exit
    // Make sure to remove the tray icon when the application exits
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (hMutex) CloseHandle(hMutex); // Release the mutex
    // DestroyIcon(nid.hIcon); // This icon is owned by the class, not directly by nid (handled by UnregisterClassW)
    UnregisterClassW(CLASS_NAME, hInstance); // Unregister the window class
    return (int)msg.wParam; // Return exit code
}