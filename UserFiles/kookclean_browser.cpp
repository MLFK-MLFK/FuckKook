#include <winsock2.h>
#include <ws2tcpip.h>

#include "kookclean.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "kookclean_payload.h"

#pragma comment(lib, "ws2_32.lib")

// ============================================================================
// 浏览器进程里的功能：
//   1. 通过 --remote-debugging-port 暴露的 CDP 端口，把 payload.js 注入到每个页面
//      （Page.addScriptToEvaluateOnNewDocument 管新文档 + Runtime.evaluate 管当前文档）
//   2. 接收页面里按钮打印的 __FKO_TOGGLE_DEVTOOLS__，切换 KOOK 内置 DevTools
//   3. 广告选择器：内置默认值 + DLL 同目录 kook-adblock.txt（每行一个选择器，
//      以 ! 开头表示把命中元素所在的 .discover-module 整块隐藏）
// ============================================================================

namespace fko {
namespace {

// ---------------------------------------------------------------- 基础工具

std::wstring BaseNameW(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

bool FileExistsW(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ReadFileToString(const std::wstring& path, std::string& out, size_t maxBytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) > maxBytes) {
        CloseHandle(file);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(file, &out[0], static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok) {
        out.clear();
        return false;
    }
    out.resize(read);
    return true;
}

std::string JsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 32);
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buffer[8] = {};
                    sprintf_s(buffer, "\\u%04x", c);
                    out += buffer;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

std::string Base64(const unsigned char* data, size_t length) {
    static const char* kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((length + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < length; i += 3) {
        const unsigned int value = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kTable[(value >> 18) & 0x3F];
        out += kTable[(value >> 12) & 0x3F];
        out += kTable[(value >> 6) & 0x3F];
        out += kTable[value & 0x3F];
    }
    if (i + 1 == length) {
        const unsigned int value = data[i] << 16;
        out += kTable[(value >> 18) & 0x3F];
        out += kTable[(value >> 12) & 0x3F];
        out += "==";
    } else if (i + 2 == length) {
        const unsigned int value = (data[i] << 16) | (data[i + 1] << 8);
        out += kTable[(value >> 18) & 0x3F];
        out += kTable[(value >> 12) & 0x3F];
        out += kTable[(value >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

// ---------------------------------------------------------------- 规则

struct Rule {
    std::string selector;
    int hideModule = 0;
};

std::vector<Rule> LoadRules() {
    std::vector<Rule> rules;
    // 内置默认：对应 E:\KOOK\DLL\广告1.txt（远程下发的广告轮播）。
    // 广告2.txt（发现页“开黑活动/开黑服务器”模块）已按要求恢复显示，不再内置。
    rules.push_back({".mixed-ad-carousel-wrapper", 0});
    rules.push_back({".kook-ad-image", 0});
    rules.push_back({".swiper-slide-duplicate .kook-ad-image", 0});

    const std::wstring configPath = GetOwnModuleDir() + L"\\kook-adblock.txt";
    std::string text;
    if (ReadFileToString(configPath, text, 256u << 10)) {
        size_t start = 0;
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
            start = 3;  // 允许 UTF-8 BOM
        }
        while (start <= text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) {
                end = text.size();
            }
            std::string line = text.substr(start, end - start);
            start = end + 1;

            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            size_t begin = line.find_first_not_of(" \t");
            if (begin == std::string::npos) {
                continue;
            }
            line = line.substr(begin);
            if (line.empty() || line[0] == '#' || line[0] == ';') {
                continue;
            }
            Rule rule;
            if (line[0] == '!') {
                rule.hideModule = 1;
                line = line.substr(1);
            }
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            rule.selector = line;
            rules.push_back(rule);
        }
        DebugLog("rules: loaded %ls (+%d lines)", configPath.c_str(), static_cast<int>(rules.size()));
    }

    // 配置文件里常常会重复写一遍内置规则，这里按选择器去重（保留先出现的，即内置的那条）
    std::vector<Rule> unique;
    for (size_t i = 0; i < rules.size(); ++i) {
        bool duplicate = false;
        for (size_t j = 0; j < unique.size(); ++j) {
            if (unique[j].selector == rules[i].selector) {
                if (rules[i].hideModule != 0) {
                    unique[j].hideModule = rules[i].hideModule;
                }
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            unique.push_back(rules[i]);
        }
    }

    return unique;
}

std::string RulesToJson(const std::vector<Rule>& rules) {
    std::string json = "[";
    for (size_t i = 0; i < rules.size(); ++i) {
        if (i != 0) {
            json += ",";
        }
        json += "{\"s\":\"";
        json += JsonEscape(rules[i].selector);
        json += "\",\"m\":";
        json += rules[i].hideModule ? "1" : "0";
        json += "}";
    }
    json += "]";
    return json;
}

std::string BuildPayload() {
    std::string payload = kPayloadTemplate;
    const std::string placeholder = "__FKO_RULES__";
    // 用 rfind：注释里也提到了这个占位符，只有最后（代码里）那一处是要替换的
    const size_t pos = payload.rfind(placeholder);
    if (pos != std::string::npos) {
        payload.replace(pos, placeholder.size(), RulesToJson(LoadRules()));
    }
    return payload;
}

// ---------------------------------------------------------------- TCP / HTTP

bool SendAll(SOCKET socket, const char* data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        const int chunk = static_cast<int>(std::min<size_t>(length - offset, 32768));
        const int sent = send(socket, data + offset, chunk, 0);
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    return true;
}

SOCKET ConnectLoopback(unsigned short port, int recvTimeoutMs) {
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket);
        return INVALID_SOCKET;
    }
    DWORD timeout = static_cast<DWORD>(recvTimeoutMs);
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return socket;
}

bool HttpGetText(unsigned short port, const char* path, std::string& body) {
    SOCKET socket = ConnectLoopback(port, 1500);
    if (socket == INVALID_SOCKET) {
        return false;
    }

    char request[512] = {};
    sprintf_s(request, "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nConnection: close\r\n\r\n", path,
              static_cast<unsigned int>(port));
    if (!SendAll(socket, request, strlen(request))) {
        closesocket(socket);
        return false;
    }

    std::string raw;
    char buffer[8192];
    const ULONGLONG deadline = GetTickCount64() + 4000;
    for (;;) {
        const int received = recv(socket, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received > 0) {
            raw.append(buffer, static_cast<size_t>(received));
            if (raw.size() > (4u << 20)) {
                break;
            }
            continue;
        }
        if (received == 0) {
            break;  // 对端关闭
        }
        if (WSAGetLastError() == WSAETIMEDOUT && GetTickCount64() < deadline) {
            continue;
        }
        break;
    }
    closesocket(socket);

    const size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return false;
    }
    body = raw.substr(headerEnd + 4);
    return true;
}

// ---------------------------------------------------------------- /json/list

struct TargetInfo {
    std::string id;
    std::string type;
    std::string url;
    std::string wsUrl;
};

std::string ExtractJsonString(const std::string& object, const char* key) {
    const std::string pattern = std::string("\"") + key + "\"";
    size_t pos = object.find(pattern);
    if (pos == std::string::npos) {
        return std::string();
    }
    pos = object.find(':', pos + pattern.size());
    if (pos == std::string::npos) {
        return std::string();
    }
    pos = object.find('"', pos + 1);
    if (pos == std::string::npos) {
        return std::string();
    }
    const size_t end = object.find('"', pos + 1);
    if (end == std::string::npos) {
        return std::string();
    }
    return object.substr(pos + 1, end - pos - 1);
}

std::vector<TargetInfo> ParseTargets(const std::string& json) {
    std::vector<TargetInfo> targets;
    size_t pos = 0;
    while ((pos = json.find("\"webSocketDebuggerUrl\"", pos)) != std::string::npos) {
        const size_t objectStart = json.rfind('{', pos);
        size_t objectEnd = json.find('}', pos);
        if (objectStart == std::string::npos || objectEnd == std::string::npos) {
            break;
        }
        const std::string object = json.substr(objectStart, objectEnd - objectStart + 1);
        TargetInfo info;
        info.id = ExtractJsonString(object, "id");
        info.type = ExtractJsonString(object, "type");
        info.url = ExtractJsonString(object, "url");
        info.wsUrl = ExtractJsonString(object, "webSocketDebuggerUrl");
        if (!info.wsUrl.empty()) {
            targets.push_back(info);
        }
        pos = objectEnd;
    }
    return targets;
}

// ---------------------------------------------------------------- WebSocket

std::string WsPathFromUrl(const std::string& wsUrl) {
    const size_t scheme = wsUrl.find("://");
    if (scheme == std::string::npos) {
        return wsUrl;
    }
    const size_t slash = wsUrl.find('/', scheme + 3);
    return slash == std::string::npos ? std::string("/") : wsUrl.substr(slash);
}

bool WsReadSome(SOCKET socket, char* buffer, int size, int& received) {
    for (;;) {
        const int result = recv(socket, buffer, size, 0);
        if (result > 0) {
            received = result;
            return true;
        }
        if (result == 0) {
            received = 0;
            return false;
        }
        if (WSAGetLastError() == WSAETIMEDOUT) {
            received = 0;
            return true;  // 超时不算断开
        }
        received = 0;
        return false;
    }
}

bool WsReadExact(SOCKET socket, char* buffer, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        int received = 0;
        if (!WsReadSome(socket, buffer + offset, static_cast<int>(length - offset), received)) {
            return false;
        }
        if (received == 0) {
            continue;  // 超时，继续等
        }
        offset += static_cast<size_t>(received);
    }
    return true;
}

bool WsSendFrame(SOCKET socket, unsigned char opcode, const char* data, size_t length) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    if (length < 126) {
        frame.push_back(static_cast<char>(0x80 | static_cast<unsigned char>(length)));
    } else if (length < 65536) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((length >> 8) & 0xFF));
        frame.push_back(static_cast<char>(length & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((length >> (i * 8)) & 0xFF));
        }
    }

    unsigned char mask[4] = {};
    const unsigned int seed = static_cast<unsigned int>(GetTickCount64() ^ (GetCurrentThreadId() << 16));
    srand(seed);
    for (int i = 0; i < 4; ++i) {
        mask[i] = static_cast<unsigned char>(rand() & 0xFF);
    }
    frame.append(reinterpret_cast<const char*>(mask), 4);
    for (size_t i = 0; i < length; ++i) {
        frame.push_back(static_cast<char>(static_cast<unsigned char>(data[i]) ^ mask[i & 3]));
    }
    return SendAll(socket, frame.data(), frame.size());
}

