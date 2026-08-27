#pragma once

namespace tabby {

struct BoardBsp;

// Boot splash. Painted with Terminus as soon as the panel is up, then replaced
// by an LVGL overlay so status can update while SD fonts load. Dismissed once
// bootstrap calls BootSplashFinish() and a short minimum time has elapsed.
void BootSplashShow(BoardBsp& bsp);
void BootSplashSetStatus(const char* text);
void BootSplashFinish();
void BootSplashCreateUi();
bool BootSplashVisible();
// Update the overlay label and delete it when bootstrap is done. Returns true
// if the overlay was just dismissed (caller should dirty the terminal).
bool BootSplashPoll();

}  // namespace tabby
