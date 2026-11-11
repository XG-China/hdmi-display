/*
 * input_router.c - bind physical Linux keyboards/mice to a screen.
 *
 * Reads /dev/input/evdev devices (read-only) and hands normalized events to
 * main.c, which forwards them to the VNC session shown on that output.  This
 * makes "one keyboard/mouse per VM console" work without touching the host's
 * cursor.
 *
 * Permissions: run as root, or add the user to the "input" group.
 */
#define _GNU_SOURCE

#include "input_router.h"
#include "os_thread.h"
#include "display.h"

#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_DEV 32
#define CURSOR_SPEED 96

typedef struct {
    char id[256];
    char name[128];
    char output[64];
    int  ready;

    int fd;
    os_thread_t thread;
    volatile int alive;

    int mask;
    int x, y;   /* 0..65535 */
    int kind;   /* INPUT_KIND_* */
    char group[256];
    char physkey[128];
    volatile unsigned long events;
    volatile long last_ms;
} in_dev;

static in_dev g_dev[MAX_DEV];
static int g_ndev;
static os_mutex_t g_lock;
static input_route_fn g_route;
static void *g_ud;
static volatile int g_run;

static void clamp_cursor(in_dev *d)
{
    if (d->x < 0) d->x = 0;
    if (d->x > 65535) d->x = 65535;
    if (d->y < 0) d->y = 0;
    if (d->y > 65535) d->y = 65535;
}

static void deliver(in_dev *d, const input_event *ev)
{
    char out[64];
    os_mutex_lock(&g_lock);
    snprintf(out, sizeof(out), "%s", d->output);
    os_mutex_unlock(&g_lock);

    if (out[0] && g_route)
        g_route(out, ev, g_ud);
}

