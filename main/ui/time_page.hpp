#pragma once

#include "lvgl.h"

namespace tabby {

struct App;

namespace time_ui {

void create(App& app, lv_obj_t* pane);
void setVisible(bool visible);
void refresh();
void poll();

}  // namespace time_ui
}  // namespace tabby
