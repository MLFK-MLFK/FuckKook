#include "kookclean.h"

#include <winternl.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace fko {

// 该标记会被写进 DLL 的只读数据里：注入子进程之前，用它判断目标目录里的 version.dll 是不是我们这份
const char kDllMarker[] = "FKO_KOOKCLEAN|2026-11-04|b7d41c9e";

namespace {

bool g_firstInstance = false;
bool g_isKookProcess = false;
bool g_isBrowserProcess = false;

void PatchGetCommandLineW();

// DllMain 里只能用这种“纯栈 + kernel32”的日志，绝不能走 CRT 堆（加载锁里分配可能死锁）。
bool RawTraceEnabled() {
    static int state = -1;
    if (state < 0) {
        wchar_t temp[MAX_PATH] = {};
        const DWORD length = GetTempPathW(static_cast<DWORD>(_countof(temp)), temp);
        state = 0;
        if (length > 0 && length + 20 < _countof(temp)) {
            wcscpy_s(temp + length, _countof(temp) - length, L"kookclean-debug.txt");
            if (GetFileAttributesW(temp) != INVALID_FILE_ATTRIBUTES) {
                state = 1;
            }
        }
    }
    return state == 1;
}

void RawTrace(const char* message) {
    if (!RawTraceEnabled()) {
        return;
    }
    wchar_t temp[MAX_PATH] = {};
    const DWORD length = GetTempPathW(static_cast<DWORD>(_countof(temp)), temp);
    if (length == 0 || length + 20 >= _countof(temp)) {
        return;
    }
    wchar_t path[MAX_PATH] = {};
    wcscpy_s(path, temp);
    wcscat_s(path, L"FuckKookObs.log");

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    char line[1200] = {};
    sprintf_s(line, _countof(line), "[%02u:%02u:%02u.%03u][%lu] %s\r\n", now.wHour, now.wMinute, now.wSecond,
              now.wMilliseconds, GetCurrentProcessId(), message);

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
    CloseHandle(file);
}

// 直接改 PEB 里的 CommandLine：KOOK.exe 静态导入 VERSION.dll，本 DLL 在进程入口点之前
// 就被加载，这时把 --remote-debugging-port 原地写进命令行缓冲区，Chromium 解析时就能看到。
void AppendSwitchToCommandLine(const wchar_t* switchText) {
#ifdef _WIN64
    PEB* peb = reinterpret_cast<PEB*>(__readgsqword(0x60));
#else
    PEB* peb = reinterpret_cast<PEB*>(__readfsdword(0x30));
#endif
    if (peb == nullptr || peb->ProcessParameters == nullptr) {
        RawTrace("cmdline: no process parameters");
        return;
    }

    PRTL_USER_PROCESS_PARAMETERS parameters = peb->ProcessParameters;
    if (parameters->CommandLine.Buffer == nullptr || parameters->CommandLine.Length == 0) {
        RawTrace("cmdline: empty");
        return;
    }
    if (wcsstr(parameters->CommandLine.Buffer, L"--remote-debugging-port=") != nullptr) {
        RawTrace("cmdline: switch already present");
        return;
    }

    const size_t oldChars = parameters->CommandLine.Length / sizeof(wchar_t);
    const size_t switchChars = wcslen(switchText);
    const size_t totalChars = oldChars + 1 + switchChars;
    const size_t capacityChars = parameters->CommandLine.MaximumLength / sizeof(wchar_t);

    if (parameters->CommandLine.MaximumLength >= parameters->CommandLine.Length &&
        capacityChars > totalChars + 1) {
        // 原地追加：GetCommandLineW() 和 CRT 的 argv 都指向同一块缓冲区，追加后都能看到
        wchar_t* inplace = parameters->CommandLine.Buffer;
        inplace[oldChars] = L' ';
        memcpy(inplace + oldChars + 1, switchText, switchChars * sizeof(wchar_t));
        inplace[totalChars] = L'\0';
        parameters->CommandLine.Length = static_cast<USHORT>(totalChars * sizeof(wchar_t));
        PatchGetCommandLineW();

        char report[600] = {};
        sprintf_s(report, _countof(report), "cmdline: appended in place -> %ls", GetCommandLineW());
        RawTrace(report);
        return;
    }

    const size_t totalBytes = (totalChars + 1) * sizeof(wchar_t);
    wchar_t* buffer =
        static_cast<wchar_t*>(VirtualAlloc(nullptr, totalBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (buffer == nullptr) {
        RawTrace("cmdline: VirtualAlloc failed");
        return;
    }
    memcpy(buffer, parameters->CommandLine.Buffer, oldChars * sizeof(wchar_t));
    buffer[oldChars] = L' ';
    memcpy(buffer + oldChars + 1, switchText, switchChars * sizeof(wchar_t));
    buffer[totalChars] = L'\0';

    parameters->CommandLine.Buffer = buffer;
    parameters->CommandLine.Length = static_cast<USHORT>(totalChars * sizeof(wchar_t));
    parameters->CommandLine.MaximumLength = static_cast<USHORT>(totalBytes);
    PatchGetCommandLineW();

    char report[600] = {};
    sprintf_s(report, _countof(report), "cmdline: buffer replaced -> %ls", GetCommandLineW());
    RawTrace(report);
}

// GetCommandLineW() 在进程初始化时被 kernelbase 缓存，改 PEB 它看不到（实测）。
// 这里把 kernelbase!GetCommandLineW 的头 12 字节改成 `mov rax, 函数地址; jmp rax`，
// 让所有调用方（CRT 解析 argv、Chromium 解析开关）都拿到改写后的命令行。
extern "C" __declspec(noinline) LPWSTR WINAPI PatchedGetCommandLineW() {
#ifdef _WIN64
    PEB* peb = reinterpret_cast<PEB*>(__readgsqword(0x60));
#else
    PEB* peb = reinterpret_cast<PEB*>(__readfsdword(0x30));
#endif
    if (peb != nullptr && peb->ProcessParameters != nullptr &&
        peb->ProcessParameters->CommandLine.Buffer != nullptr) {
        return peb->ProcessParameters->CommandLine.Buffer;
    }
    return const_cast<LPWSTR>(L"");
}

void PatchGetCommandLineW() {
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase == nullptr) {
        kernelbase = LoadLibraryW(L"kernelbase.dll");
    }
    if (kernelbase == nullptr) {
        RawTrace("cmdline: kernelbase not found");
        return;
    }

    void* target = reinterpret_cast<void*>(GetProcAddress(kernelbase, "GetCommandLineW"));
    if (target == nullptr) {
        RawTrace("cmdline: GetCommandLineW not found");
        return;
    }

    void* replacement = reinterpret_cast<void*>(&PatchedGetCommandLineW);
    unsigned char patch[12] = {0x48, 0xB8};
    memcpy(patch + 2, &replacement, sizeof(replacement));
    patch[10] = 0xFF;
    patch[11] = 0xE0;

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        RawTrace("cmdline: VirtualProtect failed");
        return;
    }
    memcpy(target, patch, sizeof(patch));
    VirtualProtect(target, sizeof(patch), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
    RawTrace("cmdline: kernelbase GetCommandLineW patched");
}

}  // namespace

std::wstring GetOwnModulePath() {
    static std::wstring cached;
    if (!cached.empty()) {
        return cached;
    }

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&fko::GetOwnModulePath), &module)) {
        return cached;
    }

    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(module, buffer, static_cast<DWORD>(_countof(buffer)));
    if (length > 0 && length < _countof(buffer)) {
        cached.assign(buffer, length);
    }
    return cached;
}