static uint32_t linux_keysym(uint16_t code, int shift, int caps)
{
    /* VNC uses the keysym as the character to type; the X clients send the
     * shifted form as well. Apply the local Shift/Caps state so a guest sees
     * the same case even when its VNC server does not combine modifiers. */
    int upper = (shift != 0) ^ (caps != 0);

    switch (code) {
    /* Linux keycodes are physical positions, not alphabetical; map the three
     * letter rows explicitly. */
    case KEY_Q: return upper ? 'Q' : 'q';
    case KEY_W: return upper ? 'W' : 'w';
    case KEY_E: return upper ? 'E' : 'e';
    case KEY_R: return upper ? 'R' : 'r';
    case KEY_T: return upper ? 'T' : 't';
    case KEY_Y: return upper ? 'Y' : 'y';
    case KEY_U: return upper ? 'U' : 'u';
    case KEY_I: return upper ? 'I' : 'i';
    case KEY_O: return upper ? 'O' : 'o';
    case KEY_P: return upper ? 'P' : 'p';
    case KEY_A: return upper ? 'A' : 'a';
    case KEY_S: return upper ? 'S' : 's';
    case KEY_D: return upper ? 'D' : 'd';
    case KEY_F: return upper ? 'F' : 'f';
    case KEY_G: return upper ? 'G' : 'g';
    case KEY_H: return upper ? 'H' : 'h';
    case KEY_J: return upper ? 'J' : 'j';
    case KEY_K: return upper ? 'K' : 'k';
    case KEY_L: return upper ? 'L' : 'l';
    case KEY_Z: return upper ? 'Z' : 'z';
    case KEY_X: return upper ? 'X' : 'x';
    case KEY_C: return upper ? 'C' : 'c';
    case KEY_V: return upper ? 'V' : 'v';
    case KEY_B: return upper ? 'B' : 'b';
    case KEY_N: return upper ? 'N' : 'n';
    case KEY_M: return upper ? 'M' : 'm';
    default: break;
    }
    if (code >= KEY_1 && code <= KEY_9) {
        static const char plain[] = "123456789";
        static const char shifted[] = "!@#$%^&*(";
        int i = code - KEY_1;
        return (uint32_t)(shift ? shifted[i] : plain[i]);
    }
    if (code == KEY_0)
        return (uint32_t)(shift ? ')' : '0');

    switch (code) {
    case KEY_ESC:        return 0xff1b;
    case KEY_BACKSPACE:  return 0xff08;
    case KEY_TAB:        return 0xff09;
    case KEY_ENTER:      return 0xff0d;
    case KEY_SPACE:      return ' ';
    case KEY_LEFT:       return 0xff51;
    case KEY_UP:         return 0xff52;
    case KEY_RIGHT:      return 0xff53;
    case KEY_DOWN:       return 0xff54;
    case KEY_HOME:       return 0xff50;
    case KEY_END:        return 0xff57;
    case KEY_PAGEUP:     return 0xff55;
    case KEY_PAGEDOWN:   return 0xff56;
    case KEY_INSERT:     return 0xff63;
    case KEY_DELETE:     return 0xffff;
    case KEY_LEFTSHIFT:  return 0xffe1;
    case KEY_RIGHTSHIFT: return 0xffe2;
    case KEY_LEFTCTRL:   return 0xffe3;
    case KEY_RIGHTCTRL:  return 0xffe4;
    case KEY_LEFTALT:    return 0xffe9;
    case KEY_RIGHTALT:   return 0xffea;
    case KEY_LEFTMETA:   return 0xffeb;
    case KEY_RIGHTMETA:  return 0xffec;
    case KEY_CAPSLOCK:   return 0xffe5;
    case KEY_NUMLOCK:    return 0xff7f;
    case KEY_SCROLLLOCK: return 0xff14;
    case KEY_MINUS:      return shift ? '_' : '-';
    case KEY_EQUAL:      return shift ? '+' : '=';
    case KEY_LEFTBRACE:  return shift ? '{' : '[';
    case KEY_RIGHTBRACE: return shift ? '}' : ']';
    case KEY_SEMICOLON:  return shift ? ':' : ';';
    case KEY_APOSTROPHE: return shift ? '"' : '\'';
    case KEY_GRAVE:      return shift ? '~' : '`';
    case KEY_BACKSLASH:  return shift ? '|' : '\\';
    case KEY_COMMA:      return shift ? '<' : ',';
    case KEY_DOT:        return shift ? '>' : '.';
    case KEY_SLASH:      return shift ? '?' : '/';

    /* function keys: F1..F10, F11/F12 and F13..F24 are not contiguous in
     * linux/input-event-codes.h, so map them explicitly */
    case KEY_F1:  return 0xffbe;
    case KEY_F2:  return 0xffbf;
    case KEY_F3:  return 0xffc0;
    case KEY_F4:  return 0xffc1;
    case KEY_F5:  return 0xffc2;
    case KEY_F6:  return 0xffc3;
    case KEY_F7:  return 0xffc4;
    case KEY_F8:  return 0xffc5;
    case KEY_F9:  return 0xffc6;
    case KEY_F10: return 0xffc7;
    case KEY_F11: return 0xffc8;
    case KEY_F12: return 0xffc9;
    case KEY_F13: return 0xffca;
    case KEY_F14: return 0xffcb;
    case KEY_F15: return 0xffcc;
    case KEY_F16: return 0xffcd;
    case KEY_F17: return 0xffce;
    case KEY_F18: return 0xffcf;
    case KEY_F19: return 0xffd0;
    case KEY_F20: return 0xffd1;
    case KEY_F21: return 0xffd2;
    case KEY_F22: return 0xffd3;
    case KEY_F23: return 0xffd4;
    case KEY_F24: return 0xffd5;

    /* keypad: send the equivalent main-key symbols so NumLock state on the
     * guest cannot turn digits into navigation keys. */
    case KEY_KP0: return '0';
    case KEY_KP1: return '1';
    case KEY_KP2: return '2';
    case KEY_KP3: return '3';
    case KEY_KP4: return '4';
    case KEY_KP5: return '5';
    case KEY_KP6: return '6';
    case KEY_KP7: return '7';
    case KEY_KP8: return '8';
    case KEY_KP9: return '9';
    case KEY_KPENTER: return 0xff0d;
    case KEY_KPSLASH: return '/';
    case KEY_KPASTERISK: return '*';
    case KEY_KPMINUS: return '-';
    case KEY_KPPLUS: return '+';
    case KEY_KPDOT: return '.';
    case KEY_KPCOMMA: return ',';
    case KEY_KPEQUAL: return '=';

    /* system / editing */
    case KEY_PAUSE:   return 0xff13;
    case KEY_SYSRQ:   return 0xff15;
    case KEY_PRINT:   return 0xff61;
    case KEY_MENU:    return 0xff67;
    case KEY_COMPOSE: return 0xff20;
    case KEY_UNDO:    return 0xff65;
    case KEY_AGAIN:   return 0xff66;
    case KEY_HELP:    return 0xff6a;

    /* media keys (mostly for desktop guests) */
    case KEY_MUTE:         return 0x1008ff12;
    case KEY_VOLUMEDOWN:   return 0x1008ff11;
    case KEY_VOLUMEUP:     return 0x1008ff13;
    case KEY_PLAYPAUSE:    return 0x1008ff14;
    case KEY_STOPCD:       return 0x1008ff15;
    case KEY_PREVIOUSSONG: return 0x1008ff16;
    case KEY_NEXTSONG:     return 0x1008ff17;

    default: break;
    }
    return 0;
}

