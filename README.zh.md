# Tabby

[English](README.md) | [中文](README.zh.md)

M5Stack Tab5 上的便携 SSH / CLI 终端。基于 **ESP-IDF**，显示与触摸走官方 **M5Unified / M5GFX**，界面是 **LVGL 8.4**。主控为 ESP32-P4，Wi-Fi 由 ESP32-C6 协处理器通过 `esp_hosted` 提供。

开发、编译和工程结构见 [DEVELOPMENT.md](DEVELOPMENT.md)（英文）。

## 功能

- **交互式 SSH**：密码登录、`xterm-256color` PTY、窗口大小同步；ANSI/VT 终端可跑常见 shell、`vim`、`nano` 等
- **本机 CLI**：类 Linux 命令集（无管道、重定向或通配展开），覆盖 Wi-Fi、SSH、SD、诊断和 Python
- **VT 仿真**：UTF-8、宽字符、备用屏幕、回滚缓冲（约 2500 行）
- **设置页**：Wi-Fi、SSH、时间、SD 卡、外观、系统；配置写入 LittleFS，重启保留
- **键盘**：Tab5 I2C 键盘 + USB HID 键盘；US / JP 键位映射
- **SD 卡**：挂载、浏览、读写；USB 口可作为 U 盘暴露给电脑
- **本机 `vi`**：在 SD 上编辑文本（模态编辑器，非完整 Vim）
- **MicroPython**：REPL、`python -c`、从 SD 跑脚本；全局 `gfx` 对象画到全屏画布
- **时间**：按需 NTP、手动 UTC 偏移、可选按公网 IP 探测时区，并写回 RTC
- **显示**：Terminus 点阵字体（20–36 px）用于 ASCII；SSH/串口/CLI 中文走固件 efont，也可从 microSD 加载更全的点阵；设置页中文（简/繁）；PPA 加速刷屏；可调亮度

尚未移植：BLE HID 键盘、SSH 公钥登录、SCP。设备本身也不跑 Tailscale；要连 tailnet 主机，请让 Tab5 走带 subnet router / 网关的网络。

## 硬件

