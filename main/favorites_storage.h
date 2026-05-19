#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MAX_FAVORITES 10
#define FAV_NAME_LEN  32

typedef struct {
    char name[FAV_NAME_LEN];
    uint8_t brightness;   // 0‑100 %
    uint16_t ct_kelvin;   // 2200‑6500
} favorite_t;

void favorites_init(void);
bool favorites_get(uint8_t id, favorite_t *out);
bool favorites_add(const favorite_t *fav);
bool favorites_update(uint8_t id, const favorite_t *fav);
bool favorites_delete(uint8_t id);