static void *linux_reader(void *arg)
{
    in_dev *d = arg;
    struct input_event ev;
    int left_shift = 0, right_shift = 0, caps = 0;

    /* Start from the physical LED state so Caps Lock is not inverted when a
     * keyboard is plugged in with Caps already on. */
    unsigned long leds = 0;
    if (ioctl(d->fd, EVIOCGLED(sizeof(leds)), &leds) == 0 &&
        (leds & (1UL << LED_CAPSL)))
        caps = 1;

    while (g_run && d->alive) {
        ssize_t n = read(d->fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                disp_sleep_ms(5);
                continue;
            }
            break;
        }

        if (ev.type == EV_KEY || ev.type == EV_REL) {
            d->events++;
            d->last_ms = (long)(disp_now() * 1000.0);
        }

        if (ev.type == EV_KEY) {
            if (ev.code == KEY_LEFTSHIFT) {
                left_shift = ev.value ? 1 : 0;
            } else if (ev.code == KEY_RIGHTSHIFT) {
                right_shift = ev.value ? 1 : 0;
            } else if (ev.code == KEY_CAPSLOCK && ev.value == 1) {
                caps ^= 1;
            }
            int shift = left_shift || right_shift;

            if (ev.code == BTN_LEFT || ev.code == BTN_RIGHT ||
                ev.code == BTN_MIDDLE) {
                int bit = ev.code == BTN_LEFT ? 1 :
                          ev.code == BTN_MIDDLE ? 2 : 4;
                if (ev.value)
                    d->mask |= bit;
                else
                    d->mask &= ~bit;
                input_event e = {0};
                e.type = 0;
                e.mask = d->mask;
                e.x = d->x;
                e.y = d->y;
                deliver(d, &e);
            } else {
                uint32_t sym = linux_keysym(ev.code, shift, caps);
                if (sym) {
                    input_event e = {0};
                    e.type = 1;
                    e.keysym = sym;
                    e.keycode = ev.code;
                    e.down = ev.value ? 1 : 0;
                    e.caps = caps;
                    deliver(d, &e);
                }
            }
        } else if (ev.type == EV_REL) {
            if (ev.code == REL_X || ev.code == REL_Y) {
                if (ev.code == REL_X)
                    d->x += ev.value * CURSOR_SPEED;
                else
                    d->y += ev.value * CURSOR_SPEED;
                clamp_cursor(d);
                input_event e = {0};
                e.type = 0;
                e.mask = d->mask;
                e.x = d->x;
                e.y = d->y;
                deliver(d, &e);
            } else if (ev.code == REL_WHEEL) {
                input_event e = {0};
                e.type = 0;
                e.mask = d->mask;
                e.x = d->x;
                e.y = d->y;
                e.wheel = ev.value;
                deliver(d, &e);
            }
        }
    }
    return NULL;
}

static void device_group(const char *path, char *out, size_t cap)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out, cap, "%s", base);

    char *ev = strstr(out, "-event");
    if (ev)
        *ev = 0;

    /* strip a trailing "-ifNN" interface suffix */
    char *dash = strrchr(out, '-');
    if (dash && dash[1] == 'i' && dash[2] == 'f') {
        const char *p = dash + 3;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            p++;
            digits++;
        }
        if (digits > 0 && *p == 0)
            *dash = 0;
    }

    if (strncmp(out, "usb-", 4) == 0)
        memmove(out, out + 4, strlen(out + 4) + 1);
    for (char *p = out; *p; p++) {
        if (*p == '_')
            *p = ' ';
    }
}

