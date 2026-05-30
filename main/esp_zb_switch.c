#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_coexist.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "bdb/esp_zigbee_bdb_touchlink.h"
#include "zcl/esp_zigbee_zcl_color_control.h"
#include "zcl/esp_zigbee_zcl_level.h"
#include "esp_zb_switch.h"
#include "web_server.h"
#include "favorites_storage.h"

/* ================================================================
 *  WIFI CREDENTIALS
 * ================================================================ */
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
/* ================================================================ */

#define BTN_TOUCHLINK GPIO_NUM_4
#define BTN_ON GPIO_NUM_5
#define BTN_OFF GPIO_NUM_6
#define DEBOUNCE_MS 300

/* All 4 ZLL channels */
#define ZLL_CHANNEL_MASK ((1l << 11) | (1l << 15) | (1l << 20) | (1l << 25))

/* Broadcast address — used for network-wide device scan on reboot */
#define ZB_ADDR_BROADCAST 0xFFFF

static const char *TAG = "HUE_CTRL";

/* ── ZLL distributed security key (required for Hue Touchlink) ───── */
static uint8_t s_zll_key[16] = {
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf};

/* ── State ───────────────────────────────────────────────────────── */
static struct
{
    uint16_t addr;
    uint8_t ep;
    bool paired;
} g_lamp = {0};

static device_state_t s_state = {
    .on = false,
    .brightness = 70,
    .ct_kelvin = 3000,
};

typedef enum
{
    EVT_TOUCHLINK,
    EVT_ON,
    EVT_OFF
} btn_evt_t;
static QueueHandle_t s_btn_q = NULL;

#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_eg = NULL;

/* ── NVS — app state ─────────────────────────────────────────────── */
static void load_state_from_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open("switch_state", NVS_READWRITE, &nvs) == ESP_OK)
    {
        uint8_t on = 0, bri = 70;
        uint16_t ct = 3000;
        nvs_get_u8(nvs, "on", &on);
        nvs_get_u8(nvs, "bri", &bri);
        nvs_get_u16(nvs, "ct", &ct);
        s_state.on = (on != 0);
        s_state.brightness = bri;
        s_state.ct_kelvin = (ct >= 2200 && ct <= 6500) ? ct : 3000;
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "App state: on=%d bri=%d%% ct=%dK",
             s_state.on, s_state.brightness, s_state.ct_kelvin);
}

static void save_state_to_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open("switch_state", NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_u8(nvs, "on", (uint8_t)s_state.on);
        nvs_set_u8(nvs, "bri", s_state.brightness);
        nvs_set_u16(nvs, "ct", s_state.ct_kelvin);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* ── NVS — lamp address ──────────────────────────────────────────── */
static void save_lamp_to_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open("lamp_state", NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_u16(nvs, "addr", g_lamp.addr);
        nvs_set_u8(nvs, "ep", g_lamp.ep);
        nvs_set_u8(nvs, "valid", 1);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Lamp saved to NVS: addr=0x%04x ep=%d",
                 g_lamp.addr, g_lamp.ep);
    }
}

static bool load_lamp_from_nvs(void)
{
    nvs_handle_t nvs;
    bool ok = false;
    if (nvs_open("lamp_state", NVS_READONLY, &nvs) == ESP_OK)
    {
        uint8_t valid = 0;
        nvs_get_u8(nvs, "valid", &valid);
        if (valid)
        {
            uint16_t addr = 0;
            uint8_t ep = 0;
            if (nvs_get_u16(nvs, "addr", &addr) == ESP_OK &&
                nvs_get_u8(nvs, "ep", &ep) == ESP_OK)
            {
                g_lamp.addr = addr;
                g_lamp.ep = ep;
                g_lamp.paired = true;
                ok = true;
                ESP_LOGI(TAG, "Lamp loaded from NVS: addr=0x%04x ep=%d",
                         addr, ep);
            }
        }
        nvs_close(nvs);
    }
    return ok;
}

