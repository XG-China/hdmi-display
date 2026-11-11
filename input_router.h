#ifndef INPUT_ROUTER_H
#define INPUT_ROUTER_H

#include <stdint.h>

/*
 * Physical input device -> screen binding (Linux evdev).
 *
 * Events are normalized and handed to a callback owned by main.c, which
 * forwards them to the VNC/RFB session displayed on the bound output.
 */

typedef struct {
    int type;          /* 0 = pointer, 1 = key */
    int mask;          /* RFB button mask: 1 left, 2 middle, 4 right */
    int x, y;          /* pointer position, 0..65535 (absolute) */
    uint32_t keysym;   /* X11 keysym (VNC) */
    uint16_t keycode;  /* Linux evdev KEY_* (RDP scancode) */
    int down;
    int caps;          /* local Caps Lock state (web/XTEST sync) */
    int wheel;         /* REL_WHEEL clicks, positive = up */
} input_event;

#define INPUT_KIND_KEYBOARD    1
#define INPUT_KIND_MOUSE       2
#define INPUT_KIND_TOUCHPAD    4
#define INPUT_KIND_TOUCHSCREEN 8
#define INPUT_KIND_TABLET      16
#define INPUT_KIND_GAMEPAD     32
#define INPUT_KIND_MEDIA       64
#define INPUT_KIND_SYSTEM      128
#define INPUT_KIND_REMOTE      256
#define INPUT_KIND_OTHER       512
#define INPUT_KIND_MOUSE_EXTRA 1024

typedef struct {
    char id[256];      /* stable device path */
    char group[256];   /* physical device (several evdev nodes share it) */
    char name[128];
    char output[64];   /* bound output name, empty = unbound */
    int  ready;        /* device could be opened */
    int  kind;         /* INPUT_KIND_KEYBOARD / INPUT_KIND_MOUSE */
    unsigned long events;  /* events seen since the app started */
    long last_ms;      /* monotonic time of the last event */
} input_device;

typedef int (*input_route_fn)(const char *output, const input_event *ev, void *ud);

int  input_init(input_route_fn fn, void *ud);
void input_shutdown(void);

int  input_count(void);
int  input_list(input_device *out, int max);
int  input_bind(const char *id, const char *output);

#endif /* INPUT_ROUTER_H */
