/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier:  LicenseRef-Included
 *
 * Zigbee HA_on_off_switch Example with HTTP server extension
 */
#pragma once

#include "esp_zigbee_core.h"
#include "switch_driver.h"
#include "zcl_utility.h"
#include <stdint.h>
#include <stdbool.h>

/* Zigbee configuration */
#define MAX_CHILDREN 10
#define INSTALLCODE_POLICY_ENABLE false
#define HA_ONOFF_SWITCH_ENDPOINT 1
#define ESP_ZB_PRIMARY_CHANNEL_MASK (1l << 13)

/* Basic manufacturer information */
#define ESP_MANUFACTURER_NAME "\x09" \
                              "ESPRESSIF"
#define ESP_MODEL_IDENTIFIER "\x07" CONFIG_IDF_TARGET

#define ESP_ZB_ZC_CONFIG()                                \
    {                                                     \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,    \
        .install_code_policy = INSTALLCODE_POLICY_ENABLE, \
        .nwk_cfg.zczr_cfg = {                             \
            .max_children = MAX_CHILDREN,                 \
        },                                                \
    }

#define ESP_ZB_DEFAULT_RADIO_CONFIG()       \
    {                                       \
        .radio_mode = ZB_RADIO_MODE_NATIVE, \
    }

#define ESP_ZB_DEFAULT_HOST_CONFIG()                          \
    {                                                         \
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE, \
    }

/* State structure for current lamp settings */
typedef struct
{
    bool on;            // true = lamp on, false = off
    uint8_t brightness; // 0‑100 %
    uint16_t ct_kelvin; // colour temperature in Kelvin (2200‑6500)
} device_state_t;

/* Public API for web server and other modules */
void start_touchlink_pairing(void);
bool get_lamp_paired(void);
device_state_t get_current_state(void);
void set_on_off(bool on);
void set_brightness(uint8_t percent);
void set_color_temp(uint16_t kelvin);
void apply_favorite(uint8_t fav_id);