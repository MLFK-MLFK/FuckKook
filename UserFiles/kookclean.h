#pragma once
#include <Windows.h>
#include <string>

// FuckKookObs + KookClean 公共接口
// 说明：本 DLL 以 version.dll 代理的方式被 KOOK 的每一个子进程加载（app 目录里的 version.dll），
//       所以这里拿到的“进程角色”信息全部来自命令行/文件名。

namespace fko {

// KOOK 主进程命令行会被追加 --remote-debugging-port=<该端口>，DLL 通过该端口用 CDP 注入脚本
const int kDevToolsPort = 39217;

// 用来识别磁盘上的 version.dll 是不是我们自己这份（自动注入前的自检）
extern const char kDllMarker[];

bool IsKookProcess();     // 可执行文件名是否 KOOK.exe
bool IsBrowserProcess();  // 命令行里没有 --type=（Electron 主进程）
bool IsFirstInstance();   // 本进程里是否第一次加载本 DLL

// DllMain 中同步调用：判定进程角色、给主进程命令行追加调试端口
void EarlyAttach();

// 所有 KOOK 进程：hook CreateProcessW，子进程目录里没有本 DLL 时自动注入
void InstallChildInjectionHook();

// 仅主进程：启动 CDP 客户端（广告剔除 + 左上角“进入调试模式”按钮 + 点击切换 DevTools）
void StartBrowserFeatures();

std::wstring GetOwnModulePath();
std::wstring GetOwnModuleDir();

// 调试日志：仅当 %TEMP%\kookclean-debug.txt 存在时才写入 %TEMP%\FuckKookObs.log
bool DebugLogEnabled();
void DebugLog(const char* format, ...);

}  // namespace fko
