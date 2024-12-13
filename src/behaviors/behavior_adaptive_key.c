/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_adaptive_key

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keys.h>
#include <zmk/matrix.h>
#include <zmk/keymap.h>

#include <zmk-adaptive-key/keys.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct key_list {
    size_t size;
    struct zmk_key_param keys[];
};

struct binding_list {
    size_t size;
    struct zmk_behavior_binding bindings[CONFIG_ZMK_ADAPTIVE_KEY_MAX_BINDINGS];
};

struct trigger_cfg {
    struct binding_list bindings;
    size_t trigger_keys_len;
    const struct zmk_key_param trigger_keys[CONFIG_ZMK_ADAPTIVE_KEY_MAX_TRIGGER_CONDITIONS];
    int min_idle_ms;
    int max_idle_ms;
    bool delete_prior;
    bool strict_modifiers;
};

struct behavior_adaptive_key_config {
    uint8_t index;
    struct binding_list default_binding;
    size_t triggers_len;
    const struct trigger_cfg *triggers;
    const struct key_list *dead_keys;
    uint32_t delete_keycode;
};

struct behavior_adaptive_key_data {
    struct zmk_key_param last_keycode;
    int64_t last_timestamp;
    const struct binding_list *pressed_bindings;
};

// Dead keys are global.
bool last_keycode_is_dead;

static inline int press_adaptive_key_behavior(const struct behavior_adaptive_key_data *data,
                                              struct zmk_behavior_binding_event *event) {
    const struct binding_list *list = data->pressed_bindings;
    LOG_DBG("Iterating %d adaptive key bindings", list->size);
    if (list->size > 1) {

        for (int i = 0; i < list->size - 1; i++) {
            zmk_behavior_queue_add(event, list->bindings[i], true, CONFIG_ZMK_ADAPTIVE_KEY_TAP_MS);
            zmk_behavior_queue_add(event, list->bindings[i], false,
                                   CONFIG_ZMK_ADAPTIVE_KEY_WAIT_MS);
        }

        return zmk_behavior_queue_add(event, list->bindings[list->size - 1], true,
                                      CONFIG_ZMK_ADAPTIVE_KEY_TAP_MS);
    }

    return zmk_behavior_invoke_binding(&list->bindings[0], *event, true);
}

static inline int release_adaptive_key_behavior(const struct behavior_adaptive_key_data *data,
                                                struct zmk_behavior_binding_event *event) {
    LOG_DBG("Releasing adaptive key binding");
    const struct binding_list *list = data->pressed_bindings;
    if (list->size > 1) {
        return zmk_behavior_queue_add(event, list->bindings[list->size - 1], false,
                                      CONFIG_ZMK_ADAPTIVE_KEY_WAIT_MS);
    }

    return zmk_behavior_invoke_binding(&list->bindings[0], *event, false);
}

static bool keys_are_equal(const struct zmk_key_param *key, const struct zmk_key_param *other,
                           bool strict) {

    if (strict && key->modifiers != other->modifiers) {
        return false;
    }

    // *key mods can be subset of *other mods if strict is disabled.
    if (!strict && (key->modifiers & other->modifiers) != key->modifiers) {
        return false;
    }

    return key->page == other->page && key->id == other->id;
}