bool WsSendText(SOCKET socket, const std::string& text) {
    return WsSendFrame(socket, 0x1, text.data(), text.size());
}

// 0 = 拿到一条文本消息（outText 有效），1 = 控制帧/需要继续，-1 = 连接结束
int WsReceiveMessage(SOCKET socket, std::string& outText) {
    unsigned char header[2] = {};
    if (!WsReadExact(socket, reinterpret_cast<char*>(header), 2)) {
        return -1;
    }

    const bool finalFrame = (header[0] & 0x80) != 0;
    const unsigned char opcode = header[0] & 0x0F;
    const bool masked = (header[1] & 0x80) != 0;
    unsigned long long length = header[1] & 0x7F;
    if (length == 126) {
        unsigned char extended[2] = {};
        if (!WsReadExact(socket, reinterpret_cast<char*>(extended), 2)) {
            return -1;
        }
        length = (static_cast<unsigned long long>(extended[0]) << 8) | extended[1];
    } else if (length == 127) {
        unsigned char extended[8] = {};
        if (!WsReadExact(socket, reinterpret_cast<char*>(extended), 8)) {
            return -1;
        }
        length = 0;
        for (int i = 0; i < 8; ++i) {
            length = (length << 8) | extended[i];
        }
    }
    if (length > (16u << 20)) {
        return -1;
    }

    unsigned char mask[4] = {};
    if (masked && !WsReadExact(socket, reinterpret_cast<char*>(mask), 4)) {
        return -1;
    }

    std::string payload;
    payload.resize(static_cast<size_t>(length));
    if (length > 0 && !WsReadExact(socket, &payload[0], static_cast<size_t>(length))) {
        return -1;
    }
    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i & 3]);
        }
    }

    if (opcode == 0x8) {
        return -1;  // close
    }
    if (opcode == 0x9) {
        WsSendFrame(socket, 0xA, payload.data(), payload.size());  // pong
        return 1;
    }
    if (opcode == 0xA) {
        return 1;
    }
    if (opcode == 0x1 || opcode == 0x0) {
        static_cast<void>(finalFrame);
        // 分片消息简单拼接（CDP 的消息一般不分片）
        thread_local std::string pending;
        pending += payload;
        if (!finalFrame) {
            return 1;
        }
        outText.swap(pending);
        pending.clear();
        return 0;
    }
    return 1;
}

