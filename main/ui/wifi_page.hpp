#pragma once

#include "tabby/keyboard_mapper.hpp"

#include "lvgl.h"

namespace tabby {

struct App;

namespace wifi_ui {

void initFonts();
const lv_font_t* font14();
const lv_font_t* font16();
const lv_font_t* font20();
void create(App& app, lv_obj_t* pane);
void setVisible(bool visible);
void refresh();
void poll();
bool handleKey(const KeyAction& action);

}  // namespace wifi_ui
}  // namespace tabby