static void clear_lamp_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open("lamp_state", NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_u8(nvs, "valid", 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* ── ZCL senders with safety checks ─────────────────────────────── */
static void _send_on_off(bool on)
{
    if (!g_lamp.paired)
    {
        ESP_LOGW(TAG, "Cannot send ON/OFF: lamp not paired");
        return;
    }
    if (g_lamp.addr == 0 || g_lamp.addr == 0xFFFF)
    {
        ESP_LOGW(TAG, "Invalid lamp address: 0x%04x", g_lamp.addr);
        return;
    }

    esp_zb_zcl_on_off_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = g_lamp.addr,
            .dst_endpoint = g_lamp.ep,
            .src_endpoint = HA_ONOFF_SWITCH_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .on_off_cmd_id = on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID,
    };
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_on_off_cmd_req(&cmd);
    esp_zb_lock_release();
}

static void _send_brightness(uint8_t percent)
{
    if (!g_lamp.paired)
    {
        ESP_LOGW(TAG, "Cannot send brightness: lamp not paired");
        return;
    }
    if (g_lamp.addr == 0 || g_lamp.addr == 0xFFFF)
    {
        ESP_LOGW(TAG, "Invalid lamp address: 0x%04x", g_lamp.addr);
        return;
    }

    uint8_t level = (percent == 0) ? 0
                                   : (uint8_t)(3 + ((uint32_t)(percent - 1) * 251) / 99);
    esp_zb_zcl_move_to_level_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = g_lamp.addr,
            .dst_endpoint = g_lamp.ep,
            .src_endpoint = HA_ONOFF_SWITCH_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .level = level,
        .transition_time = 3,
    };
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_level_move_to_level_cmd_req(&cmd);
    esp_zb_lock_release();
    ESP_LOGI(TAG, "Brightness %d%% -> level %d", percent, level);
}

static void _send_color_temp(uint16_t kelvin)
{
    if (!g_lamp.paired)
    {
        ESP_LOGW(TAG, "Cannot send color temp: lamp not paired");
        return;
    }
    if (g_lamp.addr == 0 || g_lamp.addr == 0xFFFF)
    {
        ESP_LOGW(TAG, "Invalid lamp address: 0x%04x", g_lamp.addr);
        return;
    }

    if (kelvin < 2200)
        kelvin = 2200;
    if (kelvin > 6500)
        kelvin = 6500;
    uint16_t mired = (uint16_t)(1000000UL / kelvin);
    esp_zb_zcl_color_move_to_color_temperature_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = g_lamp.addr,
            .dst_endpoint = g_lamp.ep,
            .src_endpoint = HA_ONOFF_SWITCH_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .color_temperature = mired,
        .transition_time = 3,
    };
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_color_move_to_color_temperature_cmd_req(&cmd);
    esp_zb_lock_release();
    ESP_LOGI(TAG, "CT %dK -> %d mired", kelvin, mired);
}

/* ── Public API ──────────────────────────────────────────────────── */
bool get_lamp_paired(void) { return g_lamp.paired; }
device_state_t get_current_state(void) { return s_state; }

void set_on_off(bool on)
{
    _send_on_off(on);
    s_state.on = on;
    save_state_to_nvs();
}