std::string WsHandshake(SOCKET socket, unsigned short port, const std::string& wsUrl) {
    unsigned char nonce[16] = {};
    const unsigned int seed = static_cast<unsigned int>(GetTickCount64() ^ (GetCurrentThreadId() * 2654435761u));
    srand(seed);
    for (int i = 0; i < 16; ++i) {
        nonce[i] = static_cast<unsigned char>(rand() & 0xFF);
    }
    const std::string key = Base64(nonce, sizeof(nonce));

    char request[1024] = {};
    sprintf_s(request,
              "GET %s HTTP/1.1\r\n"
              "Host: 127.0.0.1:%u\r\n"
              "Upgrade: websocket\r\n"
              "Connection: Upgrade\r\n"
              "Sec-WebSocket-Key: %s\r\n"
              "Sec-WebSocket-Version: 13\r\n"
              "\r\n",
              WsPathFromUrl(wsUrl).c_str(), static_cast<unsigned int>(port), key.c_str());
    if (!SendAll(socket, request, strlen(request))) {
        return std::string();
    }

    std::string response;
    char buffer[1024];
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        const int received = recv(socket, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received > 0) {
            response.append(buffer, static_cast<size_t>(received));
            if (response.find("\r\n\r\n") != std::string::npos) {
                break;
            }
            continue;
        }
        if (received == 0) {
            break;
        }
        if (WSAGetLastError() != WSAETIMEDOUT) {
            break;
        }
    }

    if (response.find("101") == std::string::npos) {
        DebugLog("ws handshake failed: %s", response.substr(0, 120).c_str());
        return std::string();
    }
    return key;
}

