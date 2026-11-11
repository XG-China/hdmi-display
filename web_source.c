/*
 * web_source.c - headless "web page as a display source".
 *
 * Linux pipeline:
 *   Xvfb (virtual X server, no desktop)  ->  Chromium --kiosk URL
 *        ->  FFmpeg x11grab  ->  normal video source  ->  DRM/KMS
 *
 * Xvfb is not a desktop environment: no window manager, no shell, no session.
 * It only provides a framebuffer for the browser.  Chromium runs with
 * --no-sandbox because the wall typically runs as root; only load trusted
 * URLs on such a kiosk.
 */
#define _GNU_SOURCE

#include "web_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

struct web_proc {
    pid_t xvfb_pid;
    pid_t chrome_pid;
    int   display;
    int   width, height;
    char  url[1024];

    Display *dpy;
    int   screen;
    int   mask;          /* XTest mouse button state (RFB mask) */
    int   caps_state;    /* Caps Lock state of this Xvfb screen */
    unsigned char keydown[KEY_MAX + 1];  /* for synthetic key repeat */
    int   focused;
    int   input_logged;
};

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int rm_entry(const char *path, const struct stat *st, int flag,
                    struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    remove(path);
    return 0;
}

/* Chromium keeps a sizeable profile under /tmp; remove it when the source
 * stops so repeated page changes do not leak disk space. */
static void remove_profile(int display)
{
    if (display < 0)
        return;
    char profile[64];
    snprintf(profile, sizeof(profile), "/tmp/hdmi-web-%d", display);
    nftw(profile, rm_entry, 64, FTW_DEPTH | FTW_PHYS);
}

static int have_cmd(const char *name)
{
    const char *path = getenv("PATH");
    char buf[1024];
    while (path && *path) {
        const char *colon = strchr(path, ':');
        size_t len = colon ? (size_t)(colon - path) : strlen(path);
        snprintf(buf, sizeof(buf), "%.*s/%s", (int)len, path, name);
        if (access(buf, X_OK) == 0)
            return 1;
        path = colon ? colon + 1 : NULL;
    }
    return 0;
}

