# Tabby

[English](README.md) | [中文](README.zh.md)

Portable SSH / CLI terminal for the M5Stack Tab5. Built with **ESP-IDF**. Display and touch go through official **M5Unified / M5GFX**; the settings UI is **LVGL 8.4**. The main chip is ESP32-P4; Wi-Fi comes from the ESP32-C6 coprocessor via `esp_hosted`.

Build, layout, and internals: [DEVELOPMENT.md](DEVELOPMENT.md).

## Features

- **Interactive SSH**: password login, `xterm-256color` PTY, window-size updates; ANSI/VT handling for shells, `vim`, `nano`, and similar apps
- **SCP**: copy files between an SSH host and the on-device microSD card
- **Local CLI**: Linux-like commands (no pipes, redirection, or globbing) for Wi-Fi, SSH, SD, diagnostics, and Python
- **VT emulator**: UTF-8, wide characters, alternate screen, ~2500-line scrollback
- **Settings**: Wi-Fi, SSH, time, SD card, appearance, system; stored on LittleFS across reboots
- **Keyboards**: Tab5 I2C keyboard and USB HID keyboards; US / JP layout mapping
- **SD card**: mount, browse, read/write; the USB port can expose the card as a flash drive
- **On-device `vi`**: modal text editor for SD files (not a full Vim)
- **MicroPython**: REPL, `python -c`, scripts from SD; a global `gfx` object draws to a full-screen canvas
- **Time**: on-demand NTP, manual UTC offset, optional public-IP timezone detection, written back to the RTC
- **Display**: Terminus bitmap fonts (20–36 px) for ASCII; CJK in SSH/serial/CLI from firmware efont, or a fuller bitmap on microSD; CJK in the settings UI (Simplified / Traditional); PPA-accelerated flushes; adjustable brightness

Not ported yet: BLE HID keyboards and SSH public-key login. The firmware does not run a Tailscale node; to reach a tailnet host, put the Tab5 on a network that already has a subnet router or gateway.

## Hardware