// ---------------------------------------------------------------- DevTools 切换

struct WindowSearch {
    DWORD processId = 0;
    HWND result = nullptr;
    long long bestArea = -1;
};

// DevTools 独立窗口的标题是 “Developer Tools - http://...”，它同样是 Chrome_WidgetWin_1，
// 而且也是本进程的窗口；只看 "DevTools" 会漏掉它，于是热键被发到 DevTools 窗口上就关不掉了。
bool IsDevToolsWindowTitle(const wchar_t* title) {
    return wcsstr(title, L"DevTools") != nullptr || wcsstr(title, L"Developer Tools") != nullptr ||
           wcsstr(title, L"开发者工具") != nullptr;
}

BOOL CALLBACK EnumWindowsProc(HWND window, LPARAM parameter) {
    WindowSearch* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != search->processId || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    if ((GetWindowLongW(window, GWL_STYLE) & WS_CHILD) != 0) {
        return TRUE;
    }
    wchar_t className[64] = {};
    GetClassNameW(window, className, _countof(className));
    if (_wcsicmp(className, L"Chrome_WidgetWin_1") != 0) {
        return TRUE;
    }
    wchar_t title[512] = {};
    GetWindowTextW(window, title, _countof(title));
    if (IsDevToolsWindowTitle(title)) {
        return TRUE;  // 别把 DevTools 自己的窗口当主窗口
    }
    RECT rect = {};
    if (!GetWindowRect(window, &rect)) {
        return TRUE;
    }
    const long long area = static_cast<long long>(rect.right - rect.left) * (rect.bottom - rect.top);
    if (area > search->bestArea) {
        search->bestArea = area;
        search->result = window;
    }
    return TRUE;
}

