/*
 * headless.c - Headless platform backend for lib86box
 *
 * This provides platform functions for running 86Box as a library
 * without requiring Qt, SDL, or any GUI dependencies.
 * Based on src/unix/unix.c but with SDL removed.
 */

#ifdef __linux__
#    define _GNU_SOURCE
#    define _FILE_OFFSET_BITS   64
#    define _LARGEFILE64_SOURCE 1
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <dlfcn.h>
#include <wchar.h>
#include <pwd.h>
#include <stdatomic.h>
#include <pthread.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#include <86box/86box.h>
#include <86box/mem.h>
#include <86box/rom.h>
#include <86box/keyboard.h>
#include <86box/mouse.h>
#include <86box/config.h>
#include <86box/path.h>
#include <86box/plat.h>
#include <86box/plat_dynld.h>
#include <86box/thread.h>
#include <86box/device.h>
#include <86box/gameport.h>
#include "cpu.h"
#include <86box/timer.h>
#include <86box/nvr.h>
#include <86box/version.h>
#include <86box/video.h>
#include <86box/ui.h>
#include <86box/gdbstub.h>

/*
 * Global variables required by 86Box
 * Note: many are defined in 86box.c - only define platform-specific ones here
 */
int             mouse_capture = 0;
int             infocus = 1;
int             rctrl_is_lalt = 0;
int             update_icons = 0;
int             kbd_req_capture = 0;
int             hide_status_bar = 0;
int             hide_tool_bar = 0;
int             fixed_size_x = 640;
int             fixed_size_y = 480;
volatile int    cpu_thread_run = 1;

/* Timer state */
static uint64_t StartingTime = 0;
static uint64_t Frequency = 1000000000ULL;
static int      first_use = 1;

/* Blit mutex */
static pthread_mutex_t blit_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Timer functions
 */

static uint64_t get_time_ns(void)
{
#ifdef __APPLE__
    static mach_timebase_info_data_t timebase;
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    return mach_absolute_time() * timebase.numer / timebase.denom;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

uint64_t
plat_timer_read(void)
{
    return get_time_ns();
}

static uint64_t
plat_get_ticks_common(void)
{
    uint64_t EndingTime;
    uint64_t ElapsedMicroseconds;

    if (first_use) {
        Frequency    = 1000000000ULL;
        StartingTime = get_time_ns();
        first_use    = 0;
    }
    EndingTime          = get_time_ns();
    ElapsedMicroseconds = (EndingTime - StartingTime) / 1000;

    return ElapsedMicroseconds;
}

uint32_t
plat_get_ticks(void)
{
    return (uint32_t) (plat_get_ticks_common() / 1000);
}

void
plat_delay_ms(uint32_t count)
{
    usleep(count * 1000);
}

/*
 * Localized strings - minimal implementation
 */

wchar_t *
plat_get_string(UNUSED(int id))
{
    return L"";
}

/*
 * File system functions
 */

FILE *
plat_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

FILE *
plat_fopen64(const char *path, const char *mode)
{
    return fopen(path, mode);
}

void
plat_remove(char *path)
{
    remove(path);
}

int
plat_getcwd(char *bufp, int max)
{
    return getcwd(bufp, max) != NULL ? 0 : -1;
}

int
plat_chdir(char *path)
{
    return chdir(path);
}

void
plat_tempfile(char *bufp, char *prefix, char *suffix)
{
    snprintf(bufp, 1024, "/tmp/%s%d%s", prefix ? prefix : "", (int)getpid(), suffix ? suffix : "");
}

void
plat_get_exe_name(char *s, int size)
{
    strncpy(s, "lib86box", size - 1);
    s[size - 1] = '\0';
}

void
plat_get_global_config_dir(char *outbuf, size_t len)
{
    const char *home = getenv("HOME");
    if (home) {
        snprintf(outbuf, len, "%s/.config/86Box", home);
    } else {
        strncpy(outbuf, "/tmp/86Box", len);
    }
}

void
plat_get_global_data_dir(char *outbuf, size_t len)
{
    plat_get_global_config_dir(outbuf, len);
}

void
plat_get_temp_dir(char *outbuf, uint8_t len)
{
    strncpy(outbuf, "/tmp", len);
}

void
plat_get_vmm_dir(char *outbuf, size_t len)
{
    /* Return empty string to disable VM manager mode in library builds */
    if (len > 0) {
        outbuf[0] = '\0';
    }
}

void
plat_init_rom_paths(void)
{
    /* No-op for headless */
}

void
plat_init_asset_paths(void)
{
    /* No-op for headless */
}

int
plat_dir_check(char *path)
{
    struct stat stats;
    if (stat(path, &stats) < 0)
        return 0;
    return S_ISDIR(stats.st_mode);
}

int
plat_file_check(const char *path)
{
    struct stat stats;
    if (stat(path, &stats) < 0)
        return 0;
    return !S_ISDIR(stats.st_mode);
}

int
plat_dir_create(char *path)
{
    return mkdir(path, S_IRWXU);
}

void *
plat_mmap(size_t size, uint8_t executable)
{
#if defined __APPLE__ && defined MAP_JIT
    void *ret = mmap(0, size, PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0), MAP_ANON | MAP_PRIVATE | (executable ? MAP_JIT : 0), -1, 0);
#elif defined(PROT_MPROTECT)
    void *ret = mmap(0, size, PROT_MPROTECT(PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0)), MAP_ANON | MAP_PRIVATE, -1, 0);
