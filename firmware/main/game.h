// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct game_t *game_handle_t;
typedef struct {
    uint16_t width;
    uint16_t height;
} game_config_t;

typedef struct {
    uint32_t tick;
    int32_t pointer_x;
    int32_t pointer_y;
    bool pointer_down;
    bool paused;
} game_snapshot_t;

/* Portable C shared by Host and device. One loop owns each instance and calls
 * these methods; input adapters enqueue or deliver events on that same loop. */
bool game_create(const game_config_t *config, game_handle_t *ret_handle);
void game_delete(game_handle_t handle);
void game_reset(game_handle_t handle);
void game_set_paused(game_handle_t handle, bool paused);
void game_set_pointer(game_handle_t handle, int32_t x, int32_t y, bool pressed);
void game_update(game_handle_t handle);
bool game_render(game_handle_t handle);
game_snapshot_t game_read(game_handle_t handle);
