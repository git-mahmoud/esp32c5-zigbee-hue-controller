#include "favorites_storage.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "FAVORITES";
static favorite_t s_favorites[MAX_FAVORITES];
static bool s_loaded = false;

static void load_all(void) {
    if (s_loaded) return;
    nvs_handle_t nvs;
    memset(s_favorites, 0, sizeof(s_favorites));
    if (nvs_open("favorites", NVS_READWRITE, &nvs) == ESP_OK) {
        for (int i = 0; i < MAX_FAVORITES; i++) {
            char key[8];
            sprintf(key, "fav%d", i);
            size_t len = sizeof(favorite_t);
            if (nvs_get_blob(nvs, key, &s_favorites[i], &len) != ESP_OK || len != sizeof(favorite_t)) {
                memset(&s_favorites[i], 0, sizeof(favorite_t));
            }
        }
        nvs_close(nvs);
    }
    s_loaded = true;
}

static void save_one(uint8_t id) {
    nvs_handle_t nvs;
    if (nvs_open("favorites", NVS_READWRITE, &nvs) == ESP_OK) {
        char key[8];
        sprintf(key, "fav%d", id);
        if (s_favorites[id].name[0] == 0)
            nvs_erase_key(nvs, key);
        else
            nvs_set_blob(nvs, key, &s_favorites[id], sizeof(favorite_t));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void favorites_init(void) {
    load_all();
}

bool favorites_get(uint8_t id, favorite_t *out) {
    load_all();
    if (id >= MAX_FAVORITES) return false;
    if (s_favorites[id].name[0] == 0) return false;
    memcpy(out, &s_favorites[id], sizeof(favorite_t));
    return true;
}

bool favorites_add(const favorite_t *fav) {
    load_all();
    for (int i = 0; i < MAX_FAVORITES; i++) {
        if (s_favorites[i].name[0] == 0) {
            memcpy(&s_favorites[i], fav, sizeof(favorite_t));
            save_one(i);
            return true;
        }
    }
    ESP_LOGW(TAG, "No free slot for favourite");
    return false;
}

bool favorites_update(uint8_t id, const favorite_t *fav) {
    if (id >= MAX_FAVORITES) return false;
    load_all();
    memcpy(&s_favorites[id], fav, sizeof(favorite_t));
    save_one(id);
    return true;
}

bool favorites_delete(uint8_t id) {
    if (id >= MAX_FAVORITES) return false;
    load_all();
    memset(&s_favorites[id], 0, sizeof(favorite_t));
    save_one(id);
    return true;
}