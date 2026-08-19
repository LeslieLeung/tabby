#include "port/tabby_interrupt.h"

#include "py/runtime.h"

#include <stdatomic.h>

static atomic_bool g_irq = false;

void tabby_mp_request_interrupt(void) {
    atomic_store_explicit(&g_irq, true, memory_order_release);
}

void tabby_mp_clear_interrupt(void) {
    atomic_store_explicit(&g_irq, false, memory_order_release);
}

bool tabby_mp_interrupt_pending(void) {
    return atomic_load_explicit(&g_irq, memory_order_acquire);
}

void tabby_mp_poll(void) {
#if MICROPY_KBD_EXCEPTION
    if (atomic_exchange_explicit(&g_irq, false, memory_order_acq_rel)) {
        mp_sched_keyboard_interrupt();
    }
#endif
}
