# FuckKookObs

![GitHub License](https://img.shields.io/github/license/SakuraKoi/FuckKookObs?style=for-the-badge) ![GitHub top language](https://img.shields.io/github/languages/top/SakuraKoi/FuckKookObs?style=for-the-badge) ![GitHub Release](https://img.shields.io/github/v/release/SakuraKoi/FuckKookObs?include_prereleases&style=for-the-badge) ![GitHub Downloads (all assets, all releases)](https://img.shields.io/github/downloads/SakuraKoi/FuckKookObs/total?style=for-the-badge)

Allow Kook voice chat while `OBS` or `Bilibili LiveHime` running

允许在 `OBS` 或 `B站直播姬` 运行的情况下使用Kook语音聊天

### 本仓库的附加功能（KookClean）

在原版“屏蔽 OBS / 直播姬”的基础上，本分支额外实现了：

1. **隐藏广告轮播** —— 运行时剔除 `广告1`（`.mixed-ad-carousel-wrapper` / `.kook-ad-image`），
   规则可在 `kook-adblock.txt` 里增删（`广告2` 的规则预留为注释，需要时去掉 `#`）；
2. **调试入口** —— 页面左上角一个黑红色小按钮“进入调试模式”，点一下开关 DevTools
   （等价于 KOOK 自带的隐藏热键 `Ctrl+Alt+Shift+0`）；
3. **更新跟随** —— Squirrel 每次更新会新建 `app-<版本号>` 目录，用 `dist\启动KOOK.exe`
   启动即可自动把 DLL 写进所有 `app-*` 目录再拉起 KOOK，不需要计划任务/管理员权限；
4. 窗口标题改为 `KOOK | Hook已加载`。

原理、编译、安装、加规则、调试与验收记录见 **[README-KookClean.md](README-KookClean.md)**；
可直接分发的文件在 `dist\`（`启动KOOK.exe` + `KookClean\` 里的 DLL 与规则）。

![image-20240525141645252](https://s2.loli.net/2024/05/25/ymq7H2fJtVLxgke.png)

> 致Kook的产品经理: 喜报! 您🐴在天上飞!

### 使用方式

[下载 Release](https://github.com/SakuraKoi/FuckKookObs/releases/latest/download/version.dll) 中的 `version.dll` 放入Kook安装目录下即可

**注意是放在 `app-版本号` 的子目录里面, 真正的Kook在那里面, 外面的 `KOOK.exe` 只是个启动器**

### 故障排查
1. 检查任务栏kook窗口是否出现 `HOOK已加载` / `已解锁` 字样（本分支显示 `KOOK | Hook已加载`）
2. 没有的话是没注入成功, Kook三天两头换程序编译架构 可能出现补丁架构不兼容, v2.1和v3两个版本都试试 \
   v2.1 `x86架构`: https://github.com/SakuraKoi/FuckKookObs/releases/download/v2-fix1/version.dll \
   v3 `amd64架构`: https://github.com/SakuraKoi/FuckKookObs/releases/download/v3/version.dll
3. 显示 `已解锁` 但还是失效的话发issue
> Q: 有没有可能整个同时兼容两个架构的补丁, 避免Kook更新换架构 补丁也要跟着换?\
> A: 没有 技术上不可能\
> &nbsp;&nbsp;&nbsp;&nbsp;正常人不会搞这种架构左右横跳换来换去的事情 显然Kook开发组里已经没有人类了
