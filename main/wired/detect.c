#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <xtensa/hal.h>
#include <esp_intr_alloc.h>
#include <driver/gpio.h>
#include "../zephyr/types.h"
#include "../util.h"
#include "../adapter/adapter.h"
#include "detect.h"

static intr_handle_t intr_hdl;

static const uint8_t detect_pin_low[] = {
    19, 21, 22, 25,
};

static const uint8_t detect_pin_high[] = {
    32, 33, 34, 35
};

static const uint8_t system_id_low[] = {
    N64, GC, DC, WII_EXT
};

static const uint8_t system_id_high[] = {
    NES, PCE, PSX, GENESIS
};

static void IRAM_ATTR detect_intr(void* arg) {
    const uint32_t low_io = GPIO.acpu_int;
    const uint32_t high_io = GPIO.acpu_int1.intr;

    if (high_io) {
        if (wired_adapter.system_id == WIRED_NONE) {
            for (uint32_t i = 0; i < ARRAY_SIZE(detect_pin_high); i++) {
                if (high_io & BIT(detect_pin_high[i] - 32)) {
                    wired_adapter.system_id = system_id_high[i];
                }
            }
        }
        GPIO.status1_w1tc.intr_st = high_io;
    }
    if (low_io) {
        if (wired_adapter.system_id == WIRED_NONE) {
            for (uint32_t i = 0; i < ARRAY_SIZE(detect_pin_low); i++) {
                if (low_io & BIT(detect_pin_low[i])) {
                    wired_adapter.system_id = system_id_low[i];
                }
            }
        }
        GPIO.status_w1tc = low_io;
    }
}

void detect_init(void) {
    gpio_config_t io_conf = {0};

    for (uint32_t i = 0; i < ARRAY_SIZE(detect_pin_low); i++) {
        io_conf.intr_type = GPIO_PIN_INTR_ANYEDGE;
        io_conf.pin_bit_mask = BIT(detect_pin_low[i]);
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);
    }

    for (uint32_t i = 0; i < ARRAY_SIZE(detect_pin_high); i++) {
        io_conf.intr_type = GPIO_PIN_INTR_ANYEDGE;
        io_conf.pin_bit_mask = 1ULL << detect_pin_high[i];
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);
    }

    wired_adapter.system_id = WIRED_NONE;

    adapter_init_buffer(0);

    esp_intr_alloc(ETS_GPIO_INTR_SOURCE, ESP_INTR_FLAG_LEVEL3, detect_intr, NULL, &intr_hdl);
}

void detect_deinit(void) {
    esp_intr_free(intr_hdl);

    for (uint32_t i = 0; i < ARRAY_SIZE(detect_pin_low); i++) {
        gpio_reset_pin(detect_pin_low[i]);
    }
    for (uint32_t i = 0; i < ARRAY_SIZE(detect_pin_high); i++) {
        gpio_reset_pin(detect_pin_high[i]);
    }
}