#else
    void *ret = mmap(0, size, PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0), MAP_ANON | MAP_PRIVATE, -1, 0);
#endif
    return (ret == MAP_FAILED) ? NULL : ret;
}

void
plat_munmap(void *ptr, size_t size)
{
    if (ptr)
        munmap(ptr, size);
}

/*
 * Path functions - from unix.c
 */

int
path_abs(char *path)
{
    return path[0] == '/';
}

void
path_normalize(UNUSED(char *path))
{
    /* No-op on Unix */
}

void
path_slash(char *path)
{
    if (path[strlen(path) - 1] != '/') {
        strcat(path, "/");
    }
    path_normalize(path);
}

const char *
path_get_slash(char *path)
{
    char *ret = "";

    if (path[strlen(path) - 1] != '/')
        ret = "/";

    return ret;
}

void
plat_put_backslash(char *s)
{
    int c = strlen(s) - 1;

    if (s[c] != '/')
        s[c] = '/';
}

char *
path_get_basename(const char *path)
{
    int c = (int) strlen(path);

    while (c > 0) {
        if (path[c] == '/')
            return ((char *) &path[c + 1]);
        c--;
    }

    return ((char *) path);
}

char *
path_get_filename(char *s)
{
    int c = strlen(s) - 1;

    while (c > 0) {
        if (s[c] == '/' || s[c] == '\\')
            return (&s[c + 1]);
        c--;
    }

    return s;
}

char *
path_get_extension(char *s)
{
    int c = strlen(s) - 1;

    if (c <= 0)
        return s;

    while (c && s[c] != '.')
        c--;

    if (!c)
        return (&s[strlen(s)]);

    return (&s[c + 1]);
}

void
path_append_filename(char *dest, const char *s1, const char *s2)
{
    strcpy(dest, s1);
    path_slash(dest);
    strcat(dest, s2);
}

void
path_get_dirname(char *dest, const char *path)
{
    int   c = (int) strlen(path);
    char *ptr = (char *) path;

    while (c > 0) {
        if (path[c] == '/' || path[c] == '\\') {
            ptr = (char *) &path[c];
            break;
        }
        c--;
    }

    /* Copy to destination. */
    while (path < ptr)
        *dest++ = *path++;
    *dest = '\0';
}

/*
 * Display/UI functions (no-op for headless)
 */

void
plat_pause(int p)
{
    if ((!!p) == dopause)
        return;

    if ((p == 0) && (time_sync & TIME_SYNC_ENABLED))
        nvr_time_sync();

    do_pause(p);
}

