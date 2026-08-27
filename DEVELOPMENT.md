# Tabby development

Notes for building, debugging, and changing this firmware. Product features and first-run steps are in [README.md](README.md) ([中文](README.zh.md)).

## Environment

- ESP-IDF **5.4.4** (`idf_component.yml` requires `>=5.4,<5.5`). M5Unified / M5GFX do not support IDF 6.x yet.
- Target **esp32p4**. Wi-Fi runs on the on-board ESP32-C6 through `esp_hosted` + `esp_wifi_remote` over **SDIO2** (pins in `sdkconfig.defaults`, not the ESP32-P4 Function EV defaults).
- Manage the IDF install with [eim](https://github.com/espressif/idf-im-cli), or `source $HOME/.espressif/v5.4.4/esp-idf/export.sh`.

Dependencies come from the IDF Component Manager (`main/idf_component.yml`):

| Component | Role |
| --- | --- |
| `m5stack/M5Unified` | Display, touch, power, RTC |
| `lvgl/lvgl` 8.4.0 | Settings UI |
| `bblanchon/arduinojson` | `profiles.json` |
| `joltwallet/littlefs` | Config partition |
| `espressif/esp_hosted` + `esp_wifi_remote` | Wi-Fi station on the P4 |
| `espressif/usb_host_hid` | USB keyboard |
| `espressif/esp_tinyusb` | SD card as a USB flash drive (MSC gadget) |
| `david-cermak/libssh` (`override_path`) | SSH / SCP; the local copy keeps `scp.c`, which upstream comments out |

Local components under `components/`: `settings`, `term`, `fonts`, `micropython_embed`, `libssh`.

## Build and flash

From `projects/tabby`:

```bash
eim select v5.4.4
eim run "idf.py set-target esp32p4" v5.4.4
eim run "idf.py build" v5.4.4
eim run "idf.py -p /dev/cu.usbmodem* flash monitor" v5.4.4
```

Or:

```bash
source "$HOME/.espressif/v5.4.4/esp-idf/export.sh"
idf.py set-target esp32p4
idf.py build
idf.py -p <PORT> flash monitor
```

## CI and releases

`.github/workflows/firmware.yml` builds on pull requests and `main` using [`espressif/esp-idf-ci-action`](https://github.com/espressif/esp-idf-ci-action) (`espressif/idf:v5.4.4`, target `esp32p4`). A `v*` tag also packs `cjk16.bin` and publishes a GitHub Release (merged flash image + split bins + font).

```bash
git tag v0.1.0
git push origin v0.1.0
```

Flash the merged asset with `esptool.py --chip esp32p4 -p <PORT> write_flash 0x0 tabby-v0.1.0-flash.bin`. Rebuild only the font with `.github/workflows/cjk-font.yml` (workflow_dispatch).

`sdkconfig` is generated from `sdkconfig.defaults`; do not commit it. The lock file is `dependencies.lock.esp32p4`.

The serial console is **USB Serial/JTAG** (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`). Avoid unnecessary DTR/RTS toggles in host tools; they can reset the board.

## Partitions

`partitions.csv`:

| Name | Type | Approx. size |
| --- | --- | --- |
| nvs | NVS | 20 KB |
| otadata | OTA data | 8 KB |
| factory | app | 6 MB |
| storage | LittleFS | ~10 MB |

The app image is large (libssh + MicroPython + fonts + LVGL). Config lives in `storage` as `profiles.json`. The template is `data/profiles.json`; do not bake real passwords into a LittleFS image.

## Layout

```text
main/                 App: BSP, Wi-Fi, SSH, CLI, UI, input, Python, SD, USB
main/cli/             Individual CLI commands
main/ui/              LVGL pages and terminal view
main/input/           Tab5 I2C + USB HID mapping
components/term/      VT emulator and terminal buffer
components/fonts/     Terminus bitmap fonts (terminal CJK uses efont + optional SD)
components/settings/  AppConfig and LittleFS store
components/libssh/    libssh ESP-IDF port (SCP enabled)
components/micropython_embed/
data/profiles.json    Factory profile template
tools/pack_cjk_font.py Pack GNU Unifont into /fonts/cjk16.bin for SD
sdkconfig.defaults    Board and performance Kconfig
```

`main/CMakeLists.txt` enables `-Wall -Wextra -Werror` (with a few third-party warnings ignored).

## Runtime

Startup in `app_main.cpp`: NVS → BSP → load config → keyboard / USB MSC / Python / SSH I/O task → background `tabby_net` (Wi-Fi connect, optional NTP, then SD probe so it does not race the C6 SDIO) → UI task.

| Module | Files | Notes |
| --- | --- | --- |
| Display | `bsp.cpp` | M5GFX framebuffer; async PPA blit, permanent CPU fallback on timeout |
| UI | `ui/ui.cpp` | LVGL settings + custom-drawn terminal; SSH session bypasses the LVGL widget tree |
| VT | `components/term` | CSI / OSC / UTF-8 / alternate screen / scrollback |
| SSH | `ssh_client.cpp` | Password auth; dedicated I/O task + TX/RX rings so a flush cannot stall LVGL |
| CLI | `cli.cpp` + `cli/` | Command registry; long jobs are queued and can be Ctrl+C'd |
| Python | `python_runner.cpp` | Embed VM; `gfx` draws to an RGB565 canvas via `\x1fGFX ...` lines |
| Keyboard | `input/` | I2C character mode at `0x6D`; USB HID boot keyboard; host paused while MSC is on |

CPU: main task pinned to CPU1 at 360 MHz, PSRAM hex 200 MHz. LVGL double-buffers at a 16 ms refresh. A `perf:` heartbeat logs frame/blit stats every 3 s.

## Serial and debug

`idf.py monitor` is enough. Boot logs keyboard enumeration and Wi-Fi/SD probe results. The UI task also prints periodic frame-rate and blit stats.

There is no separate serial command protocol. Use the on-device CLI (`status`, `keyboard`, `wifi status`, `free`, and so on).

## When changing code

- **Wi-Fi is station-only.** Scan/connect can block for seconds; keep that on `tabby_net` or the CLI queue, not in the LVGL loop.
- **USB is exclusive.** The MSC gadget and USB HID host cannot run together. `UsbMsc::start()` calls `pauseUsbHost()`.
- **SSH threading.** Call `ssh_channel_*` only from the SSH I/O task. The UI only touches the rings.
- **libssh override.** `main/idf_component.yml` points `override_path` at `components/libssh`. Upstream comments out SCP sources; the local CMake keeps them.
- **MicroPython.** Scripts are capped at 64 KB. Interrupts are polled via `tabby_interrupt` at points such as `gfx.present()`.
- **`vi`.** Mutually exclusive with SSH and a running Python job. Buffer cap is about 256 KB / 8000 lines.
- Do not commit `sdkconfig`, `managed_components/`, `build/`, `*.bin` font packs, or profiles that contain real credentials.

## Not ported / known gaps

- BLE HID keyboard (present in the Arduino original)
- SSH public-key / key-file login (password only)
- CLI pipelines, redirection, job control
- On-device Tailscale node
