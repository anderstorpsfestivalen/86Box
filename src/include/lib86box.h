/*
 * lib86box.h - Public API for using 86Box as a library
 *
 * This header provides a clean C API for embedding 86Box in other applications.
 * It allows initialization, control, framebuffer access, and input handling
 * without requiring Qt or SDL dependencies.
 */

#ifndef LIB86BOX_H
#define LIB86BOX_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version info */
#define LIB86BOX_VERSION_MAJOR 1
#define LIB86BOX_VERSION_MINOR 0

/*
 * Framebuffer structure - provides direct access to video output
 * Format is ARGB32 (0xAARRGGBB), which on little-endian is BGRA byte order
 *
 * The buffer is typically 2048x2048, but actual content is at (x,y) with size (width,height).
 * Use x and y as offsets into the data when copying.
 */
typedef struct {
    uint32_t *data;     /* Pixel data pointer (to full buffer, use x/y offset) */
    int x;              /* X offset of content in buffer */
    int y;              /* Y offset of content in buffer */
    int width;          /* Width of content in pixels */
    int height;         /* Height of content in pixels */
    int stride;         /* Bytes per row of the full buffer */
} lib86box_framebuffer_t;

/*
 * Initialization and shutdown
 */

/* Initialize 86Box with config file and ROM path
 * Returns 0 on success, non-zero on failure */
int lib86box_init(const char *config_path, const char *rom_path);

/* Shutdown 86Box and free all resources */
void lib86box_shutdown(void);

/*
 * Emulation control
 */

/* Start emulation (must call after init) */
void lib86box_start(void);

/* Pause emulation */
void lib86box_pause(void);

/* Resume emulation after pause */
void lib86box_resume(void);

/* Run one frame of emulation
 * Call this repeatedly from your main loop
 * Executes approximately 1/100th of a second of emulated time */
void lib86box_run_frame(void);

/* Check if emulation is currently running */
bool lib86box_is_running(void);

/* Perform a hard reset */
void lib86box_reset_hard(void);

/*
 * Framebuffer access
 */

/* Get current framebuffer
 * Returns struct with pointer to pixel data and dimensions
 * Data is valid until next lib86box_run_frame() call */
lib86box_framebuffer_t lib86box_get_framebuffer(void);

/* Check if framebuffer has changed since last call
 * Useful for dirty-rect optimization */
bool lib86box_framebuffer_dirty(void);

/* Clear the dirty flag after copying framebuffer */
void lib86box_framebuffer_clear_dirty(void);

/*
 * Input handling
 */

/* Send keyboard input
 * scancode: PC/AT keyboard scancode (set 1)
 * down: true for key press, false for key release */
void lib86box_keyboard_input(uint16_t scancode, bool down);

/* Send relative mouse movement
 * dx, dy: movement delta in pixels */
void lib86box_mouse_move(int dx, int dy);

/* Send absolute mouse position
 * x, y: screen coordinates (0,0 is top-left) */
void lib86box_mouse_move_abs(int x, int y);

/* Set mouse button state
 * buttons: bitmask (bit 0 = left, bit 1 = right, bit 2 = middle) */
void lib86box_mouse_buttons(int buttons);

/* Set mouse capture state
 * captured: 1 = mouse is captured by host, 0 = not captured
 * This must be set to 1 for mouse input to work */
void lib86box_mouse_set_capture(int captured);

/*
 * Callbacks for events (optional)
 */

/* Frame complete callback - called after each frame is rendered */
typedef void (*lib86box_frame_callback_t)(void *user_data);
void lib86box_set_frame_callback(lib86box_frame_callback_t callback, void *user_data);

/* Resize callback - called when display resolution changes */
typedef void (*lib86box_resize_callback_t)(int width, int height, void *user_data);
void lib86box_set_resize_callback(lib86box_resize_callback_t callback, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* LIB86BOX_H */
