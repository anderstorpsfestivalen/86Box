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
#ifdef __APPLE__
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

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
static lib86box_log_callback_t log_callback = NULL;
static void *log_callback_user_data = NULL;

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
    return lib86box_init_full(config_path, rom_path, NULL, NULL);
}

int lib86box_init_ex(const char *config_path, const char *rom_path, const char *global_config_path)
{
    return lib86box_init_full(config_path, rom_path, NULL, global_config_path);
}

int lib86box_init_full(const char *config_path, const char *rom_path, const char *asset_path, const char *global_config_path)
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
    static char arg_asset[] = "-A";
    static char config_buf[2048];
    static char rom_buf[2048];
    static char vmpath_buf[2048];
    static char global_buf[2048];
    static char asset_buf[2048];

    char *fake_argv[20];
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

    if (asset_path) {
        strncpy(asset_buf, asset_path, sizeof(asset_buf) - 1);
        asset_buf[sizeof(asset_buf) - 1] = '\0';
        fake_argv[fake_argc++] = arg_asset;
        fake_argv[fake_argc++] = asset_buf;
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

/* Emulation timing - track absolute ticks to never lose time */
static uint64_t emu_start_time_us = 0;
static uint64_t emu_total_ticks = 0;

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

    /* Reset timing to avoid catchup burst after pause */
    emu_start_time_us = 0;

    dopause = 0;
}

/* Get current time in microseconds for precision */
static uint64_t get_time_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)(count.QuadPart * 1000000 / freq.QuadPart);
#elif defined(__APPLE__)
    /* Use mach_absolute_time on macOS */
    static mach_timebase_info_data_t timebase = {0};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    uint64_t ticks = mach_absolute_time();
    /* Convert to microseconds */
    return (ticks * timebase.numer / timebase.denom) / 1000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

void lib86box_run_frame(void)
{
    if (!lib86box_initialized || !lib86box_running || dopause) {
        return;
    }

    uint64_t now_us = get_time_us();

    /* Initialize start time on first call */
    if (emu_start_time_us == 0) {
        emu_start_time_us = now_us;
        emu_total_ticks = 0;
    }

    /* Calculate expected ticks based on elapsed real time (1 tick = 1ms) */
    uint64_t elapsed_us = now_us - emu_start_time_us;
    uint64_t expected_ticks = elapsed_us / 1000;

    /* How many ticks we need to run to catch up */
    int64_t ticks_needed = (int64_t)(expected_ticks - emu_total_ticks);

    /* Cap per-frame to avoid death spiral, but track deficit for next frame */
    if (ticks_needed > 100) {
        /* We're too far behind - reset timing to avoid permanent catchup mode */
        emu_start_time_us = now_us;
        emu_total_ticks = 0;
        ticks_needed = 16; /* Run a normal frame's worth */
    }

    /* Run pc_run() for each tick we owe */
    for (int64_t i = 0; i < ticks_needed; i++) {
        pc_run();
        emu_total_ticks++;
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

/*
 * Run a single emulation tick (1ms of emulated time).
 * Call this in a tight loop from your emulation thread.
 * This is more suitable for threaded operation than lib86box_run_frame().
 * Returns true if a new frame was completed (framebuffer updated).
 */
bool lib86box_run_tick(void)
{
    if (!lib86box_initialized || !lib86box_running || dopause) {
        return false;
    }

    /* Check dirty state before running - we'll compare after */
    bool was_dirty = atomic_load(&lib86box_fb_dirty);

    /* Run one tick of emulation */
    pc_run();

    /* Check for resize */
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

    /* Return true if framebuffer became dirty during this tick */
    return !was_dirty && atomic_load(&lib86box_fb_dirty);
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

void lib86box_set_log_callback(lib86box_log_callback_t callback, void *user_data)
{
    log_callback = callback;
    log_callback_user_data = user_data;
}

/* Internal function called by 86box.c logging functions */
int lib86box_log_has_callback(void)
{
    return log_callback != NULL;
}

void lib86box_log_emit(int level, const char *message)
{
    if (log_callback) {
        log_callback(level, message, log_callback_user_data);
    }
}
