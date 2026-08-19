#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void tabby_mp_request_interrupt(void);
void tabby_mp_clear_interrupt(void);
bool tabby_mp_interrupt_pending(void);
void tabby_mp_poll(void);

#ifdef __cplusplus
}
#endif