HWND FindOwnMainWindow() {
    WindowSearch search;
    search.processId = GetCurrentProcessId();
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

void FocusWindow(HWND window) {
    const HWND foreground = GetForegroundWindow();
    DWORD foregroundThread = 0;
    if (foreground != nullptr) {
        foregroundThread = GetWindowThreadProcessId(foreground, nullptr);
    }
    const DWORD currentThread = GetCurrentThreadId();
    const bool attached = (foregroundThread != 0 && foregroundThread != currentThread);
    if (attached) {
        AttachThreadInput(currentThread, foregroundThread, TRUE);
    }
    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
    BringWindowToTop(window);
    SetActiveWindow(window);
    SetFocus(window);
    if (attached) {
        AttachThreadInput(currentThread, foregroundThread, FALSE);
    }
}

// 复用 KOOK 内置热键 Ctrl+Alt+Shift+0（标准布局下 Shift+0 = ')'）开关右侧停靠的 DevTools
void ToggleDevTools() {
    static ULONGLONG lastToggle = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastToggle < 600) {
        return;
    }
    lastToggle = now;

    const HWND window = FindOwnMainWindow();
    if (window == nullptr) {
        DebugLog("toggle devtools: window not found");
        return;
    }
    FocusWindow(window);
    Sleep(80);

    const BYTE control = static_cast<BYTE>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
    const BYTE menu = static_cast<BYTE>(MapVirtualKeyW(VK_MENU, MAPVK_VK_TO_VSC));
    const BYTE shift = static_cast<BYTE>(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC));
    const BYTE zero = static_cast<BYTE>(MapVirtualKeyW('0', MAPVK_VK_TO_VSC));

    keybd_event(VK_CONTROL, control, 0, 0);
    keybd_event(VK_MENU, menu, 0, 0);
    keybd_event(VK_SHIFT, shift, 0, 0);
    Sleep(20);
    keybd_event('0', zero, 0, 0);
    Sleep(30);
    keybd_event('0', zero, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_SHIFT, shift, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_MENU, menu, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, control, KEYEVENTF_KEYUP, 0);

    DebugLog("toggle devtools sent (hwnd=%p)", window);
}

// ---------------------------------------------------------------- 注入

struct TargetThreadContext {
    std::string targetId;
    std::string wsUrl;
    unsigned short port = 0;
};

CRITICAL_SECTION g_targetLock;
bool g_targetLockReady = false;
std::vector<std::string> g_activeTargets;

// 页面按钮 -> DLL 的通道名（payload.js 里同名使用）
const char* const kToggleBinding = "__fkoToggleDevtools";

bool ContainsMarker(const std::string& text) {
    if (text.find("__FKO_TOGGLE_DEVTOOLS__") != std::string::npos) {
        return true;
    }
    // Runtime.bindingCalled：页面调用了 DLL 注册的 __fkoToggleDevtools
    return text.find("bindingCalled") != std::string::npos &&
           text.find(kToggleBinding) != std::string::npos;
}

std::string BuildCommand(int id, const char* method, const std::string& params) {
    std::string message = "{\"id\":";
    message += std::to_string(id);
    message += ",\"method\":\"";
    message += method;
    message += "\"";
    if (!params.empty()) {
        message += ",\"params\":";
        message += params;
    }
    message += "}";
    return message;
}

void RunTargetSession(TargetThreadContext context) {
    const std::string payload = BuildPayload();

    SOCKET socket = ConnectLoopback(context.port, 1000);
    if (socket == INVALID_SOCKET) {
        DebugLog("cdp: connect failed for %s", context.targetId.c_str());
        return;
    }
    if (WsHandshake(socket, context.port, context.wsUrl).empty()) {
        closesocket(socket);
        return;
    }
    DebugLog("cdp: session up for %s", context.targetId.c_str());

    const std::string escaped = JsonEscape(payload);
    int id = 1;
    WsSendText(socket, BuildCommand(id++, "Runtime.enable", ""));
    // binding 必须在页面脚本之前注册：页面点击按钮时调用 __fkoToggleDevtools，
    // 浏览器会通过 Runtime.bindingCalled 事件通知本会话（console 事件在多会话下收不到）。
    WsSendText(socket, BuildCommand(id++, "Runtime.addBinding",
                                    "{\"name\":\"" + std::string(kToggleBinding) + "\"}"));
    WsSendText(socket, BuildCommand(id++, "Page.enable", ""));
    WsSendText(socket, BuildCommand(id++, "Page.addScriptToEvaluateOnNewDocument",
                                    "{\"source\":\"" + escaped + "\",\"runImmediately\":true}"));
    WsSendText(socket, BuildCommand(id++, "Runtime.evaluate",
                                    "{\"expression\":\"" + escaped + "\",\"returnByValue\":false}"));

    for (;;) {
        std::string message;
        const int state = WsReceiveMessage(socket, message);
        if (state < 0) {
            break;
        }
        if (state == 0) {
            if (ContainsMarker(message)) {
                DebugLog("cdp: devtools toggle requested from page");
                ToggleDevTools();
            } else if (message.find("\"error\"") != std::string::npos &&
                       message.find("\"id\"") != std::string::npos) {
                DebugLog("cdp: error response: %s", message.substr(0, 300).c_str());
            }
        }
    }

    closesocket(socket);
    DebugLog("cdp: session closed for %s", context.targetId.c_str());
}

