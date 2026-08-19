#pragma once

#include "lvgl.h"

namespace tabby {

struct App;

namespace system_ui {

void create(App& app, lv_obj_t* pane);
void setVisible(bool visible);
void refresh();
void poll();

}  // namespace system_ui
}  // namespace tabby