static int primary_role(int kind)
{
    static const int order[] = {
        INPUT_KIND_KEYBOARD, INPUT_KIND_MOUSE, INPUT_KIND_TOUCHPAD,
        INPUT_KIND_TOUCHSCREEN, INPUT_KIND_TABLET, INPUT_KIND_GAMEPAD,
        INPUT_KIND_MOUSE_EXTRA, INPUT_KIND_MEDIA, INPUT_KIND_SYSTEM,
        INPUT_KIND_REMOTE, INPUT_KIND_OTHER
    };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++)
        if (kind & order[i])
            return order[i];
    return 0;
}

static void friendly_group(const char *name, const char *physkey,
                           char *out, size_t cap)
{
    snprintf(out, cap, "%s", name && *name ? name : physkey);
    static const char *suffix[] = {
        " Keyboard", " Mouse", " Consumer Control", " System Control",
        " Touchpad", " TouchPad", " Gamepad", " Joystick", " Remote"
    };
    for (size_t i = 0; i < sizeof(suffix) / sizeof(suffix[0]); i++) {
        size_t n = strlen(suffix[i]);
        size_t l = strlen(out);
        if (l > n && strcasecmp(out + l - n, suffix[i]) == 0) {
            out[l - n] = 0;
            break;
        }
    }
    size_t l = strlen(out);
    while (l > 0 && out[l - 1] == ' ')
        out[--l] = 0;
}

static int linux_open_device(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    unsigned long evbits = 0;
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0 ||
        !(evbits & ((1UL << EV_KEY) | (1UL << EV_REL) | (1UL << EV_ABS)))) {
        close(fd);
        return -1;
    }

    char nm[96] = "";
    ioctl(fd, EVIOCGNAME(sizeof(nm) - 1), nm);

    unsigned long keybits[(KEY_CNT + 8 * sizeof(unsigned long) - 1) /
                          (8 * sizeof(unsigned long))];
    memset(keybits, 0, sizeof(keybits));
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
    unsigned long relbits[(REL_CNT + 8 * sizeof(unsigned long) - 1) /
                          (8 * sizeof(unsigned long))];
    memset(relbits, 0, sizeof(relbits));
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits);
    unsigned long absbits[(ABS_CNT + 8 * sizeof(unsigned long) - 1) /
                          (8 * sizeof(unsigned long))];
    memset(absbits, 0, sizeof(absbits));
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);

