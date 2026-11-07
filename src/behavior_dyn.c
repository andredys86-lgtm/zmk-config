#define DT_DRV_COMPAT zmk_behavior_dyn

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>
#include "prog.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int on_dyn_pressed(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    uint32_t idx = binding->param1;
    if (idx >= PROG_KEYS) {
        return 0;
    }
    prog_play(idx);
    return 0;
}

static int on_dyn_released(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    return 0;
}

static const struct behavior_driver_api behavior_dyn_driver_api = {
    .binding_pressed = on_dyn_pressed,
    .binding_released = on_dyn_released,
};

#define DYN_INST(n)                                                                                \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_dyn_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DYN_INST)