std::wstring GetOwnModuleDir() {
    static std::wstring cached = []() {
        const std::wstring path = GetOwnModulePath();
        const size_t pos = path.find_last_of(L"\\/");
        return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
    }();
    return cached;
}

bool IsKookProcess() {
    return g_isKookProcess;
}

bool IsBrowserProcess() {
    return g_isBrowserProcess;
}

bool IsFirstInstance() {
    return g_firstInstance;
}

void EarlyAttach() {
    // 同一进程里如果被加载了两份（人工注入等），只让第一份做事
    wchar_t mutexName[64] = {};
    swprintf_s(mutexName, L"Local\\FKO_KOOKCLEAN_%lu", GetCurrentProcessId());
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName);
    if (mutex != nullptr) {
        g_firstInstance = (GetLastError() != ERROR_ALREADY_EXISTS);
        // 故意不关闭句柄：保证进程存活期间该名字一直存在
    }

    if (!g_firstInstance) {
        return;
    }

    wchar_t exePath[MAX_PATH * 4] = {};
    const DWORD exeLength = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(_countof(exePath)));
    const wchar_t* exeName = (exeLength > 0 && exeLength < _countof(exePath)) ? wcsrchr(exePath, L'\\') : nullptr;
    if (exeName != nullptr) {
        ++exeName;
    }
    g_isKookProcess = (exeName != nullptr && _wcsicmp(exeName, L"KOOK.exe") == 0);

    const wchar_t* commandLine = GetCommandLineW();
    // 浏览器（Electron 主进程）：没有 --type= 子进程标记，也不是 “KOOK.exe xxx.js” 这种 node 脚本子进程
    g_isBrowserProcess = (commandLine != nullptr && wcsstr(commandLine, L"--type=") == nullptr &&
                          wcsstr(commandLine, L".js") == nullptr);

    if (g_isKookProcess && g_isBrowserProcess) {
        wchar_t switchText[64] = {};
        swprintf_s(switchText, L"--remote-debugging-port=%d", kDevToolsPort);
        AppendSwitchToCommandLine(switchText);
    }

    char report[300] = {};
    sprintf_s(report, _countof(report), "attach pid=%lu kook=%d browser=%d exe=%ls", GetCurrentProcessId(),
              g_isKookProcess ? 1 : 0, g_isBrowserProcess ? 1 : 0, exePath);
    RawTrace(report);
}

bool DebugLogEnabled() {
    return RawTraceEnabled();
}

void DebugLog(const char* format, ...) {
    if (!RawTraceEnabled()) {
        return;
    }
    char message[1000] = {};
    va_list arguments;
    va_start(arguments, format);
    vsnprintf_s(message, _countof(message), _TRUNCATE, format, arguments);
    va_end(arguments);
    RawTrace(message);
}

}  // namespace fko