#define BIT_TEST(bits, b) \
    (((bits)[(b) / (8 * sizeof(unsigned long))] >> \
      ((b) % (8 * sizeof(unsigned long)))) & 1)

    int name_consumer = strstr(nm, "Consumer Control") != NULL;
    int name_system = strstr(nm, "System Control") != NULL ||
                      strstr(nm, "Power Button") != NULL;
    int has_media =
        BIT_TEST(keybits, KEY_MUTE) || BIT_TEST(keybits, KEY_VOLUMEUP) ||
        BIT_TEST(keybits, KEY_VOLUMEDOWN) || BIT_TEST(keybits, KEY_PLAYPAUSE) ||
        BIT_TEST(keybits, KEY_PLAY) || BIT_TEST(keybits, KEY_PAUSE) ||
        BIT_TEST(keybits, KEY_STOPCD) || BIT_TEST(keybits, KEY_PREVIOUSSONG) ||
        BIT_TEST(keybits, KEY_NEXTSONG) || BIT_TEST(keybits, KEY_FASTFORWARD) ||
        BIT_TEST(keybits, KEY_REWIND) || BIT_TEST(keybits, KEY_MEDIA) ||
        BIT_TEST(keybits, KEY_HOMEPAGE);
    int has_system = BIT_TEST(keybits, KEY_POWER) ||
                     BIT_TEST(keybits, KEY_SLEEP) ||
                     BIT_TEST(keybits, KEY_WAKEUP) ||
                     BIT_TEST(keybits, KEY_SUSPEND);
    int has_kbd = !name_consumer && !name_system &&
                  (BIT_TEST(keybits, KEY_A) || BIT_TEST(keybits, KEY_Z) ||
                   BIT_TEST(keybits, KEY_ENTER) || BIT_TEST(keybits, KEY_SPACE));
    int has_mouse = BIT_TEST(keybits, BTN_LEFT) ||
                    (BIT_TEST(relbits, REL_X) && BIT_TEST(relbits, REL_Y));
    int has_touchpad = BIT_TEST(keybits, BTN_TOOL_FINGER) ||
                       strstr(nm, "Touchpad") || strstr(nm, "TouchPad");
    int has_touchscreen = BIT_TEST(keybits, BTN_TOUCH) &&
                          BIT_TEST(absbits, ABS_X) && BIT_TEST(absbits, ABS_Y) &&
                          !has_touchpad;
    int has_tablet = BIT_TEST(keybits, BTN_TOOL_PEN);
    int has_gamepad = BIT_TEST(keybits, BTN_SOUTH) ||
                      BIT_TEST(keybits, BTN_EAST) ||
                      BIT_TEST(keybits, BTN_NORTH) ||
                      BIT_TEST(keybits, BTN_WEST) ||
                      strstr(nm, "Gamepad") || strstr(nm, "Joystick");
    int has_remote = strstr(nm, "Remote") || strstr(nm, "remote") ||
                     strstr(nm, "Media Center");

    int kind = 0;
    if (has_kbd) kind |= INPUT_KIND_KEYBOARD;
    if (has_mouse) kind |= INPUT_KIND_MOUSE;
    if (has_touchpad) kind |= INPUT_KIND_TOUCHPAD;
    if (has_touchscreen) kind |= INPUT_KIND_TOUCHSCREEN;
    if (has_tablet) kind |= INPUT_KIND_TABLET;
    if (has_gamepad) kind |= INPUT_KIND_GAMEPAD;
    if (!(kind & (INPUT_KIND_KEYBOARD | INPUT_KIND_MOUSE |
                  INPUT_KIND_TOUCHPAD | INPUT_KIND_TOUCHSCREEN |
                  INPUT_KIND_TABLET | INPUT_KIND_GAMEPAD))) {
        if (has_media)
            kind |= INPUT_KIND_MEDIA;
        else if (has_system)
            kind |= INPUT_KIND_SYSTEM;
        else if (has_remote)
            kind |= INPUT_KIND_REMOTE;
    }
    /* The "keyboard" interface of a gaming mouse is its macro/media button
     * block, not a real keyboard. */
    if (kind && !has_mouse && strstr(nm, "GAMING MOUSE")) {
        kind = INPUT_KIND_MOUSE_EXTRA;
        if (has_media)
            kind |= INPUT_KIND_MEDIA;
    }
    /* Power/sleep-only nodes are not useful to a remote desktop and must not
     * be forwarded as keyboard input. */
    if (kind == 0 || (kind & INPUT_KIND_SYSTEM)) {
        close(fd);
        return -1;
    }

    in_dev *d = &g_dev[g_ndev];
    memset(d, 0, sizeof(*d));
    d->x = 32767;   /* relative devices start in the middle of the screen */
    d->y = 32767;
    snprintf(d->id, sizeof(d->id), "%s", path);
    snprintf(d->name, sizeof(d->name), "%s", nm[0] ? nm : path);

    char phys[128] = "";
    ioctl(fd, EVIOCGPHYS(sizeof(phys) - 1), phys);
    snprintf(d->physkey, sizeof(d->physkey), "%s", phys);
    char *ip = strstr(d->physkey, "/input");
    if (ip)
        *ip = 0;
    if (!d->physkey[0])
        device_group(path, d->physkey, sizeof(d->physkey));

    int role = primary_role(kind);
    for (int i = 0; i < g_ndev; i++) {
        if (strcmp(g_dev[i].physkey, d->physkey) == 0 &&
            primary_role(g_dev[i].kind) == role) {
            close(fd);
            return -1;   /* same physical interface already opened */
        }
    }

    if (strstr(path, "/by-id/"))
        device_group(path, d->group, sizeof(d->group));
    else
        friendly_group(nm, d->physkey, d->group, sizeof(d->group));

    d->fd = fd;
    d->ready = 1;
    d->kind = kind;
    d->x = d->y = 32767;
    d->alive = 1;
    if (os_thread_create(&d->thread, linux_reader, d) != 0) {
        d->alive = 0;
        close(fd);
        memset(d, 0, sizeof(*d));
        return -1;
    }
    g_ndev++;
    return 0;
}