- [M5Stack Tab5](https://docs.m5stack.com/en/core/Tab5) (ESP32-P4, 16 MB flash, 32 MB PSRAM)
- Tab5 Keyboard (ExtPort1, I2C `0x6D`, SDA=GPIO0, SCL=GPIO1)
- microSD (optional; files, `vi`, Python scripts, SCP)
- USB cable (flashing, serial, USB keyboard, or USB-drive mode)
- A Wi-Fi network the Tab5 can reach

## Quick start

### 1. Flash a release (easiest)

GitHub Actions builds ESP-IDF **5.4.4** firmware for **esp32p4** on each `v*` tag. Download `tabby-*-flash.bin` from [Releases](https://github.com/LeslieLeung/tabby/releases/latest) and write the whole image at offset `0x0`:

```bash
esptool.py --chip esp32p4 -p /dev/cu.usbmodem* write_flash 0x0 tabby-*-flash.bin
```

Replace the port with the Tab5 USB Serial/JTAG device. Optional CJK font: `cjk16.bin` on the same release, copied to `/fonts/cjk16.bin` on the microSD card.

### 2. Build from source

ESP-IDF **5.4.4** is required (M5Unified / M5GFX do not support IDF 6.x yet). Use [eim](https://github.com/espressif/idf-im-cli) or `export.sh`; full steps are in [DEVELOPMENT.md](DEVELOPMENT.md).

```bash
cd projects/tabby
eim select v5.4.4
eim run "idf.py set-target esp32p4" v5.4.4
eim run "idf.py build" v5.4.4
eim run "idf.py -p /dev/cu.usbmodem* flash monitor" v5.4.4
```

Replace `/dev/cu.usbmodem*` with the actual port. On macOS / Linux that is often `cu.usbmodem*` or `ttyACM*`.

### 3. First use

1. Reboot the Tab5 after flashing.
2. Press **Esc** to open settings (during an SSH session or `vi`, Esc is sent to the remote app / editor).
3. On the **Wi-Fi** page, scan or add a network and connect.
4. On the **SSH** page, add a host (name, address, port, user, password, terminal type) and connect.
5. Press Esc again to return to the terminal.

Profiles are stored on LittleFS in flash. Do not commit real Wi-Fi or SSH passwords; the template is `data/profiles.json`.

You can also connect from the local CLI:

```text
wifi scan
wifi connect <ssid> [password]
ssh list
ssh connect 0
ssh user@host
ssh -p 2222 user@host
```

If a direct `ssh user@host` omits the password, the firmware tries to reuse credentials from a saved profile with the same host/user (or host/user/port).

## Screens and keys

The status bar shows time, Wi-Fi, and battery. The gear icon opens settings. Sidebar pages:

| Page | What it does |
| --- | --- |
| Wi-Fi | On/off, scan, filter, add/edit profiles, connect |
| SSH | Add/edit profiles and connect; password login |
| Time | NTP, UTC offset, auto timezone, sync now |
| SD Card | Mount / unmount / format; USB flash-drive mode |
| Appearance | Terminal font size, backlight |
| System | Device name, chip, IDF, flash, battery |

| Input | Action |
| --- | --- |
| Esc | Terminal ↔ settings; during SSH / `vi`, forwarded to the app |
| Arrow keys | CLI history and cursor; during SSH, sent to the remote |
| Ctrl+Up / Ctrl+Down | Terminal scrollback |
| PageUp / PageDown | Page scrollback (Shift/Ctrl+Page jumps farther) |
| Touch drag | Terminal scrollback |
| Ctrl+C | Interrupt the current CLI / Python job; during SSH, sent to the remote |
| q or Ctrl+C | Interrupt a graphics Python script at `gfx.present()` |

## Local CLI

The built-in CLI is Linux-like, **not** a full POSIX shell. There are no pipelines, redirection, globbing, or background jobs. `help` / `?` / `man` lists commands; `help <cmd>` shows usage.

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
scp get /home/demo/test.py /test.py 0
scp put /test.py /home/demo/test.py 0
wget https://example.com/file.txt /file.txt
python
python -c print('hello')
python /life.py
ping 1.1.1.1
time sync
sd usb on
```

Plain `ls` uses multi-column output; `ls -l` prints one file per line. SD commands are unavailable when no card is inserted.

### Terminal CJK

ASCII, box drawing, and powerline stay on the built-in Terminus font. Chinese (and other wide characters) are drawn from:

1. **Firmware efont** (same Simplified/Traditional face as the settings UI). Enough for typical `ls` names, `vim`, and SSH output. No SD card required.
2. **Optional SD font** for more glyphs (Japanese, Korean, rarer CJK). The packed bitmap is **not** in git (~1.4 MB, generated from [GNU Unifont](https://unifoundry.com/unifont/), SIL OFL 1.1). Get it one of these ways, then put it on the card as `/fonts/cjk16.bin` and reboot (or remount):

| How | Command / URL |
| --- | --- |
| GitHub Release (preferred) | [latest `cjk16.bin`](https://github.com/LeslieLeung/tabby/releases/latest/download/cjk16.bin) |
| On the device (Wi-Fi + SD) | `mkdir /fonts` then `wget https://github.com/LeslieLeung/tabby/releases/latest/download/cjk16.bin /fonts/cjk16.bin` |
| Build it yourself | `python3 tools/pack_cjk_font.py -o cjk16.bin` |

Copy via a card reader, or `sd usb on` and drop the file from a computer. A `v*` tag runs `.github/workflows/firmware.yml`, which builds firmware, packs Unifont, and attaches `cjk16.bin` to the GitHub Release. Rebuild only the font with `.github/workflows/cjk-font.yml` (workflow_dispatch) and download the artifact.

A raw Unifont `.hex` also works (`/fonts/unifont.hex` or `/unifont.hex`); it is slower to parse at boot. Prefer `cjk16.bin`.

`status` prints `cjk=firmware efont` or `cjk=sd /fonts/cjk16.bin n=…`. Missing glyphs that the emulator treats as double-width render as an empty box.

### MicroPython and graphics

```text
python                 # REPL; exit() returns to Tabby
python -c <statement>
python /script.py [args...]
python --reset
```

Scripts receive `argv` and a global `gfx` object. Drawing goes into the firmware canvas; `gfx.present()` pushes it to the display.

```python
w, h = gfx.size()
gfx.clear(0x0B1220)
gfx.rect(20, 20, 80, 40, 0x3D8BFF, 1)
gfx.text(20, 80, "hello", 0xE8EEF4)
gfx.present()
```

`gfx` also provides `pixel` / `line` / `circle` / `fill` / `mono`. Scripts are capped at about 64 KB.

### USB flash drive

`sd usb on` (or the SD Card settings page) pauses the USB keyboard host and exposes the SD card to a computer. Run `sd usb off` when you are done so the keyboard can enumerate again.

## Credits

Tabby is an ESP-IDF rewrite of [Tab5_SSH_Client](https://github.com/airpocket-soundman/Tab5_SSH_Client). Features and interaction follow that project closely. Thanks to [airpocket-soundman](https://github.com/airpocket-soundman).

The display stack is [M5Unified](https://github.com/m5stack/M5Unified) / M5GFX and [LVGL](https://github.com/lvgl/lvgl). SSH/SCP uses the ESP-IDF port of [libssh](https://www.libssh.org/) (LGPL-2.1-or-later). Local Python is embedded MicroPython. Terminal ASCII comes from Terminus; CJK uses M5GFX efont and optional GNU Unifont on SD.
