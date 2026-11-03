// KOOK 启动器（KookClean）
//
// 作用：双击本 exe 时
//   1. 把同目录 KookClean\ 里的 version.dll 与 kook-adblock.txt 复制进每一个
//      app-<版本>\ 目录（只认里面已经有 KOOK.exe 的目录，避免写进解压到一半的目录）；
//   2. 然后启动同目录的官方 KOOK.exe（Squirrel 启动器），命令行参数原样透传。
//
// 编译：tools\build-launcher.cmd（MSVC x64，/utf-8）
// 依赖：无（只用 kernel32/user32），x64 静态运行时，单个 exe 可直接分发。

#include <windows.h>
#include <tlhelp32.h>

#include <string>
#include <vector>

namespace {

const wchar_t kPayloadDir[] = L"KookClean";
const wchar_t kDllName[] = L"version.dll";
const wchar_t kConfigName[] = L"kook-adblock.txt";
const wchar_t kLogName[] = L"launcher.log";

std::wstring ExePath() {
    wchar_t buffer[MAX_PATH * 2] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(_countof(buffer)));
    if (length == 0 || length >= _countof(buffer)) {
        return std::wstring();
    }
    return std::wstring(buffer, length);
}

std::wstring DirName(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsVersionDirName(const std::wstring& name) {
    if (name.size() < 5 || name.compare(0, 4, L"app-") != 0) {
        return false;
    }
    return name[4] >= L'0' && name[4] <= L'9';
}

bool ProcessRunning(const wchar_t* name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

void ClearReadOnly(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    }
}

void Log(HANDLE log, const std::wstring& line) {
    if (log == INVALID_HANDLE_VALUE) {
        return;
    }
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    wchar_t stamp[64] = {};
    wsprintfW(stamp, L"[%04u-%02u-%02u %02u:%02u:%02u] ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
              now.wSecond);
    const std::wstring text = std::wstring(stamp) + line + L"\r\n";
    DWORD written = 0;
    WriteFile(log, text.c_str(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &written, nullptr);
    FlushFileBuffers(log);
}

void Notice(const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), L"KOOK 启动器", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}

// 取本启动器自己的额外参数（跳过 argv[0]），以便原样透传给 KOOK.exe
std::wstring ExtraArguments() {
    const std::wstring raw = GetCommandLineW();
    size_t index = 0;
    if (index < raw.size() && raw[index] == L'"') {
        index = raw.find(L'"', index + 1);
        if (index == std::wstring::npos) {
            return std::wstring();
        }
        ++index;
    } else {
        index = raw.find(L' ');
        if (index == std::wstring::npos) {
            return std::wstring();
        }
    }
    while (index < raw.size() && raw[index] == L' ') {
        ++index;
    }
    return raw.substr(index);
}

// 把一个 payload 文件复制进目标目录；返回 true 表示这次确实写入过
bool InstallFile(const std::wstring& source, const std::wstring& target, HANDLE log, std::wstring* error) {
    if (!FileExists(source)) {
        return false;
    }
    ClearReadOnly(target);
    if (CopyFileW(source.c_str(), target.c_str(), FALSE)) {
        Log(log, L"copied -> " + target);
        return true;
    }
    const DWORD code = GetLastError();
    wchar_t buffer[128] = {};
    wsprintfW(buffer, L" (error %lu)", code);
    *error = target + buffer;
    Log(log, L"copy failed -> " + target + buffer);
    return false;
}

int Run() {
    const std::wstring exePath = ExePath();
    if (exePath.empty()) {
        Notice(L"无法取得启动器自身路径。");
        return 1;
    }
    const std::wstring root = DirName(exePath);
    const std::wstring kookExe = root + L"\\KOOK.exe";
    const std::wstring payload = root + L"\\" + kPayloadDir;

    if (ProcessRunning(L"KOOK.exe")) {
        Notice(L"KOOK 已经在运行了。\n\n要重新装一次 DLL，请先完全退出 KOOK 再双击本启动器。");
        return 0;
    }
    if (!FileExists(kookExe)) {
        Notice(L"同目录下没有找到 KOOK.exe。\n\n请把本启动器放进 KOOK 安装目录（里面有 KOOK.exe 和 app-x.y.z 文件夹）再运行。\n\n当前目录：\n" +
               root);
        return 1;
    }
    if (!FileExists(payload + L"\\" + kDllName)) {
        Notice(L"没有找到 " + payload + L"\\" + kDllName + L"。\n\n请把完整的 KookClean 文件夹放在启动器同目录下。");
        return 1;
    }

    const HANDLE log = CreateFileW((payload + L"\\" + kLogName).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Log(log, L"launcher start, root=" + root);

    // 逐个 app-<版本> 目录安装
    int installed = 0;
    std::vector<std::wstring> failures;
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW((root + L"\\app-*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        Log(log, L"no app-* folder found");
        if (log != INVALID_HANDLE_VALUE) {
            CloseHandle(log);
        }
        Notice(L"没有找到 app-x.y.z 文件夹。\n\nKOOK 可能还没解压完，请稍后重试；如果一直这样，说明这个目录不是 KOOK 安装目录。");
        return 1;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        const std::wstring name = data.cFileName;
        if (!IsVersionDirName(name)) {
            continue;
        }
        const std::wstring dir = root + L"\\" + name;
        if (!FileExists(dir + L"\\KOOK.exe")) {
            Log(log, L"skip (no KOOK.exe): " + dir);
            continue;
        }
        std::wstring error;
        const bool dll = InstallFile(payload + L"\\" + kDllName, dir + L"\\" + kDllName, log, &error);
        const bool config = InstallFile(payload + L"\\" + kConfigName, dir + L"\\" + kConfigName, log, &error);
        if (dll || config) {
            ++installed;
        } else if (!error.empty()) {
            failures.push_back(error);
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);

    Log(log, L"install done, dirs touched=" + std::to_wstring(installed) + L", failures=" +
                 std::to_wstring(failures.size()));
    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log);
    }
    if (!failures.empty() && installed == 0) {
        std::wstring text = L"DLL 写入失败，通常是 KOOK 还在运行占着文件。\n\n请完全退出 KOOK（任务管理器里确认没有 KOOK.exe）后重试。\n\n失败的路径：\n";
        for (size_t i = 0; i < failures.size() && i < 6; ++i) {
            text += failures[i] + L"\n";
        }
        Notice(text);
        return 1;
    }

    // 启动官方启动器，参数透传
    const std::wstring commandLine = L"\"" + kookExe + L"\" " + ExtraArguments();
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info = {};
    std::wstring mutableCommand = commandLine;
    const BOOL ok = CreateProcessW(kookExe.c_str(), &mutableCommand[0], nullptr, nullptr, FALSE, 0, nullptr, root.c_str(),
                                   &startup, &info);
    if (!ok) {
        wchar_t buffer[64] = {};
        wsprintfW(buffer, L" (error %lu)", GetLastError());
        Notice(L"启动 KOOK.exe 失败" + std::wstring(buffer));
        return 1;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return Run(); }