static void linux_scan(void)
{
    DIR *dir = opendir("/dev/input/by-id");
    if (dir) {
        /* Prefer the specific "-event-kbd" / "-event-mouse" interfaces over
         * generic "-event-ifNN" nodes; the config binds the stable names. */
        for (int pass = 0; pass < 2; pass++) {
            rewinddir(dir);
            struct dirent *de;
            while ((de = readdir(dir)) && g_ndev < MAX_DEV) {
                if (!strstr(de->d_name, "-event-"))
                    continue;
                int specific = strstr(de->d_name, "-event-kbd") != NULL ||
                               strstr(de->d_name, "-event-mouse") != NULL;
                if ((pass == 0) != specific)
                    continue;
                char path[512];
                snprintf(path, sizeof(path), "/dev/input/by-id/%s", de->d_name);
                linux_open_device(path);
            }
        }
        closedir(dir);
    }

    if (g_ndev == 0) {
        DIR *ed = opendir("/dev/input");
        if (ed) {
            struct dirent *de;
            while ((de = readdir(ed)) && g_ndev < MAX_DEV) {
                if (strncmp(de->d_name, "event", 5) != 0 ||
                    de->d_name[5] < '0' || de->d_name[5] > '9')
                    continue;
                char path[512];
                snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
                linux_open_device(path);
            }
            closedir(ed);
        }
    } else {
        /* by-id symlinks are preferred for stable names, but some keyboards
         * have no symlink. Scan every event node as well; duplicate interfaces
         * are rejected by the phys-key + role check. Event numbers are not
         * contiguous (uinput can be event257), so list the directory. */
        DIR *ed = opendir("/dev/input");
        if (ed) {
            struct dirent *de;
            while ((de = readdir(ed)) && g_ndev < MAX_DEV) {
                if (strncmp(de->d_name, "event", 5) != 0 ||
                    de->d_name[5] < '0' || de->d_name[5] > '9')
                    continue;
                char path[512];
                snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
                linux_open_device(path);
            }
            closedir(ed);
        }
    }
}

int input_init(input_route_fn fn, void *ud)
{
    g_route = fn;
    g_ud = ud;
    g_run = 1;
    g_ndev = 0;
    memset(g_dev, 0, sizeof(g_dev));
    os_mutex_init(&g_lock);

    linux_scan();
    return 0;
}

void input_shutdown(void)
{
    g_run = 0;
    for (int i = 0; i < g_ndev; i++) {
        in_dev *d = &g_dev[i];
        d->alive = 0;
        os_thread_join(d->thread);
        if (d->fd > 0) {
            close(d->fd);
            d->fd = -1;
        }
    }
    os_mutex_destroy(&g_lock);
}

int input_count(void)
{
    int n;
    os_mutex_lock(&g_lock);
    n = g_ndev;
    os_mutex_unlock(&g_lock);
    return n;
}

int input_list(input_device *out, int max)
{
    int n = 0;
    os_mutex_lock(&g_lock);
    for (int i = 0; i < g_ndev && n < max; i++) {
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].id, sizeof(out[n].id), "%.*s",
                 (int)sizeof(out[n].id) - 1, g_dev[i].id);
        snprintf(out[n].name, sizeof(out[n].name), "%s", g_dev[i].name);
        snprintf(out[n].output, sizeof(out[n].output), "%s", g_dev[i].output);
        out[n].ready = g_dev[i].ready;
        out[n].kind = g_dev[i].kind;
        snprintf(out[n].group, sizeof(out[n].group), "%s", g_dev[i].group);
        out[n].events = g_dev[i].events;
        out[n].last_ms = g_dev[i].last_ms;
        n++;
    }
    os_mutex_unlock(&g_lock);
    return n;
}

int input_bind(const char *id, const char *output)
{
    int found = 0;
    os_mutex_lock(&g_lock);
    for (int i = 0; i < g_ndev; i++) {
        if (strcmp(g_dev[i].id, id) == 0) {
            snprintf(g_dev[i].output, sizeof(g_dev[i].output), "%s",
                     output ? output : "");
            found = 1;
            break;
        }
    }
    os_mutex_unlock(&g_lock);
    return found ? 0 : -1;
}