void
plat_mouse_capture(int on)
{
    mouse_capture = on;
}

int
plat_vidapi(UNUSED(const char *name))
{
    return 0;  /* Software rendering */
}

char *
plat_vidapi_name(UNUSED(int api))
{
    return "headless";
}

void
plat_resize(UNUSED(int x), UNUSED(int y), UNUSED(int monitor_index))
{
    /* No-op for headless */
}

void
plat_resize_request(UNUSED(int x), UNUSED(int y), UNUSED(int monitor_index))
{
    /* No-op for headless */
}

int
plat_language_code(UNUSED(char *langcode))
{
    return 0;
}

void
plat_language_code_r(UNUSED(int id), char *outbuf, int len)
{
    strncpy(outbuf, "en", len);
}

void
plat_get_cpu_string(char *outbuf, uint8_t len)
{
    strncpy(outbuf, "Unknown CPU", len);
}

void
plat_set_thread_name(void *thread, const char *name)
{
#ifdef __APPLE__
    /* macOS only allows setting current thread name */
    (void)thread;
    if (name)
        pthread_setname_np(name);
#elif defined(__linux__)
    if (thread)
        pthread_setname_np((pthread_t)thread, name);
#else
    (void)thread;
    (void)name;
#endif
}

void
plat_break(void)
{
    /* No-op for headless */
}

void
plat_send_to_clipboard(UNUSED(unsigned char *rgb), UNUSED(int width), UNUSED(int height))
{
    /* No-op for headless */
}

wchar_t *
ui_window_title(wchar_t *str)
{
    static wchar_t title[512] = L"86Box";
    if (str)
        wcsncpy(title, str, 511);
    return title;
}

void
plat_power_off(void)
{
    confirm_exit_cmdl = 0;
    nvr_save();
    config_save();

    /* Deduct a sufficiently large number of cycles that no instructions will
       run before the main thread is terminated */
    cycles -= 99999999;

    cpu_thread_run = 0;
}

/*
 * Device mount/eject stubs
 */

void cassette_mount(UNUSED(char *fn), UNUSED(uint8_t wp)) { }
void cassette_eject(void) { }
void cartridge_mount(UNUSED(uint8_t id), UNUSED(char *fn), UNUSED(uint8_t wp)) { }
void cartridge_eject(UNUSED(uint8_t id)) { }
void floppy_mount(UNUSED(uint8_t id), UNUSED(char *fn), UNUSED(uint8_t wp)) { }
void floppy_eject(UNUSED(uint8_t id)) { }
void cdrom_mount(UNUSED(uint8_t id), UNUSED(char *fn)) { }
void plat_cdrom_ui_update(UNUSED(uint8_t id), UNUSED(uint8_t reload)) { }
void rdisk_eject(UNUSED(uint8_t id)) { }
void rdisk_mount(UNUSED(uint8_t id), UNUSED(char *fn), UNUSED(uint8_t wp)) { }
void rdisk_reload(UNUSED(uint8_t id)) { }
void mo_eject(UNUSED(uint8_t id)) { }
void mo_mount(UNUSED(uint8_t id), UNUSED(char *fn), UNUSED(uint8_t wp)) { }
void mo_reload(UNUSED(uint8_t id)) { }

/*
 * Blit synchronization
 */

void
startblit(void)
{
    pthread_mutex_lock(&blit_mutex);
}

void
endblit(void)
{
    pthread_mutex_unlock(&blit_mutex);
}

/*
 * UI status bar stubs
 */