DWORD WINAPI TargetThreadProc(LPVOID parameter) {
    TargetThreadContext* context = reinterpret_cast<TargetThreadContext*>(parameter);
    RunTargetSession(*context);
    const std::string targetId = context->targetId;
    delete context;

    if (g_targetLockReady) {
        EnterCriticalSection(&g_targetLock);
        g_activeTargets.erase(std::remove(g_activeTargets.begin(), g_activeTargets.end(), targetId),
                              g_activeTargets.end());
        LeaveCriticalSection(&g_targetLock);
    }
    return 0;
}

bool IsTargetActive(const std::string& targetId) {
    bool active = false;
    if (g_targetLockReady) {
        EnterCriticalSection(&g_targetLock);
        active = std::find(g_activeTargets.begin(), g_activeTargets.end(), targetId) != g_activeTargets.end();
        if (!active) {
            g_activeTargets.push_back(targetId);
        }
        LeaveCriticalSection(&g_targetLock);
    }
    return active;
}

unsigned short DiscoverDebugPort() {
    const wchar_t* commandLine = GetCommandLineW();
    const wchar_t* marker = wcsstr(commandLine, L"--remote-debugging-port=");
    if (marker == nullptr) {
        return 0;
    }
    marker += wcslen(L"--remote-debugging-port=");
    int port = 0;
    while (*marker >= L'0' && *marker <= L'9' && port < 65536) {
        port = port * 10 + (*marker - L'0');
        ++marker;
    }
    return port > 0 && port < 65536 ? static_cast<unsigned short>(port) : 0;
}

DWORD WINAPI BrowserWorker(LPVOID parameter) {
    static_cast<void>(parameter);

    const unsigned short port = DiscoverDebugPort();
    if (port == 0) {
        DebugLog("browser: --remote-debugging-port not found in command line, abort");
        return 0;
    }
    DebugLog("browser: cdp port = %u", static_cast<unsigned int>(port));

    bool endpointReady = false;
    for (int attempt = 0; attempt < 120; ++attempt) {
        std::string body;
        if (HttpGetText(port, "/json/version", body)) {
            endpointReady = true;
            break;
        }
        Sleep(500);
    }
    if (!endpointReady) {
        DebugLog("browser: cdp endpoint never came up on %u", static_cast<unsigned int>(port));
        return 0;
    }
    DebugLog("browser: cdp endpoint ready");

    for (;;) {
        std::string body;
        if (HttpGetText(port, "/json/list", body)) {
            const std::vector<TargetInfo> targets = ParseTargets(body);
            for (size_t i = 0; i < targets.size(); ++i) {
                const TargetInfo& info = targets[i];
                if (info.wsUrl.empty() || info.id.empty()) {
                    continue;
                }
                if (info.type != "page") {
                    continue;
                }
                // DevTools 前端自己也是一个 page target，绝不能往里注入
                if (info.url.rfind("devtools://", 0) == 0) {
                    continue;
                }
                if (IsTargetActive(info.id)) {
                    continue;
                }
                TargetThreadContext* context = new TargetThreadContext();
                context->targetId = info.id;
                context->wsUrl = info.wsUrl;
                context->port = port;
                HANDLE thread = CreateThread(nullptr, 0, TargetThreadProc, context, 0, nullptr);
                if (thread != nullptr) {
                    CloseHandle(thread);
                } else {
                    delete context;
                }
            }
        }
        Sleep(2500);
    }
    return 0;
}

}  // namespace

void StartBrowserFeatures() {
    if (!IsKookProcess() || !IsBrowserProcess()) {
        return;
    }
    if (!g_targetLockReady) {
        InitializeCriticalSection(&g_targetLock);
        g_targetLockReady = true;
    }
    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        DebugLog("browser: WSAStartup failed");
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, BrowserWorker, nullptr, 0, nullptr);
    if (thread != nullptr) {
        CloseHandle(thread);
    }
}

}  // namespace fko
