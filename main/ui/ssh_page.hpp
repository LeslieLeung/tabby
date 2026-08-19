#pragma once

#include "tabby/keyboard_mapper.hpp"

#include "lvgl.h"

namespace tabby {

struct App;

namespace ssh_ui {

void create(App& app, lv_obj_t* pane);
void setVisible(bool visible);
void refresh();
void poll();
bool handleKey(const KeyAction& action);
void setPtySize(int columns, int rows);

}  // namespace ssh_ui
}  // namespace tabby
