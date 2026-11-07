#define DT_DRV_COMPAT zmk_behavior_menu

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>
#include "prog.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int on_menu_pressed(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    prog_menu_event(binding->param1);
    return 0;
}

static int on_menu_released(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    return 0;
}

static const struct behavior_driver_api behavior_menu_driver_api = {
    .binding_pressed = on_menu_pressed,
    .binding_released = on_menu_released,
};

#define MENU_INST(n)                                                                               \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_menu_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MENU_INST)
