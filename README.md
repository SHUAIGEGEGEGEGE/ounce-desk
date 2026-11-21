# OunceDesk — Switch 2 (kaiHEN) 原生 Windows 3389 (RDP) + macOS (5900) 远程桌面客户端

**OunceDesk** 是一款纯运行在 **Nintendo Switch 2（kaiHEN）** 上的单文件自制软件（`OunceDesk.nro`），**电脑端无需安装任何第三方服务端**，即可直接连接：

1. **Windows 系统自带远程桌面（RDP，默认端口 `3389`）**
   * 原生实现 `MS-RDPBCGR`：X.224 协商 + **TLS 加密（mbedTLS）** + **Windows 10/11 默认 NLA / CredSSP（NTLMv2）认证** + 直连 TLS 模式
   * 支持 MCS/GCC、登录切桌面会话无缝重连（`Deactivate All` → `Confirm Active`）
   * 支持 **Fast-Path 高速位图输出 + 16/24/32bpp 交错 RLE 解压 + Surface Commands**
   * 支持 **Fast-Path 键鼠输入 + IBM PS/2 硬件扫描码 + UTF-16 Unicode 中文输入**
2. **macOS 系统自带远程桌面 / 屏幕共享（VNC / RFB 3.8，默认端口 `5900`）**
   * 支持 **Bonjour / mDNS（`_rfb._tcp.local`）局域网自动发现开启了屏幕共享的 Mac**
   * 同时支持 **Apple Remote Desktop（ARD，SecurityType 30，DH + MD5 + AES-128）**（直接用 macOS 账号密码登录）与 **传统 VNC 密码（SecurityType 2，位反转 DES）**（也兼容 Windows/Linux 的 TightVNC / TigerVNC）
   * 支持 **ZRLE（zlib 压缩分块）**、**CopyRect（零带宽拖拽滚动）**、**Raw** 与 **DesktopSize 动态分辨率**

---

## 为 Switch 2 打磨的主流级交互体验

* **三模输入**：
  * **触控板模式（默认，类 Jump Desktop）**：单指滑动带加速曲线、单指轻点左键、双指轻点右键、双指滑动平滑滚轮、双指捏合局部缩放（`1.0x–3.0x`）并拖拽平移。
  * **直触模式**：指哪点哪，按 `-` 键随时在触控板/直触模式间切换。
  * **手柄虚拟鼠标**：左摇杆带死区与三次曲线加速（**按住 `L` 触发精确狙击低速模式**）、右摇杆平滑滚轮、`A/ZR` 左键（支持按住拖拽）、`B/ZL` 右键、`Y` 中键、`X` 键呼出内置全功能虚拟 PC 键盘、十字键方向键。
* **USB / 蓝牙实体键鼠直通**：插上底座或 USB-C 键鼠后，自动透传 USB HID 键码与鼠标移动。
* **Windows / macOS 自适应快捷栏**：按 `L + R + (+)`、三指轻触或点击左上角 `MENU` 打开悬浮面板：
  * 连接 **Windows 3389** 自动显示：`Win` / `Alt+Tab` / `Ctrl+C` / `Ctrl+V` / `Ctrl+Z` / `Win+D` / `TaskMgr (Ctrl+Shift+Esc)`
  * 连接 **macOS 5900** 自动切换：`Cmd+Space` / `Cmd+Tab` / `Cmd+C` / `Cmd+V` / `Cmd+Z` / `Mission Control` / `Force Quit (Cmd+Opt+Esc)`
  * 支持调用 Switch 2 系统输入法（`swkbd`）直接向 Windows 或 macOS 输入中文。
* **kaiHEN 友好**：所有文件与配置目录均使用纯 ASCII 名称（`OunceDesk.nro`、`sdmc:/switch/OunceDesk/profiles.txt`），完全避免 OunceShell/FTP 不显示中文文件名的问题。

---

## 被控电脑准备（零第三方软件）

### 连接 Windows（3389）
1. 在 Windows 上打开 `设置 → 系统 → 远程桌面`，开启 **远程桌面**（默认端口 `3389`）。
2. 确保 Windows 账号已设置登录密码（推荐本地密码，微软账号如首次未用密码登过可先用密码登录一次本机）。
3. 在 Switch 2 打开 **OunceDesk** → 点击 **`+ Windows (3389)`** → 输入电脑局域网 IP、Windows 用户名和密码即可连接。

### 连接 macOS（5900）
1. 在 Mac 上打开 `系统设置 → 通用 → 共享`，开启 **屏幕共享**（默认端口 `5900`）。
2. 在 Switch 2 打开 **OunceDesk**，同局域网的 Mac 会通过 Bonjour 自动出现在列表中（也可点 **`+ macOS (5900)`** 手动添加）：
   * **方式 A（推荐，直接用 Mac 账号）**：填写你的 macOS 用户名与开机密码（走原生 Apple ARD 认证）。
   * **方式 B（纯 VNC 密码）**：在 macOS 屏幕共享的“电脑设置”里勾选“VNC 观察者可以使用密码控制屏幕”并设密码，Switch 2 端用户名留空、仅填该密码即可。

---

## 编译 `OunceDesk.nro`

### 方式一：GitHub Actions 云端自动编译（最省事）
将本目录推送到你的 GitHub 仓库，自动触发 `.github/workflows/build.yml`，完成后在 Actions 的 Artifacts 中直接下载 **`OunceDesk-Switch2-kaiHEN`**（内含 `OunceDesk.nro`）。

### 方式二：本地 Docker 一键编译
```bash
cd client-switch2
docker build -t ouncedesk-switch2-builder .
docker run --rm -v "$PWD:/src" -w /src ouncedesk-switch2-builder make -j4
```
产物位于 `client-switch2/OunceDesk.nro`。

### 方式三：已配置 devkitPro + `ouncebrew/ounce-devkit`
```bash
sudo dkp-pacman -S switch-sdl2 switch-sdl2_ttf switch-mbedtls switch-zlib
cd client-switch2
make -j4
```

### 安装到 Switch 2（kaiHEN）
将 `OunceDesk.nro` 复制到 SD 卡 `/switch/OunceDesk/OunceDesk.nro`，在 kaiHEN 的 `hbmenu` 中启动即可。
