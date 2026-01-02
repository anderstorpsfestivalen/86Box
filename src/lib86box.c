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
#include <86box/mem.h>
#include <86box/rom.h>
#include <lib86box.h>

/* State tracking */
static bool lib86box_initialized = false;
static bool lib86box_running = false;
static atomic_bool lib86box_fb_dirty = false;

/* Last blit region (where content is within the 2048x2048 buffer) */
static int blit_x = 0, blit_y = 0, blit_w = 0, blit_h = 0;

/* Callbacks */
static lib86box_frame_callback_t frame_callback = NULL;
static void *frame_callback_user_data = NULL;
static lib86box_resize_callback_t resize_callback = NULL;
static void *resize_callback_user_data = NULL;

/* Last known framebuffer dimensions (for resize detection) */
static int last_fb_width = 0;
static int last_fb_height = 0;

/*
 * Internal blit callback - called by 86Box's video system when a frame is ready.
 * This just marks the framebuffer as dirty and signals completion.
 */
static void lib86box_blit(int x, int y, int w, int h, int monitor_index)
{
    /* Save the blit region - this tells us where content is in the 2048x2048 buffer */
    blit_x = x;
    blit_y = y;
    blit_w = w;
    blit_h = h;

    /* Mark framebuffer as dirty */
    atomic_store(&lib86box_fb_dirty, true);

    /* Signal that we're done with the buffer so 86Box can continue rendering next frame */
    video_blit_complete_monitor(monitor_index);
}

/*
 * Initialization and shutdown
 */

int lib86box_init(const char *config_path, const char *rom_path)
{
    return lib86box_init_ex(config_path, rom_path, NULL);
}

int lib86box_init_ex(const char *config_path, const char *rom_path, const char *global_config_path)
{
    if (lib86box_initialized) {
        return -1;  /* Already initialized */
    }

    /* Build argc/argv for pc_init with proper command-line arguments
     * pc_init() parses these to set up paths correctly */
    static char arg0[] = "lib86box";
    static char arg_config[] = "-C";
    static char arg_rom[] = "-R";
    static char arg_vmpath[] = "-P";
    static char arg_global[] = "-O";
    static char config_buf[2048];
    static char rom_buf[2048];
    static char vmpath_buf[2048];
    static char global_buf[2048];

    char *fake_argv[16];
    int fake_argc = 1;
    fake_argv[0] = arg0;

    if (config_path) {
        strncpy(config_buf, config_path, sizeof(config_buf) - 1);
        config_buf[sizeof(config_buf) - 1] = '\0';
        fake_argv[fake_argc++] = arg_config;
        fake_argv[fake_argc++] = config_buf;

        /* Also set the VM path to the directory containing the config file */
        strncpy(vmpath_buf, config_path, sizeof(vmpath_buf) - 1);
        vmpath_buf[sizeof(vmpath_buf) - 1] = '\0';
        /* Find last slash and truncate to get directory */
        char *last_slash = strrchr(vmpath_buf, '/');
        if (last_slash) {
            *last_slash = '\0';
            fake_argv[fake_argc++] = arg_vmpath;
            fake_argv[fake_argc++] = vmpath_buf;
        }
    }

    if (rom_path) {
        strncpy(rom_buf, rom_path, sizeof(rom_buf) - 1);
        rom_buf[sizeof(rom_buf) - 1] = '\0';
        fake_argv[fake_argc++] = arg_rom;
        fake_argv[fake_argc++] = rom_buf;
    }

    if (global_config_path) {
        strncpy(global_buf, global_config_path, sizeof(global_buf) - 1);
        global_buf[sizeof(global_buf) - 1] = '\0';
        fake_argv[fake_argc++] = arg_global;
        fake_argv[fake_argc++] = global_buf;
    }

    fake_argv[fake_argc] = NULL;

    /* Call 86Box's main initialization
     * Note: pc_init returns 1 for success, 0 for failure */
    int result = pc_init(fake_argc, fake_argv);
    if (result == 0) {
        return 1;  /* Return non-zero to indicate failure to caller */
    }

    /* Check that required ROMs are available */
    if (!pc_init_roms()) {
        return 2;  /* No usable ROMs found */
    }

    /* Initialize modules */
    pc_init_modules();

    /* Register our blit callback so we get notified when frames are ready */
    video_setblit(lib86box_blit);

    /* Fire up the machine - this initializes CPU, memory, devices etc. */
    pc_reset_hard_init();

    lib86box_initialized = true;
    lib86box_running = false;

    return 0;  /* Success */
}

void lib86box_shutdown(void)
{
    if (!lib86box_initialized) {
        return;
    }

    lib86box_running = false;
    video_setblit(NULL);  /* Unregister blit callback */
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

    /* Run emulation for ~16ms worth of time (targeting 60fps display).
     * pc_run() executes cpu_s->rspeed/1000 cycles, which is ~1ms of emulation.
     * We need to call it multiple times to keep up with real time. */
    for (int i = 0; i < 16; i++) {
        pc_run();
    }

    /* Process mouse input - the timer-based polling may not work reliably in lib mode */
    mouse_process();

    /* Check for resize using unscaled size (the actual pixel dimensions set by set_screen_size).
     * mon_xsize/mon_ysize may be set by some video cards but mon_unscaled_size_x/y is more reliable. */
    monitor_t *mon = &monitors[0];
    if (mon->target_buffer && mon->mon_unscaled_size_x > 0 && mon->mon_unscaled_size_y > 0) {
        int w = mon->mon_unscaled_size_x;
        int h = mon->mon_unscaled_size_y;

        if (w != last_fb_width || h != last_fb_height) {
            last_fb_width = w;
            last_fb_height = h;

            if (resize_callback) {
                resize_callback(w, h, resize_callback_user_data);
            }
        }
    }

    /* Note: framebuffer dirty flag is set by lib86box_blit callback, not here */

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
    lib86box_framebuffer_t fb = { NULL, 0, 0, 0, 0, 0 };

    if (!lib86box_initialized) {
        return fb;
    }

    monitor_t *mon = &monitors[0];
    if (!mon->target_buffer) {
        return fb;
    }

    bitmap_t *bmp = mon->target_buffer;

    /* Return framebuffer with offset information.
     * The blit callback provides x, y, w, h which tell us where in the 2048x2048
     * buffer the actual content is located. The caller should use these offsets. */
    fb.data = bmp->dat;
    fb.x = blit_x;
    fb.y = blit_y;
    fb.width = blit_w > 0 ? blit_w : bmp->w;
    fb.height = blit_h > 0 ? blit_h : bmp->h;
    /* Stride is allocated buffer width * 4 bytes per pixel (ARGB32) */
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
    /* Process immediately for lower latency */
    mouse_process();
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
    /* Process immediately for lower latency */
    mouse_process();
}

void lib86box_mouse_set_capture(int captured)
{
    mouse_capture = captured;
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
