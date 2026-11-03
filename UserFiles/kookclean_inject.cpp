#include "kookclean.h"

#include <detours/detours.h>
#include <tlhelp32.h>

#include <string>

namespace fko {
namespace {

typedef BOOL(WINAPI* CreateProcessWFn)(LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                                       LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                       LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles,
                                       DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
                                       LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);

CreateProcessWFn g_realCreateProcessW = nullptr;
bool g_hookInstalled = false;

std::wstring BaseName(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring DirName(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

bool ReadWholeFile(const std::wstring& path, std::string& out, size_t maxBytes) {
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

// 目录里的 version.dll 是不是我们自己这份（是我们这份就不需要注入，进程自己会加载）
bool DirectoryHasOurDll(const std::wstring& exePath) {
    const std::wstring dir = DirName(exePath);
    if (dir.empty()) {
        return false;
    }
    std::string content;
    if (!ReadWholeFile(dir + L"\\version.dll", content, 8u << 20)) {
        return false;
    }
    return content.find(kDllMarker) != std::string::npos;
}

struct ModuleSearch {
    const wchar_t* moduleName = nullptr;
    BYTE* baseAddress = nullptr;
    std::wstring path;
};

bool FindRemoteModule(DWORD processId, ModuleSearch& search) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (search.moduleName == nullptr ||
                _wcsicmp(entry.szModule, search.moduleName) == 0) {
                search.baseAddress = entry.modBaseAddr;
                search.path = entry.szExePath;
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

// 子进程里 kernel32 的基址可能和本进程不同（强制 ASLR 等），按偏移换算出远端 LoadLibraryW 地址
FARPROC ResolveRemoteLoadLibrary(DWORD processId, FARPROC localAddress) {
    ModuleSearch search;
    search.moduleName = L"kernel32.dll";
    if (!FindRemoteModule(processId, search) || search.baseAddress == nullptr) {
        return localAddress;
    }
    BYTE* localBase = reinterpret_cast<BYTE*>(GetModuleHandleW(L"kernel32.dll"));
    if (localBase == nullptr) {
        return localAddress;
    }
    return reinterpret_cast<FARPROC>(reinterpret_cast<BYTE*>(localAddress) + (search.baseAddress - localBase));
}

bool AlreadyInjected(DWORD processId) {
    ModuleSearch search;
    if (!FindRemoteModule(processId, search)) {
        return false;
    }
    const std::wstring own = GetOwnModulePath();
    if (own.empty()) {
        return false;
    }
    // FindRemoteModule 用 nullptr 名字时返回第一个模块，这里重新精确查找
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExePath, own.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool InjectLibrary(HANDLE process, DWORD processId) {
    const std::wstring dllPath = GetOwnModulePath();
    if (dllPath.empty()) {
        return false;
    }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == nullptr) {
        DebugLog("inject: VirtualAllocEx failed (%lu)", GetLastError());
        return false;
    }
    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
        DebugLog("inject: WriteProcessMemory failed (%lu)", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    FARPROC loadLibrary = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (loadLibrary == nullptr) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }
    loadLibrary = ResolveRemoteLoadLibrary(processId, loadLibrary);

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary),
                                       remote, 0, nullptr);
    if (thread == nullptr) {
        DebugLog("inject: CreateRemoteThread failed (%lu)", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return wait == WAIT_OBJECT_0;
}

std::wstring ResolveChildExePath(LPCWSTR applicationName, LPWSTR commandLine, LPCWSTR currentDirectory,
                                 DWORD processId) {
    wchar_t imagePath[MAX_PATH * 4] = {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process != nullptr) {
        DWORD size = static_cast<DWORD>(_countof(imagePath));
        const BOOL ok = QueryFullProcessImageNameW(process, 0, imagePath, &size);
        CloseHandle(process);
        if (ok && size > 0) {
            return std::wstring(imagePath, size);
        }
    }

    std::wstring path;
    if (applicationName != nullptr) {
        path = applicationName;
    } else if (commandLine != nullptr) {
        std::wstring text = commandLine;
        if (!text.empty() && text[0] == L'"') {
            const size_t end = text.find(L'"', 1);
            if (end != std::wstring::npos) {
                path = text.substr(1, end - 1);
            }
        } else {
            const size_t space = text.find(L' ');
            path = space == std::wstring::npos ? text : text.substr(0, space);
        }
    }
    if (path.empty()) {
        return path;
    }

    if (currentDirectory != nullptr && path.find(L':') == std::wstring::npos) {
        path = std::wstring(currentDirectory) + L"\\" + path;
    }
    wchar_t full[MAX_PATH * 4] = {};
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(_countof(full)), full, nullptr);
    if (length > 0 && length < _countof(full)) {
        return std::wstring(full, length);
    }
    return path;
}

BOOL WINAPI HookCreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                               LPSECURITY_ATTRIBUTES lpProcessAttributes,
                               LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles,
                               DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
                               LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation) {
    const BOOL result = g_realCreateProcessW(lpApplicationName, lpCommandLine, lpProcessAttributes,
                                             lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment,
                                             lpCurrentDirectory, lpStartupInfo, lpProcessInformation);

    if (!result || lpProcessInformation == nullptr || lpProcessInformation->hProcess == nullptr) {
        return result;
    }

    const DWORD processId = lpProcessInformation->dwProcessId;
    const std::wstring exePath =
        ResolveChildExePath(lpApplicationName, lpCommandLine, lpCurrentDirectory, processId);
    if (exePath.empty()) {
        return result;
    }
    if (_wcsicmp(BaseName(exePath).c_str(), L"KOOK.exe") != 0) {
        return result;  // 只管 KOOK 自己的子进程
    }
    if (DirectoryHasOurDll(exePath)) {
        return result;  // 目录里已经有本 DLL，子进程自己会加载
    }
    if (AlreadyInjected(processId)) {
        return result;
    }

    DebugLog("child %lu (%ls) dir has no version.dll, injecting", processId, exePath.c_str());
    if (InjectLibrary(lpProcessInformation->hProcess, processId)) {
        DebugLog("inject into %lu ok", processId);
    }
    return result;
}

}  // namespace

void InstallChildInjectionHook() {
    if (g_hookInstalled || !IsKookProcess()) {
        return;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == nullptr) {
        return;
    }
    g_realCreateProcessW = reinterpret_cast<CreateProcessWFn>(GetProcAddress(kernel32, "CreateProcessW"));
    if (g_realCreateProcessW == nullptr) {
        return;
    }

    if (DetourTransactionBegin() != NO_ERROR) {
        return;
    }
    DetourUpdateThread(GetCurrentThread());
    if (DetourAttach(reinterpret_cast<PVOID*>(&g_realCreateProcessW),
                     reinterpret_cast<PVOID>(&HookCreateProcessW)) != NO_ERROR) {
        DetourTransactionAbort();
        return;
    }
    if (DetourTransactionCommit() != NO_ERROR) {
        return;
    }

    g_hookInstalled = true;
    DebugLog("CreateProcessW hook installed (pid=%lu)", GetCurrentProcessId());
}

}  // namespace fko
