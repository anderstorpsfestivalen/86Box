/*
 * lib86box.c - Library API implementation for 86Box
 *
 * This file implements the public C API defined in lib86box.h,
 * providing a clean interface for embedding 86Box in other applications.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>

#include <86box/86box.h>
#include <86box/config.h>
#include <86box/plat.h>
#include <86box/video.h>
#include <86box/keyboard.h>
#include <86box/mouse.h>
#include <86box/thread.h>
#include <lib86box.h>

/* State tracking */
static bool lib86box_initialized = false;
static bool lib86box_running = false;
static atomic_bool lib86box_fb_dirty = false;

/* Callbacks */
static lib86box_frame_callback_t frame_callback = NULL;
static void *frame_callback_user_data = NULL;
static lib86box_resize_callback_t resize_callback = NULL;
static void *resize_callback_user_data = NULL;

/* Last known framebuffer dimensions (for resize detection) */
static int last_fb_width = 0;
static int last_fb_height = 0;

/*
 * Initialization and shutdown
 */

int lib86box_init(const char *config_path, const char *rom_path)
{
    if (lib86box_initialized) {
        return -1;  /* Already initialized */
    }

    /* Set up paths */
    if (config_path) {
        /* Set config file path - 86Box uses cfg_path global */
        strncpy(cfg_path, config_path, sizeof(cfg_path) - 1);
        cfg_path[sizeof(cfg_path) - 1] = '\0';
    }

    if (rom_path) {
        /* Set ROM path - 86Box uses rom_path global */
        strncpy(exe_path, rom_path, sizeof(exe_path) - 1);
        exe_path[sizeof(exe_path) - 1] = '\0';
    }

    /* Build fake argc/argv for pc_init */
    char *fake_argv[] = { "lib86box", NULL };
    int fake_argc = 1;

    /* Call 86Box's main initialization */
    int result = pc_init(fake_argc, fake_argv);
    if (result != 0) {
        return result;
    }

    lib86box_initialized = true;
    lib86box_running = false;

    return 0;
}

void lib86box_shutdown(void)
{
    if (!lib86box_initialized) {
        return;
    }

    lib86box_running = false;
    pc_close(NULL);
    lib86box_initialized = false;
}

/*
 * Emulation control
 */

void lib86box_start(void)
{
    if (!lib86box_initialized) {
        return;
    }

    dopause = 0;
    lib86box_running = true;
}

void lib86box_pause(void)
{
    if (!lib86box_initialized) {
        return;
    }

    dopause = 1;
}

void lib86box_resume(void)
{
    if (!lib86box_initialized) {
        return;
    }

    dopause = 0;
}

void lib86box_run_frame(void)
{
    if (!lib86box_initialized || !lib86box_running || dopause) {
        return;
    }

    /* Run one frame of emulation */
    pc_run();

    /* Check for resize */
    monitor_t *mon = &monitors[0];
    if (mon->target_buffer) {
        int w = mon->target_buffer->w;
        int h = mon->target_buffer->h;

        if (w != last_fb_width || h != last_fb_height) {
            last_fb_width = w;
            last_fb_height = h;

            if (resize_callback) {
                resize_callback(w, h, resize_callback_user_data);
            }
        }
    }

    /* Mark framebuffer as dirty */
    atomic_store(&lib86box_fb_dirty, true);

    /* Call frame callback */
    if (frame_callback) {
        frame_callback(frame_callback_user_data);
    }
}

bool lib86box_is_running(void)
{
    return lib86box_initialized && lib86box_running && !dopause;
}

void lib86box_reset_hard(void)
{
    if (!lib86box_initialized) {
        return;
    }

    pc_reset_hard();
}

/*
 * Framebuffer access
 */

lib86box_framebuffer_t lib86box_get_framebuffer(void)
{
    lib86box_framebuffer_t fb = { NULL, 0, 0, 0 };

    if (!lib86box_initialized) {
        return fb;
    }

    monitor_t *mon = &monitors[0];
    if (!mon->target_buffer) {
        return fb;
    }

    bitmap_t *bmp = mon->target_buffer;

    fb.data = bmp->dat;
    fb.width = bmp->w;
    fb.height = bmp->h;
    /* Stride is width * 4 bytes per pixel (ARGB32) */
    fb.stride = bmp->w * sizeof(uint32_t);

    return fb;
}

bool lib86box_framebuffer_dirty(void)
{
    return atomic_load(&lib86box_fb_dirty);
}

void lib86box_framebuffer_clear_dirty(void)
{
    atomic_store(&lib86box_fb_dirty, false);
}

/*
 * Input handling
 */

void lib86box_keyboard_input(uint16_t scancode, bool down)
{
    if (!lib86box_initialized) {
        return;
    }

    keyboard_input(down ? 1 : 0, scancode);
}

void lib86box_mouse_move(int dx, int dy)
{
    if (!lib86box_initialized) {
        return;
    }

    mouse_scale(dx, dy);
}

void lib86box_mouse_move_abs(int x, int y)
{
    if (!lib86box_initialized) {
        return;
    }

    /* Set absolute mouse position */
    mouse_x_abs = (double)x;
    mouse_y_abs = (double)y;
}

void lib86box_mouse_buttons(int buttons)
{
    if (!lib86box_initialized) {
        return;
    }

    mouse_set_buttons_ex(buttons);
}

/*
 * Callbacks
 */

void lib86box_set_frame_callback(lib86box_frame_callback_t callback, void *user_data)
{
    frame_callback = callback;
    frame_callback_user_data = user_data;
}

void lib86box_set_resize_callback(lib86box_resize_callback_t callback, void *user_data)
{
    resize_callback = callback;
    resize_callback_user_data = user_data;
}
