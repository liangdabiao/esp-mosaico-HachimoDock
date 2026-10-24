// SPDX-License-Identifier: Apache-2.0
/* The engine bridge's entry symbol is plain-C with no dllexport; MinGW ld
 * stops auto-exporting once this project's data symbols appear. Re-export
 * the entry explicitly through a COFF .drectve so ctypes always finds it. */
#if defined(_WIN32) && !defined(__clang__)
__asm__(".section .drectve
	.ascii \" -export:mosaico_host_game_v1\"
	");
#endif

/* Host simulator module: drives the HachimoDock renderer with a scripted
 * runtime state (no protocol stack) so page layouts, CJK text and session
 * cards are validated on the sim canvas exactly as on device. */
#include <stdio.h>
#include <string.h>

#include "game_config.h"
#include "hd_pet.h"
#include "mosaico_game_module.h"
#include "mosaico_game_2d.h"
#include "mosaico_raylib_fast.h"

#include "pet/pet_p4_renderer.h"
#include "pet/pet_p4_protocol.h"
#include "pet/pet_p4_view.h"

typedef struct {
    pet_p4_runtime_state_t state;
    pet_p4_view_model_t view;
    uint32_t tick;
    int phase;
    Texture2D frame;
} host_state_t;

static const char *k_phases[] = {"working", "waiting_user", "done", "error", "idle"};

static void advance_phase(host_state_t *host) {
    host->phase = (host->phase + 1) % 5;
    pet_p4_runtime_state_t *s = &host->state;
    snprintf(s->active_agent, sizeof(s->active_agent), "%s", "Claude Code");
    snprintf(s->current_state, sizeof(s->current_state), "%s", k_phases[host->phase]);
    snprintf(s->current_title, sizeof(s->current_title), "%s",
             host->phase == 0 ? "正在重构 usb_serial.rs 协议层"
             : host->phase == 1 ? "等待确认：是否删除旧传输代码？"
             : host->phase == 2 ? "构建通过，测试全绿"
             : host->phase == 3 ? "链接失败：undefined reference"
             : "空闲");
    snprintf(s->current_speech, sizeof(s->current_speech), "%s",
             "已修改 12 个文件，正在运行回归测试");
    s->session_queue_count = 2;
    snprintf(s->session_queue[0].id, sizeof(s->session_queue[0].id), "%s", "s-1");
    snprintf(s->session_queue[0].title, sizeof(s->session_queue[0].title), "%s",
             "修复登录页触摸偏移");
    snprintf(s->session_queue[0].state, sizeof(s->session_queue[0].state), "%s", "working");
    s->session_queue[0].terminal_until_ms = 0;
    snprintf(s->session_queue[1].id, sizeof(s->session_queue[1].id), "%s", "s-2");
    snprintf(s->session_queue[1].title, sizeof(s->session_queue[1].title), "%s",
             "words_app 字库补字");
    snprintf(s->session_queue[1].state, sizeof(s->session_queue[1].state), "%s", "done");
    s->session_queue[1].terminal_until_ms = 0;
    s->last_update_ms = (unsigned long long) host->tick * 50ULL;
}

static int initialize(void *value, const char *asset_root)
{
    (void)asset_root;
    host_state_t *host = value;
    memset(host, 0, sizeof(*host));
    pet_p4_state_init(&host->state, "sim-hachimo");
    if (pet_p4_renderer_init() != 0) return -1;
    advance_phase(host);
    InitWindow(GAME_WIDTH, GAME_HEIGHT, GAME_NAME);
    SetTargetFPS(GAME_TICK_HZ);
    return 0;
}

static void shutdown(void *value)
{
    (void)value;
}

static void input(void *value, const mosaico_host_input_v1_t *event)
{
    (void)value;
    (void)event;
}

static void update(void *value)
{
    host_state_t *host = value;
    ++host->tick;
    if (host->tick % 100 == 0) advance_phase(host);
}

static int render(void *value)
{
    host_state_t *host = value;
    BeginDrawing();
    if (!MosaicoFastFrameAvailable()) {
        EndDrawing();
        return -1;
    }
    unsigned long long now_ms = (unsigned long long) host->tick * 50ULL;
    pet_p4_build_view_model(&host->state, &host->view);
    if (pet_p4_renderer_render(&host->state, &host->view, now_ms) == 0) {
        int width = 0, height = 0;
        const uint16_t *pixels = pet_p4_renderer_logical_framebuffer(&width, &height);
        if (pixels) {
            if (host->frame.id == 0)
                host->frame = Mosaico2DRegisterRGB565(pixels, width, height);
            if (host->frame.id != 0)
                MosaicoFastDrawTexture(host->frame, 0, 0, WHITE);
        }
    }
    EndDrawing();
    return 0;
}

static uint32_t state_hash(const void *value)
{
    const host_state_t *host = value;
    uint32_t hash = host->tick;
    hash = hash * 31U + (uint32_t) host->phase;
    return hash * 31U + (uint32_t) host->state.last_update_ms;
}

static int state_json(const void *value, char *output, size_t capacity)
{
    if (!output || !capacity) return -1;
    const host_state_t *host = value;
    int size = snprintf(output, capacity,
        "{\"phase\":\"%s\",\"tick\":%lu,\"agent\":\"%s\","
        "\"sessions\":%u,\"state_hash\":\"%08lx\"}",
        k_phases[host->phase], (unsigned long) host->tick,
        host->state.active_agent, host->state.session_queue_count,
        (unsigned long) state_hash(value));
    return size < 0 || (size_t) size >= capacity ? -1 : size;
}

static const mosaico_game_module_v1_t s_module = {
    .descriptor = {MOSAICO_HOST_GAME_ABI_V1, GAME_NAME, "HachimoPet",
                   GAME_WIDTH, GAME_HEIGHT, GAME_TICK_HZ, 1},
    .state_size = sizeof(host_state_t),
    .initialize = initialize, .shutdown = shutdown, .input = input,
    .update = update, .render = render,
    .state_hash = state_hash, .state_json = state_json,
};

const mosaico_game_module_v1_t *mosaico_game_module_v1(void)
{
    return &s_module;
}