static bool trigger_is_true(const struct trigger_cfg *trigger,
                            struct behavior_adaptive_key_data *data, int64_t timestamp) {

    int64_t last_timestamp = data->last_timestamp;
    if (trigger->min_idle_ms > -1 && (timestamp - last_timestamp) < trigger->min_idle_ms) {
        return false;
    }

    if (trigger->max_idle_ms > -1 && (timestamp - last_timestamp) > trigger->max_idle_ms) {
        return false;
    }

    for (int i = 0; i < trigger->trigger_keys_len; i++) {
        if (keys_are_equal(&trigger->trigger_keys[i], &data->last_keycode,
                           trigger->strict_modifiers)) {

            data->pressed_bindings = &trigger->bindings;

            return true;
        }
    }

    return false;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_adaptive_key_data *data = dev->data;

    if (!data->last_keycode.page && data->pressed_bindings) {
        LOG_ERR("Adaptive key pressed twice or no prior key press detected");
        return ZMK_BEHAVIOR_OPAQUE;
    }

    bool match = false;
    const struct behavior_adaptive_key_config *config = dev->config;
    for (int i = 0; i < config->triggers_len; i++) {
        if (trigger_is_true(&config->triggers[i], data, event.timestamp)) {
            match = true;
            break;
        }
    }

    if (!match) {
        LOG_DBG("No adaptive key match found, invoking default behavior");
        data->pressed_bindings = &config->default_binding;
    }

    press_adaptive_key_behavior(data, &event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_adaptive_key_data *data = dev->data;

    if (data->pressed_bindings) {
        release_adaptive_key_behavior(data, &event);
        data->pressed_bindings = NULL;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_adaptive_key_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

static int adaptive_key_keycode_state_changed_listener(const zmk_event_t *eh);

ZMK_LISTENER(behavior_adaptive_key, adaptive_key_keycode_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_adaptive_key, zmk_keycode_state_changed);

static bool key_list_contains(const struct key_list *list, const struct zmk_key_param *key) {
    for (int i = 0; i < list->size; i++) {
        if (keys_are_equal(&list->keys[i], key, true)) {
            return true;
        }
    }

    return false;
}

static const struct device *devs[DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)];

static int adaptive_key_keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || (!ev->state && !last_keycode_is_dead)) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (!ev->state && last_keycode_is_dead) {
        return ZMK_EV_EVENT_HANDLED;
    }

    const struct zmk_key_param key = {
        .modifiers = ev->implicit_modifiers | zmk_hid_get_explicit_mods(),
        .page = ev->usage_page,
        .id = ev->keycode,
    };

    bool ignore = false;
    for (int i = 0; i < DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT); i++) {

        const struct device *dev = devs[i];
        if (dev == NULL) {
            continue;
        }

        struct behavior_adaptive_key_data *data = dev->data;
        data->last_keycode = key;
        data->last_timestamp = ev->timestamp;

        const struct behavior_adaptive_key_config *config = dev->config;
        if (key_list_contains(config->dead_keys, &key)) {
            ignore = true;
        }
    }

    last_keycode_is_dead = ignore && !last_keycode_is_dead;
    if (last_keycode_is_dead) {
        return ZMK_EV_EVENT_HANDLED;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int behavior_adaptive_key_init(const struct device *dev) {
    const struct behavior_adaptive_key_config *config = dev->config;
    devs[config->index] = dev;
    return 0;
}

#define KEY_TRIGGER_ITEM(i, n, prop) ZMK_KEY_PARAM_DECODE(DT_PROP_BY_IDX(n, prop, i))

#define ZMK_KEY_PARAM_DECODE(param)                                                                \
    (struct zmk_key_param) {                                                                       \
        .modifiers = SELECT_MODS(param), .page = ZMK_HID_USAGE_PAGE(param),                        \
        .id = ZMK_HID_USAGE_ID(param),                                                             \
    }

#define TRANSFORMED_BINDINGS(n)                                                                    \
    (struct binding_list) {                                                                        \
        .size = DT_PROP_LEN(n, bindings),                                                          \
        .bindings = {LISTIFY(DT_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), n)},      \
    }

#define PROP_TRIGGERS(n, prop)                                                                     \
    {                                                                                              \
        .bindings = TRANSFORMED_BINDINGS(n), .trigger_keys_len = DT_PROP_LEN(n, prop),             \
        .trigger_keys = {LISTIFY(DT_PROP_LEN(n, prop), KEY_TRIGGER_ITEM, (, ), n, prop)},          \
        .min_idle_ms = DT_PROP(n, min_prior_idle_ms),                                              \
        .max_idle_ms = DT_PROP(n, max_prior_idle_ms),                                              \
        .strict_modifiers = DT_PROP(n, strict_modifiers),                                          \
    }

#define KEY_LIST_ITEM(i, n, prop) ZMK_KEY_PARAM_DECODE(DT_INST_PROP_BY_IDX(n, prop, i))

#define PROP_KEY_LIST(n, prop)                                                                     \
    COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(n), prop),                                            \
                ({                                                                                 \
                    .size = DT_INST_PROP_LEN(n, prop),                                             \
                    .keys = {LISTIFY(DT_INST_PROP_LEN(n, prop), KEY_LIST_ITEM, (, ), n, prop)},    \
                }),                                                                                \
                ({.size = 0}))

#define AK_INST(n)                                                                                 \
    static const struct trigger_cfg adaptive_key_triggers_##n[] = {                                \
        DT_INST_FOREACH_CHILD_STATUS_OKAY_SEP_VARGS(n, PROP_TRIGGERS, (, ), trigger_keys)};        \
    static const struct key_list adaptive_key_ignore_keys_##n = PROP_KEY_LIST(n, dead_keys);       \
    static struct behavior_adaptive_key_data behavior_adaptive_key_data_##n = {};                  \
    static const struct behavior_adaptive_key_config behavior_adaptive_key_config_##n = {          \
        .index = n,                                                                                \
        .default_binding =                                                                         \
            (struct binding_list){                                                                 \
                .size = 1,                                                                         \
                .bindings = {ZMK_KEYMAP_EXTRACT_BINDING(0, DT_DRV_INST(n))},                       \
            },                                                                                     \
        .triggers = adaptive_key_triggers_##n,                                                     \
        .triggers_len = ARRAY_SIZE(adaptive_key_triggers_##n),                                     \
        .dead_keys = &adaptive_key_ignore_keys_##n,                                                \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_adaptive_key_init, NULL, &behavior_adaptive_key_data_##n,  \
                            &behavior_adaptive_key_config_##n, POST_KERNEL,                        \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_adaptive_key_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AK_INST)
