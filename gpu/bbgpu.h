/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} BbGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* A trophy the game unlocked (runtime_services.c): the overlay's notice. grade: P, G, S, B or '-';
 * icon: a TROPxxx.rgba file of scripts/trophies.py (u32 width, u32 height, RGBA), or NULL. */
void bbgpu_trophy_notice(int id, char grade, const char *name_utf8, const char *icon_path);
/* Mouse look (runtime_pad.c): the window takes the mouse when it is clicked (relative mode) and
 * lets it go on F1, losing focus or the menu opening. take: motion (pixels) and wheel notches
 * since the last call, reset; captured: 1 while the window holds it; enable: mouse_look=0 in
 * bbport.ini keeps the window from taking it. */
void bbgpu_mouse_take(double *dx, double *dy, int *wheel_up, int *wheel_down);
int bbgpu_mouse_captured(void);
void bbgpu_mouse_look_enable(int enabled);
/* Patches the loaded image before the game runs (image still writable): libGnm entry hooks. */
void bbgpu_patch_image(unsigned char *image, uint64_t size);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);

/* The input devices the launcher shows (the controls page): the connected gamepads and the
 * keyboard. The launcher asks before the game starts, so the list is built here (the SDL
 * that reads the pads is here too).
 *
 * A device is written into the buffer as a line:
 *   <kind>\t<guid>\t<name>\t<architecture>
 * kind: "pad" or "keyboard"; architecture: "xinput", "directinput" or "" (a keyboard has
 * none). The buffer is filled with the lines and its length is returned; the caller
 * gives a buffer of 64 lines of 512 bytes. */
int bbgpu_input_devices(char *buffer, int size);
/* The buttons a gamepad has, as the names the runtime's configuration uses (SDL's:
 * "a", "b", "x", "y", "back", "start", "leftstick", "rightstick", "leftshoulder",
 * "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "touchpad", "misc1",
 * "paddle1" ... "paddle4", "lefttrigger", "righttrigger"), comma-separated, in the
 * buffer. Returns the length written, or 0 when the device has no gamepad buttons. */
int bbgpu_input_buttons(const char *guid, char *buffer, int size);
#ifdef __cplusplus
}
#endif
#endif