void set_brightness(uint8_t percent)
{
    if (!s_state.on && percent > 0)
    {
        _send_on_off(true);
        s_state.on = true;
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    _send_brightness(percent);
    s_state.brightness = percent;
    save_state_to_nvs();
}

void set_color_temp(uint16_t kelvin)
{
    if (!s_state.on)
    {
        _send_on_off(true);
        s_state.on = true;
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    _send_color_temp(kelvin);
    s_state.ct_kelvin = kelvin;
    save_state_to_nvs();
}

void apply_favorite(uint8_t fav_id)
{
    favorite_t fav;
    if (!favorites_get(fav_id, &fav))
    {
        ESP_LOGW(TAG, "Favorite %d not found", fav_id);
        return;
    }
    if (!g_lamp.paired)
    {
        ESP_LOGW(TAG, "Cannot apply favorite: lamp not paired");
        return;
    }

    _send_on_off(true);
    s_state.on = true;
    vTaskDelay(pdMS_TO_TICKS(100));
    _send_brightness(fav.brightness);
    s_state.brightness = fav.brightness;
    vTaskDelay(pdMS_TO_TICKS(80));
    _send_color_temp(fav.ct_kelvin);
    s_state.ct_kelvin = fav.ct_kelvin;
    save_state_to_nvs();
    ESP_LOGI(TAG, "Applied fav %d: %s bri=%d%% ct=%dK",
             fav_id, fav.name, fav.brightness, fav.ct_kelvin);
}

/*
 * start_touchlink_pairing()
 *
 * Single entry point for Touchlink commissioning — called by both the
 * physical button (button_task) and the web handler (/cmd?pair).
 *
 * Always clears the stale lamp record first so the Zigbee stack does not
 * skip commissioning because it thinks a device is already paired.
 */
void start_touchlink_pairing(void)
{
    ESP_LOGI(TAG, "Touchlink — hold ESP ~2 cm from lamp");
    g_lamp.paired = false;
    g_lamp.addr = 0;
    g_lamp.ep = 0;
    clear_lamp_nvs();

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_bdb_start_top_level_commissioning(
        ESP_ZB_BDB_MODE_INITIALIZATION | ESP_ZB_BDB_TOUCHLINK_COMMISSIONING);
    esp_zb_lock_release();
}

/* ── GPIO ────────────────────────────────────────────────────────── */
static void IRAM_ATTR btn_isr(void *arg)
{
    btn_evt_t e = (btn_evt_t)(uint32_t)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_btn_q)
    {
        xQueueSendFromISR(s_btn_q, &e, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken)
        {
            portYIELD_FROM_ISR();
        }
    }
}

static esp_err_t buttons_init(void)
{
    gpio_config_t c = {
        .pin_bit_mask = (1ULL << BTN_TOUCHLINK) | (1ULL << BTN_ON) | (1ULL << BTN_OFF),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&c), TAG, "gpio_config failed");
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), TAG, "isr_service failed");
    gpio_isr_handler_add(BTN_TOUCHLINK, btn_isr, (void *)EVT_TOUCHLINK);
    gpio_isr_handler_add(BTN_ON, btn_isr, (void *)EVT_ON);
    gpio_isr_handler_add(BTN_OFF, btn_isr, (void *)EVT_OFF);
    ESP_LOGI(TAG, "Buttons: GPIO%d=pair  GPIO%d=on  GPIO%d=off",
             BTN_TOUCHLINK, BTN_ON, BTN_OFF);
    return ESP_OK;
}

/* ── Button task ─────────────────────────────────────────────────── */
static void button_task(void *arg)
{
    btn_evt_t evt;
    TickType_t last[3] = {0};

    while (1)
    {
        if (!s_btn_q || !xQueueReceive(s_btn_q, &evt, portMAX_DELAY))
            continue;

        TickType_t now = xTaskGetTickCount();
        if ((now - last[evt]) < pdMS_TO_TICKS(DEBOUNCE_MS))
            continue;
        last[evt] = now;

        switch (evt)
        {
        case EVT_TOUCHLINK:
            start_touchlink_pairing();
            break;

        case EVT_ON:
            if (!g_lamp.paired)
            {
                ESP_LOGW(TAG, "[BTN2] Lamp not paired");
                break;
            }
            ESP_LOGI(TAG, "[BTN2] On -> 0x%04x ep%d", g_lamp.addr, g_lamp.ep);
            _send_on_off(true);
            s_state.on = true;
            break;

        case EVT_OFF:
            if (!g_lamp.paired)
            {
                ESP_LOGW(TAG, "[BTN3] Lamp not paired");
                break;
            }
            ESP_LOGI(TAG, "[BTN3] Off -> 0x%04x ep%d", g_lamp.addr, g_lamp.ep);
            _send_on_off(false);
            s_state.on = false;
            break;
        }
    }
}