- [M5Stack Tab5](https://docs.m5stack.com/zh_CN/core/Tab5)（ESP32-P4，16 MB flash，32 MB PSRAM）
- Tab5 Keyboard（ExtPort1，I2C `0x6D`，SDA=GPIO0，SCL=GPIO1）
- microSD（可选，用于文件、`vi`、Python 脚本）
- USB 线（烧录、串口、USB 键盘或 U 盘模式）
- Tab5 能连上的 Wi-Fi 网络

## 快速开始

### 1. 烧录 Release（最省事）

每次打 `v*` tag，GitHub Actions 会用 ESP-IDF **5.4.4** 编 **esp32p4** 固件。从 [Releases](https://github.com/LeslieLeung/tabby/releases/latest) 下载 `tabby-*-flash.bin`，从地址 `0x0` 整片写入：

```bash
esptool.py --chip esp32p4 -p /dev/cu.usbmodem* write_flash 0x0 tabby-*-flash.bin
```

串口换成 Tab5 的 USB Serial/JTAG。可选中文字库：同一 Release 里的 `cjk16.bin`，拷到 microSD 的 `/fonts/cjk16.bin`。

### 2. 从源码编译

需要 ESP-IDF **5.4.4**（M5Unified / M5GFX 尚不支持 IDF 6.x）。用 [eim](https://github.com/espressif/idf-im-cli) 或 `export.sh` 均可，完整步骤见 [DEVELOPMENT.md](DEVELOPMENT.md)。

```bash
cd projects/tabby
eim select v5.4.4
eim run "idf.py set-target esp32p4" v5.4.4
eim run "idf.py build" v5.4.4
eim run "idf.py -p /dev/cu.usbmodem* flash monitor" v5.4.4
```

把 `/dev/cu.usbmodem*` 换成实际串口。macOS / Linux 常见是 `cu.usbmodem*` 或 `ttyACM*`。

### 3. 第一次使用

1. 烧录后重启 Tab5。
2. 按 **Esc** 打开设置（SSH 会话或 `vi` 进行中时，Esc 会发给远端 / 编辑器）。
3. 在 **Wi-Fi** 页扫描或添加网络并连接。
4. 在 **SSH** 页添加主机（名称、地址、端口、用户、密码、终端类型），点连接。
5. 再按 Esc 回到终端。

配置保存在 flash 的 LittleFS 里。不要把真实 Wi-Fi / SSH 密码提交进仓库；模板见 `data/profiles.json`。

也可以在本机 CLI 里连：

```text
wifi scan
wifi connect <ssid> [password]
ssh list
ssh connect 0
ssh user@host
ssh -p 2222 user@host
```

直接 `ssh user@host` 且未给密码时，会尝试复用已保存、且 host/user（或 host/user/port）相同的配置。

## 屏幕与按键

状态栏显示时间、Wi-Fi 和电量。齿轮图标打开设置。设置侧栏：

| 页 | 作用 |
| --- | --- |
| Wi-Fi | 开关、扫描、筛选、添加/编辑配置并连接 |
| SSH | 添加/编辑配置并连接；密码登录 |
| Time | NTP、UTC 偏移、自动时区、立即同步 |
| SD Card | 挂载 / 卸载 / 格式化；USB U 盘模式 |
| Appearance | 终端字号、背光 |
| System | 设备名、芯片、IDF、Flash、电池 |

| 操作 | 行为 |
| --- | --- |
| Esc | 终端 ↔ 设置；SSH / `vi` 中则发给应用 |
| 方向键 | CLI 历史与光标；SSH 中发给远端 |
| Ctrl+↑ / Ctrl+↓ | 终端回滚 |
| PageUp / PageDown | 翻页回滚（Shift/Ctrl+Page 一次滚很多） |
| 触摸拖动 | 终端回滚 |
| Ctrl+C | 中断当前 CLI / Python；SSH 中发给远端 |
| q 或 Ctrl+C | 图形 Python 脚本在 `gfx.present()` 时可中断 |

## 本机 CLI

内置命令是类 Linux 的，**不是**完整 POSIX shell。没有管道、重定向、通配或后台任务。`help` / `?` / `man` 可列出命令；`help <cmd>` 看用法。

```text
help
status
wifi status
ssh list
ssh connect 0
ls -lah /
cat /notes.txt
vi /notes.txt
df
wget https://example.com/file.txt /file.txt
python
python -c print('hello')
python /life.py
ping 1.1.1.1
time sync
sd usb on
```

`ls` 默认多列；`ls -l` 一行一个文件。SD 相关命令在卡未插入时不可用。

### 终端中文

ASCII、盒线绘制和 powerline 仍用内置 Terminus。中文（以及其他宽字符）来源：

1. **固件 efont**（与设置页相同的简/繁字体）。日常 `ls` 文件名、`vim`、SSH 输出一般够用，不需要 SD 卡。
2. **可选 SD 字库**，覆盖日文、韩文和更冷门的汉字。打包后的点阵**不进 git**（约 1.4 MB，由 [GNU Unifont](https://unifoundry.com/unifont/) 生成，SIL OFL 1.1）。用下面任一方式拿到文件，放到卡上的 `/fonts/cjk16.bin`，然后重启或重新挂载：

| 方式 | 命令 / 地址 |
| --- | --- |
| GitHub Release（推荐） | [最新 `cjk16.bin`](https://github.com/LeslieLeung/tabby/releases/latest/download/cjk16.bin) |
| 设备上（Wi-Fi + SD） | `mkdir /fonts` 再 `wget https://github.com/LeslieLeung/tabby/releases/latest/download/cjk16.bin /fonts/cjk16.bin` |
| 自己打包 | `python3 tools/pack_cjk_font.py -o cjk16.bin` |

可以用读卡器拷，或 `sd usb on` 从电脑拖进去。打 `v*` tag 会跑 `.github/workflows/firmware.yml`，编固件、打包 Unifont，并把 `cjk16.bin` 挂到 GitHub Release。只重打包字库用 `.github/workflows/cjk-font.yml`（手动触发），从 artifact 下载。

也可以放原始 Unifont `.hex`（`/fonts/unifont.hex` 或 `/unifont.hex`），开机解析更慢，优先用 `cjk16.bin`。

`status` 会显示 `cjk=firmware efont` 或 `cjk=sd /fonts/cjk16.bin n=…`。字库里没有、且按宽字符处理的码点会画成空方框。

### MicroPython 与绘图

```text
python                 # REPL；exit() 回到 Tabby
python -c <statement>
python /script.py [args...]
python --reset
```

脚本能拿到 `argv` 和全局 `gfx`。绘制发生在固件画布上，`gfx.present()` 才推到屏幕。

```python
w, h = gfx.size()
gfx.clear(0x0B1220)
gfx.rect(20, 20, 80, 40, 0x3D8BFF, 1)
gfx.text(20, 80, "hello", 0xE8EEF4)
gfx.present()
```

`gfx` 还提供 `pixel` / `line` / `circle` / `fill` / `mono`。脚本上限约 64 KB。

### USB U 盘

`sd usb on`（或设置页 SD Card）会暂停 USB 键盘主机，把 SD 卡暴露给电脑。拷完文件后 `sd usb off`，键盘才能重新枚举。

## Credits

Tabby 是 [Tab5_SSH_Client](https://github.com/airpocket-soundman/Tab5_SSH_Client) 的 ESP-IDF 重写，功能与交互大量参考该项目。感谢 [airpocket-soundman](https://github.com/airpocket-soundman)。

显示栈基于 [M5Unified](https://github.com/m5stack/M5Unified) / M5GFX 与 [LVGL](https://github.com/lvgl/lvgl)。SSH 使用 [libssh](https://www.libssh.org/)（LGPL-2.1-or-later）的 ESP-IDF 移植。本机 Python 是嵌入式 MicroPython。终端 ASCII 来自 Terminus；中文用 M5GFX efont，也可在 SD 上放 GNU Unifont。
