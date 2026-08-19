#pragma once

#include "tabby/app_config.hpp"
#include "tabby/bsp.hpp"
#include "tabby/cli.hpp"
#include "tabby/keyboard_input.hpp"
#include "tabby/python_runner.hpp"
#include "tabby/sd_card.hpp"
#include "tabby/settings_store.hpp"
#include "tabby/ssh_client.hpp"
#include "tabby/terminal_buffer.hpp"
#include "tabby/terminal_emulator.hpp"
#include "tabby/time_sync.hpp"
#include "tabby/usb_msc.hpp"
#include "tabby/vi_editor.hpp"
#include "tabby/wifi_station.hpp"

namespace tabby {

enum class Screen : uint8_t {
    Terminal,
    Settings,
};

struct App {
    BoardBsp bsp;
    SettingsStore settings;
    AppConfig config;
    WifiStation wifi;
    SshClient ssh;
    PythonRunner python;
    KeyboardInput keyboard;
    SdCard sd;
    UsbMsc usb_msc;
    TimeSync time;
    TerminalBuffer terminal{2500};
    TerminalEmulator vt;
    ViEditor editor;
    Cli cli;
    Screen screen{Screen::Terminal};
};

bool UiStart(App& app);

}  // namespace tabby