/* ── WiFi ────────────────────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "WiFi disconnected — retrying");
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "=========================================");
        ESP_LOGI(TAG, "  IP:  " IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "  URL: http://" IPSTR "/", IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "=========================================");
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_eg = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi connecting to \"%s\"", WIFI_SSID);
}

static void wifi_and_server_task(void *arg)
{
    xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT,
                        false, true, portMAX_DELAY);
    start_web_server();
    vTaskDelete(NULL);
}

/* ── ZDO: found lamp on network ──────────────────────────────────── */
static void find_light_cb(esp_zb_zdp_status_t status, uint16_t addr,
                          uint8_t ep, void *ctx)
{
    if (status == ESP_ZB_ZDP_STATUS_SUCCESS)
    {
        bool was_paired = g_lamp.paired;
        bool addr_changed = (g_lamp.addr != addr || g_lamp.ep != ep);

        g_lamp.addr = addr;
        g_lamp.ep = ep;
        g_lamp.paired = true;
        save_lamp_to_nvs();

        if (!was_paired)
        {
            ESP_LOGI(TAG, "Lamp bound: addr=0x%04x ep=%d", addr, ep);
        }
        else if (addr_changed)
        {
            ESP_LOGI(TAG, "Lamp re-confirmed (new addr): 0x%04x ep=%d", addr, ep);
        }
        else
        {
            ESP_LOGI(TAG, "Lamp re-confirmed: addr=0x%04x ep=%d", addr, ep);
        }
        ESP_LOGI(TAG, "Ready — BTN2=On  BTN3=Off  HTTP active");
    }
    else
    {
        ESP_LOGD(TAG, "find_light_cb ZDP 0x%x (lamp may be off)", status);
    }
}

/* Broadcast scan — finds lamp on reboot without waiting for announce */
static void scan_for_lamp(void)
{
    if (!g_lamp.paired)
        return;
    ESP_LOGI(TAG, "Scanning network for existing lamp...");
    esp_zb_zdo_match_desc_req_param_t req = {
        .dst_nwk_addr = ZB_ADDR_BROADCAST,
        .addr_of_interest = ZB_ADDR_BROADCAST,
    };
    esp_zb_zdo_find_on_off_light(&req, find_light_cb, NULL);
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    esp_zb_bdb_start_top_level_commissioning(mode_mask);
}

/* ── Periodic scan task ──────────────────────────────────────────── */
static void periodic_scan_task(void *arg)
{
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (g_lamp.paired)
        {
            scan_for_lamp();
        }
    }
}