static const char *find_chrome(void)
{
    static const char *names[] = {
        "chromium", "chromium-browser", "google-chrome",
        "google-chrome-stable", "microsoft-edge"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (have_cmd(names[i]))
            return names[i];
    return NULL;
}

static int wait_for_x(int display)
{
    char path[64];
    snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", display);
    for (int i = 0; i < 50; i++) {          /* up to 5 s */
        if (access(path, F_OK) == 0)
            return 0;
        sleep_ms(100);
    }
    return -1;
}

web_proc *web_proc_start(const char *url, int width, int height,
                         char *err, size_t errsz)
{
    if (!url || !*url) {
        snprintf(err, errsz, "empty web URL");
        return NULL;
    }
    if (!have_cmd("Xvfb")) {
        snprintf(err, errsz, "Xvfb not found: sudo apt install xvfb");
        return NULL;
    }
    const char *chrome = find_chrome();
    if (!chrome) {
        snprintf(err, errsz,
                 "Chromium not found: sudo apt install chromium "
                 "(or google-chrome)");
        return NULL;
    }

    if (width < 320) width = 1280;
    if (height < 240) height = 720;

    web_proc *w = calloc(1, sizeof(*w));
    if (!w) {
        snprintf(err, errsz, "out of memory");
        return NULL;
    }
    w->xvfb_pid = -1;
    w->chrome_pid = -1;

    if (strstr(url, "://"))
        snprintf(w->url, sizeof(w->url), "%s", url);
    else
        snprintf(w->url, sizeof(w->url), "http://%s", url);

    /* ---- Xvfb: let it pick a free display with -displayfd ---- */
    int fds[2];
    if (pipe(fds) != 0) {
        snprintf(err, errsz, "pipe failed: %s", strerror(errno));
        free(w);
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errsz, "fork failed: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        free(w);
        return NULL;
    }
    if (pid == 0) {
        if (setsid() < 0)
            _exit(127);
        close(fds[0]);
        if (dup2(fds[1], 3) < 0)
            _exit(127);
        close(fds[1]);

        char screen[64];
        snprintf(screen, sizeof(screen), "%dx%dx24", width, height);
        execlp("Xvfb", "Xvfb", "-displayfd", "3", "-screen", "0", screen,
               "-nolisten", "tcp", "-ac", (char *)NULL);
        _exit(127);
    }
    w->xvfb_pid = pid;
    close(fds[1]);

    int display = -1;
    struct pollfd pfd;
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 5000) > 0) {
        char buf[32] = {0};
        ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
        if (n > 0)
            display = atoi(buf);
    }
    close(fds[0]);

    if (display < 0 || wait_for_x(display) != 0) {
        snprintf(err, errsz, "Xvfb failed to start");
        if (w->xvfb_pid > 0) {
            kill(-w->xvfb_pid, SIGKILL);
            waitpid(w->xvfb_pid, NULL, 0);
        }
        free(w);
        return NULL;
    }
    w->display = display;

    /* ---- Chromium kiosk ---- */
    /* Drop a stale profile from an unclean previous run before reusing the
     * display number, so Chromium always starts with a clean profile. */
    remove_profile(display);
    pid = fork();
    if (pid < 0) {
        snprintf(err, errsz, "fork failed: %s", strerror(errno));
        kill(-w->xvfb_pid, SIGTERM);
        waitpid(w->xvfb_pid, NULL, 0);
        free(w);
        return NULL;
    }
    if (pid == 0) {
        if (setsid() < 0)
            _exit(127);

        char disp[16], profile[160], winsz[64], apparg[1100];
        snprintf(disp, sizeof(disp), ":%d", display);
        snprintf(profile, sizeof(profile), "--user-data-dir=/tmp/hdmi-web-%d", display);
        snprintf(winsz, sizeof(winsz), "--window-size=%d,%d", width, height);
        snprintf(apparg, sizeof(apparg), "--app=%s", w->url);

        setenv("DISPLAY", disp, 1);
        execlp(chrome, chrome,
               "--no-sandbox",
               profile,
               "--kiosk",
               "--disable-gpu",
               "--disable-dev-shm-usage",
               "--hide-scrollbars",
               "--no-first-run",
               "--no-default-browser-check",
               "--disable-infobars",
               "--autoplay-policy=no-user-gesture-required",
               "--force-device-scale-factor=1",
               winsz,
               apparg,
               NULL);
        _exit(127);
    }
    w->chrome_pid = pid;

    /* give Chromium a moment; if it exited immediately, report it */
    sleep_ms(500);
    int status = 0;
    if (waitpid(w->chrome_pid, &status, WNOHANG) == w->chrome_pid) {
        snprintf(err, errsz, "Chromium exited immediately (code %d)",
                 WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        kill(-w->xvfb_pid, SIGTERM);
        waitpid(w->xvfb_pid, NULL, 0);
        remove_profile(display);
        free(w);
        return NULL;
    }

    w->width = width;
    w->height = height;
    w->mask = 0;
    w->focused = 0;

    /* A second X connection drives XTEST input (the Xvfb screen is ours). */
    char dispname[32];
    snprintf(dispname, sizeof(dispname), ":%d", display);
    w->dpy = XOpenDisplay(dispname);
    if (w->dpy) {
        int ev_base = 0, err_base = 0, major = 0, minor = 0;
        if (!XTestQueryExtension(w->dpy, &ev_base, &err_base, &major, &minor)) {
            fprintf(stderr, "web: XTEST unavailable on :%d (input disabled)\n",
                    display);
            XCloseDisplay(w->dpy);
            w->dpy = NULL;
        } else {
            w->screen = DefaultScreen(w->dpy);
            XkbStateRec st;
            if (XkbGetState(w->dpy, XkbUseCoreKbd, &st) == Success)
                w->caps_state = (st.locked_mods & LockMask) ? 1 : 0;
        }
    } else {
        fprintf(stderr, "web: cannot open display :%d for input\n", display);
    }

    return w;
}

int web_proc_display(const web_proc *w)
{
    return w ? w->display : 0;
}

/* Chromium is the only client on this Xvfb screen.  Give its main window the
 * input focus once; a bare Xvfb has no window manager to do it for us. */
static void web_focus(web_proc *w)
{
    if (w->focused)
        return;
    Window root = DefaultRootWindow(w->dpy);
    Window rr, pr, *kids = NULL, best = None;
    unsigned int n = 0;
    long best_area = 0;
    if (XQueryTree(w->dpy, root, &rr, &pr, &kids, &n)) {
        for (unsigned int i = 0; i < n; i++) {
            XWindowAttributes a;
            if (!XGetWindowAttributes(w->dpy, kids[i], &a))
                continue;
            if (a.map_state != IsViewable || a.width < 2 || a.height < 2)
                continue;
            long area = (long)a.width * (long)a.height;
            if (area > best_area) {
                best_area = area;
                best = kids[i];
            }
        }
        if (kids)
            XFree(kids);
    }
    if (best != None) {
        XSetInputFocus(w->dpy, best, RevertToPointerRoot, CurrentTime);
        w->focused = 1;
    }
    XFlush(w->dpy);
}

