#include "shared.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(shared, LOG_LEVEL_INF);

K_SEM_DEFINE(button_sem, 0, K_SEM_MAX_LIMIT);
K_SEM_DEFINE(graphics_ready_sem, 0, K_SEM_MAX_LIMIT);
atomic_t button_irq_count;
atomic_t button_handled_count;

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
#define LED2_NODE DT_ALIAS(led2)
#define SW0_NODE DT_ALIAS(sw0)

#if !DT_NODE_HAS_STATUS_OKAY(LED0_NODE)
#error "Unsupported board: led0 devicetree alias is not defined"
#endif

#if !DT_NODE_HAS_STATUS_OKAY(LED1_NODE)
#error "Unsupported board: led1 devicetree alias is not defined"
#endif

#if !DT_NODE_HAS_STATUS_OKAY(LED2_NODE)
#error "Unsupported board: led2 devicetree alias is not defined"
#endif

#if !DT_NODE_HAS_STATUS_OKAY(SW0_NODE)
#error "Unsupported board: sw0 devicetree alias is not defined"
#endif

const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(SW0_NODE, gpios, {0});
struct gpio_callback button_cb_data;

const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

const struct led led0 = {
    .spec = GPIO_DT_SPEC_GET_OR(LED0_NODE, gpios, {0}),
    .num = 0,
};

const struct led led1 = {
    .spec = GPIO_DT_SPEC_GET_OR(LED1_NODE, gpios, {0}),
    .num = 1,
};

const struct led led2 = {
    .spec = GPIO_DT_SPEC_GET_OR(LED2_NODE, gpios, {0}),
    .num = 2,
};
