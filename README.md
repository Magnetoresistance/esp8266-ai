<p align="center">
  <img src="docs/images/logo.svg" width="72" alt="logo">
</p>

<h1 align="center">AI Mac 小屏幕</h1>

<p align="center">桌上的一台 AI 状态小电脑 —— ESP8266 · 开源硬件 · 桌面伴侣</p>

<p align="center">
  中文 ·
  <a href="README.en.md">English</a>
</p>

> [!IMPORTANT]
> 本项目基于原作者 **[@pengchujin](https://github.com/pengchujin)** 的开源项目 [**pengchujin/esp8266-ai**](https://github.com/pengchujin/esp8266-ai) 二次开发而来，**衷心感谢原作者的开源贡献**！
> 原项目面向 240×240 的「SD2 小电视」；本项目将其适配到 **128×128 GC9107（0.85"）屏**，并新增屏幕镜像、照片相册、音乐频谱等功能，详见下方[「相对原项目的改动」](#相对原项目的改动)。刷机请用 PlatformIO 自行编译（原项目的网页刷机包与本硬件不通用）。

<p align="center">
  <a href="https://mac.qust.me">官网</a> ·
  <a href="https://mac.qust.me/#flash">网页刷机</a> ·
  <a href="https://github.com/pengchujin/esp8266-ai/releases/latest">下载</a>
</p>

<p align="center">
  <img src="docs/images/hero.jpg" width="640" alt="AI Mac 小屏幕">
</p>

一块 240×240 的复古小电视，放在桌上实时显示 **Claude Code / Codex CLI 在干什么、额度还剩多少**。不需要任何 API key：数据来自本机已有的 CLI 登录凭据和会话日志，由配套的 Mac / Windows 桥接程序在局域网内提供给设备。

## 功能

| | |
|---|---|
| <img src="docs/images/feature1.jpg" width="360" alt="AI 工作状态"> | **AI 工作状态与额度**<br>桌宠动起来 = AI 正在干活。方形进度环 + 大字显示 5 小时 / 周额度的真实用量；额度用满自动换成重置倒计时，等你审批时整圈边框红闪提醒。 |
| <img src="docs/images/feature2.jpg" width="360" alt="网速监视"> | **网速实时监视**<br>任务管理器风格的上下行曲线，56 秒滚动窗口，量程自动调整。 |
| <img src="docs/images/music.jpg" width="360" alt="音乐播放"> | **音乐播放显示**<br>专辑封面、歌名、歌手、进度条实时同步；音乐响起自动切入，停止自动切回。 |
| <img src="docs/images/feature3.jpg" width="360" alt="桌宠可换"> | **可换桌宠**<br>内置 [petdex.dev](https://petdex.dev) 画廊 3300+ 开源桌宠，也可上传任意 GIF，设备板上直接解码，无需重烧固件。 |

## 相对原项目的改动

- **屏幕适配 128×128 GC9107**：原工程为 240×240 ST7789；本项目改用 ST7789_2 驱动 GC9107（BGR 色序、反色、CGRAM 偏移在固件内处理），支持 0-7 旋转并持久化
- **屏幕镜像**：Windows 桥接新增 `ScreenMirrorService`，把 PC 全屏 / 指定窗口 / 鼠标框选区域实时镜像到小屏（行流式帧传输）
- **照片相册**：Windows 桥接新增 `AlbumService`，本地目录幻灯片轮播，支持间隔 / 顺序 / 排序 / 适配方式设置与上一张 / 下一张 / 随机切换
- **音乐频谱**：Windows 桥接 `SpectrumService` 通过 WASAPI loopback 抓取系统音频做 24 段 FFT，设备端绘制柱状 / 波形 / 环形三大类型十余种效果（只输出频谱数据，不录制不存储音频）
- **股票页优化**：中文名条带放大（固件 2x 下采样显示更清晰）、股票代码与中文名垂直居中对齐
- **其他**：背光极性可反转、亮度 PWM、显示旋转持久化（`/api/rotation`）、`/api/brightness-invert`、`/api/music-spectrum` 等新端点；1MB flash 变体环境

## 快速上手

需要的东西：一台「SD2 小电视」开发板（[开源硬件](https://oshwhub.com/q21182889/sd2)，也可[直接购买成品](https://mobile.yangkeduo.com/goods.html?ps=OuBjGMWE82)）、一根 USB **数据**线。

### 第 1 步 · 刷固件（约 30 秒）

用 Chrome / Edge 打开 **[mac.qust.me/#flash](https://mac.qust.me/#flash)**，USB 连接设备，点「连接设备并烧录」，选择串口等待完成即可，无需安装任何工具。

> 弹窗里看不到串口？Windows 需要装 [CH340 驱动](https://www.wch.cn/downloads/CH341SER_EXE.html)，Mac 系统自带无需安装；换根 USB 线（很多线只能充电）；更多排查见[官网 FAQ](https://mac.qust.me/#flash-faq)。
>
> 命令行党也可以用 esptool 把 [Releases](https://github.com/pengchujin/esp8266-ai/releases/latest) 里的 `esp8266-ai-firmware-*.bin` 刷到 `0x0`。

### 第 2 步 · 配 WiFi

设备首次开机会开热点 **`AI-Clock-Setup`**：手机连上后自动弹出配网页（没弹就用浏览器打开 `192.168.4.1`），选择家里 WiFi、输入密码，完成。

### 第 3 步 · 装桥接程序

从 [Releases](https://github.com/pengchujin/esp8266-ai/releases/latest) 下载并打开：

- **macOS**：`AIClockBridge-*-macOS.dmg`，拖入 Applications（ad-hoc 签名，首次启动需在「系统设置 → 隐私与安全性」允许，并同意本地网络权限）
- **Windows**：`AIClockBridge-*-Windows-x64.exe`，双击即用

桥接程序常驻菜单栏 / 托盘，会**自动发现并配对**同一局域网内的设备——到这里屏幕就活了。

<p align="center">
  <img src="docs/images/working.jpg" width="640" alt="工作演示">
</p>

日常使用都在托盘图标上：**左键**打开设备画面的实时镜像（底部有屏幕亮度滑条），**右键**是完整菜单（额度详情、屏幕切换、更换桌宠、音乐/网速页等）。

## 常见问题

- **屏幕边框红色闪烁**：设备连不上桥接程序——确认电脑端程序在运行、和设备在同一 WiFi。
- **额度一直显示 `-`**：本机没有登录过 Claude Code / Codex CLI，桥接程序读不到凭据。
- **想换桌宠**：右键托盘图标 → 「更换桌宠动画…」，挑一个点上传就行。

## 开发

```
firmware/     ESP8266 固件（PlatformIO + Arduino，含板上 GIF 解码）
mac-app/      macOS 菜单栏桥接（Swift/SPM，零第三方依赖）
windows-app/  Windows 托盘桥接（C# / .NET 8 WinForms）
tools/        GIF → RGB565 内置精灵图转换脚本
docs/         开发文档（硬件引脚、HTTP API、架构细节）
```

```bash
cd firmware && pio run -t upload   # 固件：编译 + USB 烧录
cd mac-app && swift run            # Mac 桥接：本地跑起来
```

硬件引脚表、屏幕驱动的坑、设备 HTTP API、GIF 板上解码架构等细节见 **[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)**。

硬件、固件、软件全部开源，拿去改、拿去做、拿去卖都行。

## 致谢

- 本项目 fork 自 **[@pengchujin](https://github.com/pengchujin)** 的 [esp8266-ai](https://github.com/pengchujin/esp8266-ai)：固件架构、Mac/Windows 桥接程序、配网方案与本文档的基础内容均出自原作者之手，**感谢原作者的开源精神**！原项目官网 [mac.qust.me](https://mac.qust.me) 提供网页刷机、FAQ 与成品购买。
- 依赖的开源组件：[TFT_eSPI](https://github.com/bodmer/TFT_eSPI)、[WiFiManager](https://github.com/tzapu/WiFiManager)、[ArduinoJson](https://github.com/bblanchon/ArduinoJson)、[AnimatedGIF](https://github.com/bitbank2/AnimatedGIF)、[NAudio](https://github.com/naudio/NAudio) 等。
- 上游项目的后续更新欢迎关注原作者仓库；本仓库的改动见[「相对原项目的改动」](#相对原项目的改动)。