/* ── Zigbee signal handler ───────────────────────────────────────── */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig = *p_sg_p;
    esp_zb_zdo_signal_device_annce_params_t *ann = NULL;

    switch (sig)
    {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack ready");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK)
        {
            ESP_LOGI(TAG, "Started (%s)",
                     esp_zb_bdb_is_factory_new() ? "factory-new" : "NVS");
            if (esp_zb_bdb_is_factory_new())
            {
                esp_zb_bdb_start_top_level_commissioning(
                    ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            }
            else
            {
                esp_zb_bdb_open_network(180);
                ESP_LOGI(TAG, "Network open 180 s");
                if (g_lamp.paired)
                {
                    ESP_LOGI(TAG, "Lamp addr restored from NVS (0x%04x ep%d) — verifying...",
                             g_lamp.addr, g_lamp.ep);
                    esp_zb_scheduler_alarm(
                        (esp_zb_callback_t)scan_for_lamp, 0, 2000);
                }
            }
        }
        else
        {
            ESP_LOGE(TAG, "Stack init failed: %s", esp_err_to_name(err_status));
        }
        break;

    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err_status == ESP_OK)
        {
            ESP_LOGI(TAG, "Network formed");
            esp_zb_bdb_start_top_level_commissioning(
                ESP_ZB_BDB_MODE_NETWORK_STEERING);
        }
        else
        {
            esp_zb_scheduler_alarm(
                (esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK)
            ESP_LOGI(TAG, "Network open — BTN1 to pair");
        break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
        ann = (esp_zb_zdo_signal_device_annce_params_t *)
            esp_zb_app_signal_get_params(p_sg_p);
        if (ann)
        {
            ESP_LOGI(TAG, "Device announced: 0x%04hx", ann->device_short_addr);
            esp_zb_zdo_match_desc_req_param_t req = {
                .dst_nwk_addr = ann->device_short_addr,
                .addr_of_interest = ann->device_short_addr,
            };
            esp_zb_zdo_find_on_off_light(&req, find_light_cb, NULL);
        }
        break;

    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        if (err_status == ESP_OK)
        {
            uint8_t s = *(uint8_t *)esp_zb_app_signal_get_params(p_sg_p);
            if (s)
                ESP_LOGI(TAG, "Network open for %d s", s);
            else
                ESP_LOGW(TAG, "Network closed");
        }
        break;

    case ESP_ZB_BDB_SIGNAL_TOUCHLINK_NWK_STARTED:
    case ESP_ZB_BDB_SIGNAL_TOUCHLINK_NWK_JOINED_ROUTER:
        if (err_status == ESP_OK)
        {
            ESP_LOGI(TAG, "Touchlink: device joined");
            esp_zb_bdb_start_top_level_commissioning(
                ESP_ZB_BDB_MODE_NETWORK_STEERING);
        }
        else
        {
            ESP_LOGW(TAG, "Touchlink join failed: %s", esp_err_to_name(err_status));
        }
        break;

    case ESP_ZB_BDB_SIGNAL_TOUCHLINK:
        if (err_status == ESP_OK)
        {
            ESP_LOGI(TAG, "Touchlink commissioning complete");
            /* Scan immediately to resolve the new device's address */
            esp_zb_scheduler_alarm(
                (esp_zb_callback_t)scan_for_lamp, 0, 1000);
        }
        else
        {
            ESP_LOGW(TAG, "Touchlink failed: %s", esp_err_to_name(err_status));
        }
        break;

    default:
        break;
    }
}

/* ── Zigbee task ─────────────────────────────────────────────────── */
static void esp_zb_task(void *pvParameters)
{
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZC_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    esp_zb_set_rx_on_when_idle(true);
    esp_zb_secur_TC_standard_distributed_key_set(s_zll_key);
    esp_zb_zdo_touchlink_set_rssi_threshold(-72);

    esp_zb_on_off_switch_cfg_t switch_cfg = ESP_ZB_DEFAULT_ON_OFF_SWITCH_CONFIG();
    esp_zb_ep_list_t *ep_list =
        esp_zb_on_off_switch_ep_create(HA_ONOFF_SWITCH_ENDPOINT, &switch_cfg);

    esp_zb_cluster_list_t *cluster_list =
        esp_zb_ep_list_get_ep(ep_list, HA_ONOFF_SWITCH_ENDPOINT);

    esp_zb_color_cluster_cfg_t color_cfg = {
        .current_x = 0,
        .current_y = 0,
        .color_mode = 2,
        .options = 0,
        .enhanced_color_mode = 2,
        .color_capabilities = 0x0010,
    };
    esp_zb_cluster_list_add_color_control_cluster(
        cluster_list,
        esp_zb_color_control_cluster_create(&color_cfg),
        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    zcl_basic_manufacturer_info_t info = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(
        ep_list, HA_ONOFF_SWITCH_ENDPOINT, &info);

    esp_zb_device_register(ep_list);
    esp_zb_set_primary_network_channel_set(ZLL_CHANNEL_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

/* ── app_main ────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    /* Must be initialised before any WiFi or netif calls */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    ESP_ERROR_CHECK(esp_coex_wifi_i154_enable());

    load_state_from_nvs();
    load_lamp_from_nvs();
    favorites_init();
    wifi_init();

    s_btn_q = xQueueCreate(10, sizeof(btn_evt_t));
    if (!s_btn_q)
    {
        ESP_LOGE(TAG, "Failed to create button queue");
        return;
    }
    ESP_ERROR_CHECK(buttons_init());

    ESP_LOGI(TAG, "=== Hue Candle Controller ===");
    ESP_LOGI(TAG, "BTN1=Pair  BTN2=On  BTN3=Off");
    ESP_LOGI(TAG, "Lamp state: paired=%d addr=0x%04x ep=%d",
             g_lamp.paired, g_lamp.addr, g_lamp.ep);

    if (g_lamp.paired)
    {
        xTaskCreate(periodic_scan_task, "periodic_scan", 4096, NULL, 3, NULL);
    }

    xTaskCreate(button_task, "btn_task", 4096, NULL, 5, NULL);
    xTaskCreate(wifi_and_server_task, "wifi_srv", 4096, NULL, 4, NULL);
    xTaskCreate(esp_zb_task, "Zigbee_main", 8192, NULL, 5, NULL);
}