void ui_sb_set_text(UNUSED(char *str)) { }
void ui_sb_set_text_w(UNUSED(wchar_t *str)) { }
void ui_sb_update_icon(UNUSED(int tag), UNUSED(int active)) { }
void ui_sb_update_icon_state(UNUSED(int tag), UNUSED(int state)) { }
void ui_sb_update_icon_wp(UNUSED(int tag), UNUSED(int wp)) { }
void ui_sb_update_icon_write(UNUSED(int tag), UNUSED(int writing)) { }
void ui_sb_update_panes(void) { }
void ui_sb_update_text(void) { }
void ui_sb_update_tip(UNUSED(int arg)) { }
void ui_sb_bugui(UNUSED(char *str)) { }
void ui_sb_mt32lcd(UNUSED(char *str)) { }
void ui_sb_set_ready(UNUSED(int ready)) { }
void ui_init_monitor(UNUSED(int monitor_index)) { }
void ui_deinit_monitor(UNUSED(int monitor_index)) { }
void ui_hard_reset_completed(void) { }

int
ui_msgbox(UNUSED(int flags), void *message)
{
    if (message)
        fprintf(stderr, "86Box: %s\n", (char *)message);
    return 0;
}

int
ui_msgbox_header(UNUSED(int flags), UNUSED(void *header), void *message)
{
    if (message)
        fprintf(stderr, "86Box: %s\n", (char *)message);
    return 0;
}

/*
 * Main thread and emulation control - for library use, these are controlled
 * by the lib86box API, not internal threading
 */

thread_t *thMain = NULL;

void
do_start(void)
{
    /* Initialize the high-precision timer. */
    timer_freq = 1000000000ULL;
    is_quit = 0;
    /* Note: In library mode, the caller runs the main loop via lib86box_run_frame() */
}

void
do_stop(void)
{
    is_quit = 1;
}

/*
 * UTF conversion functions
 */

size_t
mbstoc16s(uint16_t dst[], const char src[], int len)
{
    size_t i;
    for (i = 0; i < (size_t)len - 1 && src[i]; i++) {
        dst[i] = (uint16_t)(unsigned char)src[i];
    }
    dst[i] = 0;
    return i;
}

size_t
c16stombs(char dst[], const uint16_t src[], int len)
{
    size_t i;
    for (i = 0; i < (size_t)len - 1 && src[i]; i++) {
        dst[i] = (char)(src[i] < 128 ? src[i] : '?');
    }
    dst[i] = '\0';
    return i;
}

/*
 * String functions for compatibility
 */

#ifndef _WIN32
int
stricmp(const char *s1, const char *s2)
{
    return strcasecmp(s1, s2);
}

int
strnicmp(const char *s1, const char *s2, size_t n)
{
    return strncasecmp(s1, s2, n);
}
#endif

/*
 * Dynamic library loading
 */

void *
dynld_module(const char *name, dllimp_t *table)
{
    void *handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!handle)
        return NULL;

    for (dllimp_t *imp = table; imp->name; imp++) {
        void *sym = dlsym(handle, imp->name);
        if (!sym && imp->func) {
            dlclose(handle);
            return NULL;
        }
        if (imp->func)
            *(void **)imp->func = sym;
    }

    return handle;
}

void
dynld_close(void *handle)
{
    if (handle)
        dlclose(handle);
}

/*
 * Joystick stubs (no joystick in headless mode)
 */

/* Global joystick state required by config */
joystick_state_t      joystick_state[GAMEPORT_MAX][MAX_JOYSTICKS];
plat_joystick_state_t plat_joystick_state[MAX_PLAT_JOYSTICKS];
int                   joysticks_present = 0;

void
joystick_init(void)
{
    memset(joystick_state, 0, sizeof(joystick_state));
    memset(plat_joystick_state, 0, sizeof(plat_joystick_state));
    joysticks_present = 0;
}

void
joystick_close(void)
{
    /* No-op */
}

void
joystick_process(UNUSED(uint8_t gp))
{
    /* No-op */
}

/*
 * OpenGL shader file path (used by config)
 */

char gl3_shader_file[512] = "";

/*
 * From musl - local_strsep
 */

char *
local_strsep(char **str, const char *sep)
{
    char *s = *str;
    char *end;

    if (!s)
        return NULL;
    end = s + strcspn(s, sep);
    if (*end)
        *end++ = 0;
    else
        end = 0;
    *str = end;

    return s;
}
