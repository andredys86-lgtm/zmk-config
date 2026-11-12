#define DT_DRV_COMPAT zmk_behavior_calc

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include "calc.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int on_calc_pressed(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    int layer = calc_key((uint8_t)binding->param1);
    if (layer >= 0) {
        zmk_keymap_layer_to(layer);
    }
    return 0;
}

static int on_calc_released(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    return 0;
}

static const struct behavior_driver_api behavior_calc_driver_api = {
    .binding_pressed = on_calc_pressed,
    .binding_released = on_calc_released,
};

#define CALC_INST(n)                                                                               \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_calc_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CALC_INST)