/* Xvfb can start with Caps Lock on, and the XTEST key state is independent
 * from the physical keyboard. Keep the X server's locked Caps state equal to
 * the local keyboard state so case never gets inverted. */
static void web_set_caps(web_proc *w, int caps)
{
    caps = caps ? 1 : 0;
    if (!w->dpy || caps == w->caps_state)
        return;
    KeyCode kc = XKeysymToKeycode(w->dpy, XK_Caps_Lock);
    if (!kc)
        return;
    XTestFakeKeyEvent(w->dpy, kc, True, CurrentTime);
    XTestFakeKeyEvent(w->dpy, kc, False, CurrentTime);
    XFlush(w->dpy);
    w->caps_state = caps;
}

void web_proc_input(web_proc *w, const input_event *ev)
{
    if (!w || !w->dpy || !ev)
        return;
    web_focus(w);
    if (!w->input_logged) {
        fprintf(stderr, "web: XTEST input on :%d\n", w->display);
        w->input_logged = 1;
    }

    if (ev->type == 0) {
        int W = w->width > 0 ? w->width : 1920;
        int H = w->height > 0 ? w->height : 1080;
        int x = (int)((int64_t)ev->x * W / 65535);
        int y = (int)((int64_t)ev->y * H / 65535);
        if (x < 0) x = 0;
        if (x >= W) x = W - 1;
        if (y < 0) y = 0;
        if (y >= H) y = H - 1;
        XTestFakeMotionEvent(w->dpy, w->screen, x, y, CurrentTime);

        int changed = w->mask ^ ev->mask;
        for (int b = 0; b < 3; b++) {
            if (!(changed & (1 << b)))
                continue;
            int btn = b == 0 ? 1 : b == 1 ? 2 : 3;
            XTestFakeButtonEvent(w->dpy, btn,
                                 (ev->mask & (1 << b)) ? True : False,
                                 CurrentTime);
        }
        w->mask = ev->mask;

        int wheel = ev->wheel;
        if (wheel > 20) wheel = 20;
        if (wheel < -20) wheel = -20;
        if (wheel) {
            int btn = wheel > 0 ? 4 : 5;
            int n = wheel > 0 ? wheel : -wheel;
            for (int i = 0; i < n; i++) {
                XTestFakeButtonEvent(w->dpy, btn, True, CurrentTime);
                XTestFakeButtonEvent(w->dpy, btn, False, CurrentTime);
            }
        }
        XFlush(w->dpy);
    } else if (ev->type == 1) {
        if (ev->keycode == KEY_CAPSLOCK) {
            if (ev->down)
                web_set_caps(w, ev->caps);
            return;
        }
        web_set_caps(w, ev->caps);

        KeySym ks = (KeySym)ev->keysym;
        if (!ks)
            return;
        KeyCode kc = XKeysymToKeycode(w->dpy, ks);
        if (!kc)
            return;
        int idx = ev->keycode <= KEY_MAX ? ev->keycode : 0;
        int repeat = ev->down && idx && w->keydown[idx];
        /* XTEST does not auto-repeat a held synthetic key; a release+press
         * pair reproduces the normal repeat behaviour. */
        if (repeat)
            XTestFakeKeyEvent(w->dpy, kc, False, CurrentTime);
        XTestFakeKeyEvent(w->dpy, kc, ev->down ? True : False, CurrentTime);
        if (idx)
            w->keydown[idx] = ev->down ? 1 : 0;
        XFlush(w->dpy);
    }
}

static void kill_group(pid_t pid)
{
    if (pid <= 0)
        return;
    kill(-pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid)
            return;
        sleep_ms(50);
    }
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

void web_proc_stop(web_proc *w)
{
    if (!w)
        return;
    if (w->dpy) {
        XCloseDisplay(w->dpy);
        w->dpy = NULL;
    }
    kill_group(w->chrome_pid);
    kill_group(w->xvfb_pid);
    remove_profile(w->display);
    free(w);
}
