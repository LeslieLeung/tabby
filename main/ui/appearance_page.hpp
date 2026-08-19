#pragma once

#include "lvgl.h"

#include <cstdint>

namespace tabby {

struct App;

namespace appearance_ui {

void create(App& app, lv_obj_t* pane, void (*apply_font)(uint8_t height));
void setVisible(bool visible);
void refresh();

}  // namespace appearance_ui
}  // namespace tabby
