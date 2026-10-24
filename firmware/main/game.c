// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>
#include "game.h"
#include "mosaico_raylib_fast.h"

struct game_t {
    game_config_t config;
    game_snapshot_t state;
};

bool game_create(const game_config_t *config, game_handle_t *ret_handle)
{
    if (!ret_handle) return false;
    *ret_handle = NULL;
    if (!config || !config->width || !config->height) return false;
    game_handle_t game = calloc(1, sizeof(*game));
    if (!game) return false;
    game->config = *config;
    *ret_handle = game;
    return true;
}

void game_delete(game_handle_t game)
{
    free(game);
}

void game_reset(game_handle_t game)
{
    if (game) game->state = (game_snapshot_t){0};
}

void game_set_paused(game_handle_t game, bool paused)
{
    if (game) game->state.paused = paused;
}

void game_set_pointer(game_handle_t game, int32_t x, int32_t y, bool pressed)
{
    if (!game) return;
    game->state.pointer_x = x < 0 ? 0 : (x >= game->config.width ? game->config.width - 1 : x);
    game->state.pointer_y = y < 0 ? 0 : (y >= game->config.height ? game->config.height - 1 : y);
    game->state.pointer_down = pressed;
    /* Add touch-driven game logic here. */
}

void game_update(game_handle_t game)
{
    if (!game || game->state.paused) return;
    ++game->state.tick;
    /* Add fixed-step game logic here. */
}

bool game_render(game_handle_t game)
{
    if (!game) return false;
    BeginDrawing();
    if (!MosaicoFastFrameAvailable()) {
        EndDrawing();
        return false;
    }
    ClearBackground(BLACK);
    /* Add Raylib Lite drawing here; Host and device use this same function. */
    EndDrawing();
    return true;
}

game_snapshot_t game_read(game_handle_t game)
{
    return game ? game->state : (game_snapshot_t){0};
}
