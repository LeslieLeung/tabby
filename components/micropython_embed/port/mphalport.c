#include "port/tabby_interrupt.h"
#include "py/mphal.h"
#include "port/micropython_host_io.h"

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) {
    micropython_host_stdout(str, len);
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    micropython_host_stdout(str, len);
    return len;
}

void mp_hal_delay_ms(mp_uint_t ms) {
    const TickType_t start = xTaskGetTickCount();
    const TickType_t wait = pdMS_TO_TICKS(ms ? ms : 1);
    while ((xTaskGetTickCount() - start) < wait) {
        tabby_mp_poll();
        vTaskDelay(1);
    }
}

void mp_hal_delay_us(mp_uint_t us) {
    esp_rom_delay_us(us);
}

mp_uint_t mp_hal_ticks_ms(void) {
    return (mp_uint_t)(esp_timer_get_time() / 1000ULL);
}

mp_uint_t mp_hal_ticks_us(void) {
    return (mp_uint_t)esp_timer_get_time();
}

mp_uint_t mp_hal_ticks_cpu(void) {
    return (mp_uint_t)esp_timer_get_time();
}
