/*
 * main.c - multi-screen manager with runtime output assignment.
 *
 *   source (media / image / vnc / rdp / web / pip / ui / bars)
 *        -> XRGB canvas (one per output)
 *        -> DRM CRTC / plane
 *
 * Outputs are enumerated at startup and can be enabled, disabled and
 * reassigned from the built-in web UI without restarting the program.
 */
#define _GNU_SOURCE

#include "display.h"
#include "draw.h"
#include "video_dec.h"
#include "vnc_source.h"
#include "rdp_source.h"
#include "web_source.h"
#include "input_router.h"
#include "web_server.h"
#include "config.h"
#include "os_thread.h"
#include "alsa_out.h"

#include <dirent.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_SLOTS DISP_MAX_OUTPUTS

enum {
    KIND_BARS = 0,
    KIND_UI,
    KIND_MEDIA
};

typedef struct {
    int online;
    char codec[32];
    double in_fps;
    double out_fps;
    double pos;
    double dur;
    int src_w, src_h;
    char error[256];
} screen_stats;

typedef struct screen {
    int index;
    char name[64];      /* cardN-CONNECTOR (unique across GPUs) */
    char conn[64];      /* connector name on its card */
    disp_ctx *ctx;      /* DRM card this output belongs to */
    int cardno;         /* /dev/dri/cardN index */
    char audio_dev[64]; /* HDMI PCM on the same GPU */

    os_mutex_t lock;   /* connected/active/source/stats */
    int connected;
    int active;
    int width, height, refresh;
    int want_w, want_h;

    disp_output *out;

    char src_type[16];
    char src_arg[1024];
    char source[1024];   /* what the HUD shows */
    int dirty;
    int kind;
    int still;
    int hotplug_off;
    int user_off;       /* explicitly disabled from the web panel */
    int ever_started;   /* a source has been attached at least once */
    int hud_off;        /* raw pass-through: no LIVE VIEW overlay */
    double hud_until;   /* hide the video HUD after this time (0 = show) */
    double next_retry;  /* backoff for failed hotplug restores (DRM busy) */

    volatile int stop;
    os_thread_t thread;
    int thread_started;

    volatile double last_present;   /* watchdog: last successful flip */
    int wd_restarts;

    video_source *vs;    /* owned by the worker thread */
    vnc_source *vnc;     /* owned by the worker thread */
    rdp_source *rdp;     /* Microsoft RDP via FreeRDP, owned by the worker thread */
    video_source *vs2;   /* picture-in-picture overlay */
    vnc_source *vnc2;
    web_proc *web;
    web_proc *web2;
    int pip_on;
    int pip_x, pip_y, pip_w, pip_h;
    int input_layer;    /* 0 = main, 1 = picture-in-picture overlay */

    screen_stats st;
    char open_err[256];
    disp_info info;

    os_mutex_t preview_lock;
    uint8_t *preview;
    int pw, ph;
    uint64_t preview_serial;
} screen_t;

typedef struct {
    int index;
    int disable;
    char type[16];
    char arg[1024];
    int w, h;
} app_cmd;

typedef struct {
    disp_ctx *ctx[DISP_MAX_CARDS];
    int cardno[DISP_MAX_CARDS];
    int n_ctx;
    screen_t *slot[MAX_SLOTS];
    int n;

    char device[64];
    app_config cfg;
    int web_port;
    char web_bind[64];
    char web_token[128];
    int hwaccel;
    char audio_dev[64];   /* explicit device, empty = none */
    int  audio_auto;      /* pick the HDMI PCM of each output's GPU */
    double started;
    int cfg_dirty;

    os_mutex_t lock;
    app_cmd q[64];
    int qh, qt;
} app_t;

static void screen_set_audio(screen_t *s);

static app_t g_app;
static os_mutex_t g_input_lock;
static volatile sig_atomic_t g_run = 1;
static int g_fps = 30;
static int g_verbose;
static char g_backend[32] = "?";

static void on_signal(int sig)
{
    (void)sig;
    g_run = 0;
}

/* ------------------------------------------------------------- utilities */

static int classify(const char *type)
{
    if (strcmp(type, "ui") == 0 || strcmp(type, "dashboard") == 0)
        return KIND_UI;
    if (strcmp(type, "bars") == 0 || strcmp(type, "test") == 0)
        return KIND_BARS;
    return KIND_MEDIA;
}

static uint32_t accent_for(int idx)
{
    static const uint32_t c[4] = {
        RGB888(56, 189, 248), RGB888(251, 191, 36),
        RGB888(244, 114, 182), RGB888(74, 222, 128)
    };
    return c[idx & 3];
}

static void local_time(struct tm *tm)
{
    time_t t = time(NULL);
    localtime_r(&t, tm);
}

/* max_cells is a display width (ASCII 1, CJK 2), not a byte count. */
static void ellipsis(char *dst, size_t n, const char *src, size_t max_cells)
{
    if (!dst || n == 0)
        return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    if ((size_t)cv_utf8_cells(src) <= max_cells) {
        snprintf(dst, n, "%.*s", (int)(n - 1), src);
        return;
    }
    if (max_cells <= 3) {
        snprintf(dst, n, "...");
        return;
    }

    size_t want = max_cells - 3;
    size_t used = 0, o = 0;
    const char *p = src;
    while (*p) {
        const char *q = p;
        uint32_t cp = 0;
        int len = cv_utf8_next(&q, &cp);
        if (len <= 0)
            break;
        size_t cells = cp < 0x80 ? 1 : 2;
        if (used + cells > want || o + (size_t)len + 4 > n)
            break;
        memcpy(dst + o, p, (size_t)len);
        o += (size_t)len;
        used += cells;
        p = q;
    }
    memcpy(dst + o, "...", 4);
}

/* --------------------------------------------------------------- drawing */

static void draw_panel(canvas *cv, int x, int y, int w, int h,
                       const char *title, uint32_t accent, int sc)
{
    cv_rect_a(cv, x, y, w, h, RGB888(18, 24, 34), 235);
    cv_outline(cv, x, y, w, h, 1, RGB888(38, 50, 68));
    cv_rect(cv, x, y, 4 * sc, h, accent);
    cv_text(cv, x + 16 * sc, y + 12 * sc, title, accent, sc);
    cv_hline(cv, x + 16 * sc, y + 34 * sc, w - 28 * sc, 1, RGB888(38, 50, 68));
}

static void draw_spinner(canvas *cv, int cx, int cy, int r, double t, uint32_t col)
{
    for (int i = 0; i < 12; i++) {
        double a = i * M_PI / 6.0 + t * 2.5;
        double k = 0.25 + 0.75 * (0.5 + 0.5 * sin(t * 5.0 - i * 0.7));
        int x = cx + (int)(cos(a) * r);
        int y = cy + (int)(sin(a) * r);
        cv_rect_a(cv, x - 3, y - 3, 6, 6, col, (unsigned)(255 * k));
    }
}

static void draw_topbar(canvas *cv, screen_t *s, uint32_t accent, int sc,
                        const char *left, const char *right)
{
    int bar = 44 * sc;
    (void)s;
    cv_rect_a(cv, 0, 0, cv->w, bar, RGB888(0, 0, 0), 150);
    cv_rect(cv, 0, 0, 6 * sc, bar, accent);
    cv_text(cv, 18 * sc, 12 * sc, left, COL_WHITE, sc);
    if (right && *right)
        cv_text(cv, cv->w - 18 * sc - cv_text_w(right, sc), 12 * sc,
                right, COL_GREY, sc);
}

static void draw_testpattern(canvas *cv, screen_t *s, double t)
{
    static const uint32_t bars[8] = {
        RGB888(200, 200, 200), RGB888(200, 200, 0), RGB888(0, 200, 200), RGB888(0, 200, 0),
        RGB888(200, 0, 200), RGB888(200, 0, 0), RGB888(0, 0, 200), RGB888(16, 16, 16)
    };
    int W = cv->w, H = cv->h;
    uint32_t accent = accent_for(s->index);
    int sc = H >= 900 ? 2 : 1;
    int top = H * 3 / 4;

    for (int i = 0; i < 8; i++)
        cv_rect(cv, i * W / 8, 0, W / 8 + 1, top, bars[i]);

    for (int y = top; y < H; y++) {
        int step = (y - top) * 32 / (H - top);
        uint32_t v = (uint32_t)(step * 255 / 31);
        cv_rect(cv, 0, y, W, 1, (v << 16) | (v << 8) | v);
    }

    int x = (int)(fmod(t * 0.16, 1.0) * W);
    cv_rect(cv, x, 0, 3, H, RGB888(255, 255, 255));

    cv_rect(cv, W / 3, H / 4, W / 3, H / 4, RGB888(0, 0, 0));
    cv_outline(cv, W / 3, H / 4, W / 3, H / 4, 2, accent);

    char big[16];
    snprintf(big, sizeof(big), "%d", s->index + 1);
    cv_text_center(cv, W / 2, H / 4 + 10 * sc, big, accent, 8 * sc);

    char line[128];
    snprintf(line, sizeof(line), "%s  %dx%d@%d", s->name, s->width, s->height, s->refresh);
    cv_text_center(cv, W / 2, H / 4 + 140 * sc, line, COL_WHITE, sc);

    snprintf(line, sizeof(line), "HDMI %d / %s / INDEPENDENT OUTPUT", s->index + 1, g_backend);
    cv_text_center(cv, W / 2, H - 40 * sc, line, RGB888(20, 20, 20), sc);

    struct tm tm;
    char clk[32];
    local_time(&tm);
    strftime(clk, sizeof(clk), "%H:%M:%S", &tm);
    draw_topbar(cv, s, accent, sc, "TEST PATTERN", clk);
}

static void draw_dashboard(canvas *cv, screen_t *s, double t)
{
    int W = cv->w, H = cv->h;
    int sc = H >= 900 ? 2 : 1;
    int m = 32 * sc;
    uint32_t accent = accent_for(s->index);
    disp_sysinfo si;
    disp_get_sysinfo(&si);

    cv_fill(cv, RGB888(9, 12, 18));
    for (int y = 0; y < H; y += 48 * sc)
        cv_hline(cv, 0, y, W, 1, RGB888(14, 19, 27));

    char line[256];

    cv_rect(cv, 0, 0, W, 72 * sc, RGB888(13, 18, 27));
    cv_rect(cv, 0, 0, 6 * sc, 72 * sc, accent);
    snprintf(line, sizeof(line), "HDMI %d  ·  LOCAL UI", s->index + 1);
    cv_text(cv, m, 22 * sc, line, COL_WHITE, sc);
    snprintf(line, sizeof(line), "%s  %dx%d@%d", s->name, s->width, s->height, s->refresh);
    cv_text(cv, W - m - cv_text_w(line, 1), 28 * sc, line, COL_GREY, 1);

    struct tm tm;
    local_time(&tm);
    char clk[32], date[64];
    strftime(clk, sizeof(clk), "%H:%M:%S", &tm);
    strftime(date, sizeof(date), "%Y-%m-%d   %A", &tm);
    int cs = H / 150;
    if (cs < 4) cs = 4;
    if (cs > 10) cs = 10;
    cv_text_center(cv, W / 2, H * 14 / 100, clk, COL_WHITE, cs);
    cv_text_center(cv, W / 2, H * 14 / 100 + cv_text_h(cs) + 10 * sc, date, COL_GREY, sc);

    int py = H * 47 / 100;
    int pw = (W - m * 3) / 2;
    int ph = (H - py - m) * 9 / 10;
    int left_x = m;
    int right_x = m * 2 + pw;

    draw_panel(cv, left_x, py, pw, ph, "SYSTEM", accent, sc);
    int ty = py + 52 * sc;
    unsigned up = (unsigned)si.uptime_s;
    snprintf(line, sizeof(line), "HOST      %s", si.host[0] ? si.host : "-");
    cv_text(cv, left_x + 24 * sc, ty, line, COL_GREY, sc);
    snprintf(line, sizeof(line), "UPTIME    %ud %02u:%02u:%02u", up / 86400,
             (up % 86400) / 3600, (up % 3600) / 60, up % 60);
    cv_text(cv, left_x + 24 * sc, ty + 30 * sc, line, COL_WHITE, sc);
    snprintf(line, sizeof(line), "LOAD       %.2f", si.load1);
    cv_text(cv, left_x + 24 * sc, ty + 60 * sc, line, COL_GREY, sc);
    snprintf(line, sizeof(line), "MEMORY    %llu / %llu MB",
             si.mem_free_mb, si.mem_total_mb);
    cv_text(cv, left_x + 24 * sc, ty + 90 * sc, line, COL_GREY, sc);
    snprintf(line, sizeof(line), "OUTPUT    %s %dx%d", g_backend, s->width, s->height);
    cv_text(cv, left_x + 24 * sc, ty + 120 * sc, line, COL_GREY, sc);

    draw_panel(cv, right_x, py, pw, ph, "VOICE / AI", RGB888(74, 222, 128), sc);
    cv_text(cv, right_x + 24 * sc, ty, "VAD       Silero VAD (offline, plug-in)", COL_GREY, sc);
    cv_text(cv, right_x + 24 * sc, ty + 30 * sc, "ASR       SenseVoice (offline, plug-in)", COL_GREY, sc);
    cv_text(cv, right_x + 24 * sc, ty + 60 * sc, "TTS       Audio8 (offline, plug-in)", COL_GREY, sc);
    cv_text(cv, right_x + 24 * sc, ty + 90 * sc, "STATE     idle / no audio pipeline", COL_GREY, sc);
    cv_wave(cv, right_x + 24 * sc, py + ph - 76 * sc, pw - 48 * sc, 56 * sc,
            t, 0.75, RGB888(74, 222, 128));

    snprintf(line, sizeof(line), "WEB  http://%s:%d/   OUT %0.1f fps",
             si.host[0] ? si.host : "localhost", g_app.web_port,
             s->st.out_fps);
    cv_text(cv, m, H - m - 16, line, COL_DIM, 1);
}

static void draw_video_hud(canvas *cv, screen_t *s, const video_frame_info *fi,
                           double t)
{
    int W = cv->w, H = cv->h;
    int sc = H >= 900 ? 2 : 1;
    uint32_t accent = accent_for(s->index);
    char line[320];

    struct tm tm;
    char clk[32];
    local_time(&tm);
    strftime(clk, sizeof(clk), "%H:%M:%S", &tm);
    draw_topbar(cv, s, accent, sc, "LIVE VIEW", clk);

    if (fi->online) {
        int tx = 18 * sc + cv_text_w("LIVE VIEW", sc) + 18 * sc;
        int blink = ((int)(t * 2)) & 1;
        cv_rect(cv, tx, 16 * sc, 10 * sc, 10 * sc,
                blink ? COL_RED : RGB888(90, 30, 30));
        cv_text(cv, tx + 16 * sc, 11 * sc, "REC", COL_RED, sc);
    }

    int bh = 64 * sc;
    cv_rect_a(cv, 0, H - bh, W, bh, RGB888(0, 0, 0), 170);
    cv_rect(cv, 0, H - bh, 6 * sc, bh, accent);

    char src[256];
    ellipsis(src, sizeof(src), s->source, 90);
    cv_text(cv, 18 * sc, H - bh + 10 * sc, src, COL_GREY, 1);

    snprintf(line, sizeof(line), "%s  %dx%d  %.0f fps",
             fi->codec[0] ? fi->codec : "decoding", fi->src_w, fi->src_h,
             fi->stream_fps);
    cv_text(cv, 18 * sc, H - bh + 30 * sc, line, COL_WHITE, sc);

    if (fi->dur_s > 1.0) {
        double p = fi->pos_s / fi->dur_s;
        if (p < 0) p = 0;
        if (p > 1) p = 1;
        int bw = W - 36 * sc;
        cv_bar(cv, 18 * sc, H - 10 * sc, bw, 6 * sc, p, accent, RGB888(40, 46, 58));
        int mins = (int)(fi->pos_s / 60);
        int secs = (int)fi->pos_s % 60;
        snprintf(line, sizeof(line), "%02d:%02d", mins, secs);
        cv_text(cv, W - 18 * sc - cv_text_w(line, 1), H - bh + 10 * sc,
                line, COL_GREY, 1);
    }
}

static void draw_waiting(canvas *cv, screen_t *s, double t)
{
    int W = cv->w, H = cv->h;
    int sc = H >= 900 ? 2 : 1;
    uint32_t accent = accent_for(s->index);
    char src[256], err[256] = "";

    os_mutex_lock(&s->lock);
    snprintf(err, sizeof(err), "%s", s->st.error);
    os_mutex_unlock(&s->lock);

    draw_topbar(cv, s, accent, sc, "CONNECTING", "");

    /* Center the spinner + text block as one group (both axes). */
    int line_h = cv_text_h(sc);
    int gap1 = 26 * sc;
    int gap2 = 12 * sc;
    int group = 56 * sc + gap1 + line_h + gap2 + line_h;
    if (err[0])
        group += gap2 + line_h;
    int top = (H - group) / 2;
    if (top < 80 * sc)
        top = 80 * sc;

    draw_spinner(cv, W / 2, top + 28 * sc, 28 * sc, t, accent);

    int y = top + 56 * sc + gap1;
    ellipsis(src, sizeof(src), s->source, 56);
    cv_text_center(cv, W / 2, y, src, COL_WHITE, sc);
    y += line_h + gap2;
    cv_text_center(cv, W / 2, y,
                   "waiting for the first decoded frame ...", COL_DIM, 1);
    if (err[0]) {
        y += line_h + gap2;
        cv_text_center(cv, W / 2, y, err, COL_RED, 1);
    }

    char line[128];
    snprintf(line, sizeof(line), "%s   %dx%d", s->name, s->width, s->height);
    cv_text(cv, 18 * sc, H - 30 * sc, line, COL_DIM, 1);
}

/* --------------------------------------------------------------- preview */

static void preview_update(screen_t *s, canvas *cv)
{
    int pw = 640;
    int ph = (int)((double)pw * cv->h / cv->w);
    if (ph > 400) {
        ph = 400;
        pw = (int)((double)ph * cv->w / cv->h);
    }
    pw &= ~1;
    ph &= ~1;
    if (pw < 16 || ph < 16)
        return;

    os_mutex_lock(&s->preview_lock);
    if (!s->preview || s->pw != pw || s->ph != ph) {
        uint8_t *n = realloc(s->preview, (size_t)pw * (size_t)ph * 3);
        if (n) {
            s->preview = n;
            s->pw = pw;
            s->ph = ph;
        }
    }
    if (s->preview) {
        for (int y = 0; y < ph; y++) {
            int sy = y * cv->h / ph;
            uint32_t *row = (uint32_t *)(cv->px + (size_t)sy * cv->pitch);
            uint8_t *d = s->preview + (size_t)y * (size_t)pw * 3;
            for (int x = 0; x < pw; x++) {
                uint32_t p = row[x * cv->w / pw];
                *d++ = (uint8_t)((p >> 16) & 0xff);
                *d++ = (uint8_t)((p >> 8) & 0xff);
                *d++ = (uint8_t)(p & 0xff);
            }
        }
        s->preview_serial++;
    }
    os_mutex_unlock(&s->preview_lock);
}

/* ---------------------------------------------------------------- worker */

static void screen_publish_stats(screen_t *s, video_source *vs, vnc_source *vn,
                                 rdp_source *rd, int local_fps)
{
    char codec[32] = "", err[256] = "";
    double pos = 0, dur = 0, fps = 0;
    int online = 0;
    int set_error = 1;

    if (vs) {
        video_source_status(vs, codec, sizeof(codec), &pos, &dur, &fps,
                            &online, err, sizeof(err));
    } else if (vn) {
        vnc_source_status(vn, codec, sizeof(codec), &pos, &dur, &fps,
                          &online, err, sizeof(err));
        fps = 0;
    } else if (rd) {
        rdp_source_status(rd, codec, sizeof(codec), &fps, &online,
                          err, sizeof(err));
    } else {
        snprintf(codec, sizeof(codec), "local");
        fps = local_fps;
        online = 1;
        if (s->kind == KIND_MEDIA)
            set_error = 0;   /* keep the decoder's failure message */
    }

    os_mutex_lock(&s->lock);
    snprintf(s->st.codec, sizeof(s->st.codec), "%s", codec);
    if (set_error)
        snprintf(s->st.error, sizeof(s->st.error), "%s", err);
    s->st.pos = pos;
    s->st.dur = dur;
    s->st.in_fps = fps;
    s->st.online = online;
    os_mutex_unlock(&s->lock);
}

static void close_layer(video_source **vs, vnc_source **vn,
                        rdp_source **rdp, web_proc **wp)
{
    if (*vs) {
        video_source_close(*vs);
        *vs = NULL;
    }
    if (*vn) {
        vnc_source_close(*vn);
        *vn = NULL;
    }
    if (rdp && *rdp) {
        rdp_source_close(*rdp);
        *rdp = NULL;
    }
    if (wp && *wp) {
        web_proc_stop(*wp);
        *wp = NULL;
    }
}

/* x11grab can only capture a running X server. Check the local socket first
 * so the panel gets a clear message instead of FFmpeg's "Input/output error".
 * Remote displays (host:0) are assumed reachable and left to FFmpeg. */
static int check_x11_display(const char *disp, char *err, size_t errsz)
{
    const char *d = disp;
    if (!d || !*d)
        d = ":0";
    if (strncmp(d, "unix:", 5) == 0)
        d += 5;
    if (d[0] != ':')
        return 0;

    int num = atoi(d + 1);
    char path[64];
    snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", num);
    if (access(path, F_OK) != 0) {
        snprintf(err, errsz,
                 "未检测到 X 桌面（%s 不存在）。fnOS 没有本地桌面，"
                 "请改用网页/VNC/视频源；本机桌面源需带 X11 的 Linux。",
                 path);
        return -1;
    }
    return 0;
}

/* Accept "RDP:", "rdp://", "VNC:" and "vnc://" prefixes in address fields.
 * The prefix selects/confirms the protocol; the rest is handed to the source
 * implementation without the scheme. */
static int arg_has_proto(const char *arg, const char *proto)
{
    if (!arg || !proto)
        return 0;
    size_t n = strlen(proto);
    return strncasecmp(arg, proto, n) == 0 && arg[n] == ':';
}

static const char *strip_proto(const char *arg, const char *proto)
{
    if (!arg_has_proto(arg, proto))
        return arg;
    const char *p = arg + strlen(proto) + 1;
    if (p[0] == '/' && p[1] == '/')
        p += 2;                 /* rdp://host, vnc://host */
    return p;
}

/* Layer types are case-insensitive so "RDP:..." / "VNC:..." work in PiP
 * specifications and CLI arguments too. */
static const char *canon_layer_type(const char *type)
{
    static const char *known[] = {
        "media", "image", "screen", "rdp", "vnc", "web",
        "ui", "bars", "monitor", "remote"
    };
    if (!type)
        return type;
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (strcasecmp(type, known[i]) == 0)
            return known[i];
    return type;
}

static int open_layer(const char *type, const char *arg, int w, int h, int hw,
                      const char *audio_dev,
                      video_source **vs, vnc_source **vn, rdp_source **rd,
                      web_proc **wp,
                      char *err, size_t errsz)
{
    type = canon_layer_type(type);
    *vs = NULL;
    *vn = NULL;
    if (rd)
        *rd = NULL;
    if (wp)
        *wp = NULL;
    if (strcmp(type, "remote") == 0) {
        const char *a = arg;
        int is_rdp = 0;

        if (arg_has_proto(arg, "rdp")) {
            is_rdp = 1;
            a = strip_proto(arg, "rdp");
        } else if (arg_has_proto(arg, "vnc")) {
            a = strip_proto(arg, "vnc");
        } else {
            /* No keyword: "user:password@host" is RDP; a local socket path
             * or "host:port" is VNC. */
            is_rdp = arg && strchr(arg, '@') != NULL;
        }

        if (is_rdp) {
            if (!rd) {
                snprintf(err, errsz, "internal: rdp layer needs an rdp slot");
                return -1;
            }
            *rd = rdp_source_open(a, w, h, audio_dev, err, errsz);
            return *rd ? 0 : -1;
        }
        *vn = vnc_source_open(a, w, h, err, errsz);
        return *vn ? 0 : -1;
    }
    if (strcmp(type, "rdp") == 0) {
        if (!rd) {
            snprintf(err, errsz, "internal: rdp layer needs an rdp slot");
            return -1;
        }
        if (arg_has_proto(arg, "vnc")) {
            snprintf(err, errsz, "这是 VNC 地址，请以 VNC: 开头并选择“远程连接”");
            return -1;
        }
        *rd = rdp_source_open(strip_proto(arg, "rdp"), w, h, audio_dev,
                              err, errsz);
        return *rd ? 0 : -1;
    }
    if (strcmp(type, "vnc") == 0) {
        if (arg_has_proto(arg, "rdp")) {
            snprintf(err, errsz, "这是 RDP 地址，请以 RDP: 开头并选择“远程连接”");
            return -1;
        }
        *vn = vnc_source_open(strip_proto(arg, "vnc"), w, h, err, errsz);
        return *vn ? 0 : -1;
    }
    if (strcmp(type, "web") == 0) {
        if (!wp) {
            snprintf(err, errsz, "internal: web layer needs a web_proc slot");
            return -1;
        }
        *wp = web_proc_start(arg, w, h, err, errsz);
        if (!*wp)
            return -1;

        char url[64];
        snprintf(url, sizeof(url), ":%d.0+0,0", web_proc_display(*wp));

        video_src_opts opt;
        memset(&opt, 0, sizeof(opt));
        snprintf(opt.fmt, sizeof(opt.fmt), "x11grab");
        snprintf(opt.url, sizeof(opt.url), "%s", url);
        opt.screen_w = w;
        opt.screen_h = h;
        opt.loop = 1;

        *vs = video_source_open(&opt, err, errsz);
        if (!*vs) {
            web_proc_stop(*wp);
            *wp = NULL;
            return -1;
        }
        return 0;
    }
    if (strcmp(type, "screen") == 0) {
        /* "本机画面": mirror what the machine itself shows on its display.
         * A running X desktop is captured with x11grab; otherwise the kernel
         * console framebuffer (/dev/fb0) is captured, which is the fnOS
         * boot/login screen (desktop or text console, as-is).
         * Explicit forms: screen:/dev/fb1, screen:fb1, screen::1.0. */
        const char *a = (arg && arg[0]) ? arg : NULL;
        char fbpath[32] = "/dev/fb0";
        int want_x11 = 0;
        int explicit_fb = 0;

        if (a && strncmp(a, "/dev/fb", 7) == 0) {
            explicit_fb = 1;
            snprintf(fbpath, sizeof(fbpath), "%s", a);
        } else if (a && a[0] == 'f' && a[1] == 'b') {
            explicit_fb = 1;
            if (a[2] >= '0' && a[2] <= '9')
                snprintf(fbpath, sizeof(fbpath), "/dev/fb%s", a + 2);
        } else if (a && !strstr(a, "://") && (a[0] == ':' || strchr(a, ':'))) {
            want_x11 = 1;   /* ":N" or "host:N" */
        }
        /* Anything else (URLs, socket paths) is not a display: fall through
         * to auto instead of making x11grab/fbdev fail on it. */

        if (!want_x11 && !explicit_fb) {
            const char *d = getenv("DISPLAY");
            char probe[32];
            snprintf(probe, sizeof(probe), "%s", (d && *d) ? d : ":0");
            if (check_x11_display(probe, err, errsz) == 0)
                want_x11 = 1;
            else
                err[0] = 0;   /* auto: use the console framebuffer */
        }

        video_src_opts opt;
        memset(&opt, 0, sizeof(opt));
        if (want_x11) {
            const char *disp = a ? a : (getenv("DISPLAY") ? getenv("DISPLAY") : ":0.0");
            if (check_x11_display(disp, err, errsz) != 0)
                return -1;
            snprintf(opt.fmt, sizeof(opt.fmt), "x11grab");
            snprintf(opt.url, sizeof(opt.url), "%s", disp);
        } else {
            if (access(fbpath, R_OK) != 0) {
                snprintf(err, errsz,
                         "本机画面不可用：%s 不可读，且没有运行 X 桌面。"
                         "需要控制台帧缓冲（/dev/fb0）或一个 X11 桌面。",
                         fbpath);
                return -1;
            }
            snprintf(opt.fmt, sizeof(opt.fmt), "fbdev");
            snprintf(opt.url, sizeof(opt.url), "%s", fbpath);
        }
        /* No video_size: x11grab uses the whole root window and fbdev uses
         * the framebuffer's own size, so the picture is not cropped. */
        opt.loop = 1;
        opt.hw = hw;
        *vs = video_source_open(&opt, err, errsz);
        return *vs ? 0 : -1;
    }
    if (strcmp(type, "media") == 0 || strcmp(type, "monitor") == 0 ||
        strcmp(type, "image") == 0) {
        video_src_opts opt;
        memset(&opt, 0, sizeof(opt));
        snprintf(opt.url, sizeof(opt.url), "%s", arg);
        opt.screen_w = w;
        opt.screen_h = h;
        opt.still = (strcmp(type, "image") == 0);
        opt.loop = !opt.still;
        opt.hw = hw;
        snprintf(opt.audio_dev, sizeof(opt.audio_dev), "%s",
                 audio_dev ? audio_dev : "");
        *vs = video_source_open(&opt, err, errsz);
        return *vs ? 0 : -1;
    }
    snprintf(err, errsz, "layer type '%s' is not supported here", type);
    return -1;
}

static int split_layer(const char *s, char *type, size_t tn, char *arg, size_t an)
{
    const char *colon = strchr(s, ':');
    if (!colon || colon == s)
        return -1;
    size_t tl = (size_t)(colon - s);
    if (tl >= tn)
        tl = tn - 1;
    memcpy(type, s, tl);
    type[tl] = 0;
    snprintf(arg, an, "%s", colon + 1);
    return 0;
}

/* "mainType:mainArg|overlayType:overlayArg|x,y,w,h" */
static int parse_pip(const char *arg, char *mt, size_t mtn, char *ma, size_t man,
                     char *ot, size_t otn, char *oa, size_t oan,
                     int *x, int *y, int *w, int *h)
{
    char buf[1400];
    snprintf(buf, sizeof(buf), "%s", arg);

    char *p1 = strchr(buf, '|');
    if (!p1)
        return -1;
    *p1++ = 0;
    char *p2 = strchr(p1, '|');
    if (p2)
        *p2++ = 0;

    if (split_layer(buf, mt, mtn, ma, man) != 0)
        return -1;
    if (split_layer(p1, ot, otn, oa, oan) != 0)
        return -1;

    *x = *y = *w = *h = 0;
    if (p2)
        sscanf(p2, "%d,%d,%d,%d", x, y, w, h);
    return 0;
}

static void screen_sync(screen_t *s, app_t *a)
{
    char type[16], arg[1024];
    int dirty;

    os_mutex_lock(&s->lock);
    dirty = s->dirty;
    s->dirty = 0;
    snprintf(type, sizeof(type), "%s", s->src_type);
    snprintf(arg, sizeof(arg), "%s", s->src_arg);
    os_mutex_unlock(&s->lock);

    if (!dirty)
        return;

    s->hud_until = 0;   /* show the HUD again for the new source */
    s->input_layer = 0; /* keyboard starts on the main layer */

    os_mutex_lock(&g_input_lock);
    close_layer(&s->vs, &s->vnc, &s->rdp, &s->web);
    close_layer(&s->vs2, &s->vnc2, NULL, &s->web2);
    s->pip_on = 0;

    s->kind = classify(type);
    if (g_verbose)
        fprintf(stderr, "output %s: %s %s\n", s->name, type, arg);

    char err[256] = "";

    if (strcmp(type, "pip") == 0) {
        char mt[16], ma[1024], ot[16], oa[1024];
        int x = 0, y = 0, w = 0, h = 0;
        if (parse_pip(arg, mt, sizeof(mt), ma, sizeof(ma),
                      ot, sizeof(ot), oa, sizeof(oa), &x, &y, &w, &h) != 0) {
            snprintf(err, sizeof(err),
                     "bad pip spec: mainType:arg|overlayType:arg|x,y,w,h");
        } else {
            if (open_layer(mt, ma, s->width, s->height, a->hwaccel,
                           s->audio_dev,
                           &s->vs, &s->vnc, &s->rdp, &s->web, err, sizeof(err)) != 0) {
                /* main layer failed: error is shown */
            } else if (open_layer(ot, oa, s->width, s->height, a->hwaccel,
                                  s->audio_dev,
                                  &s->vs2, &s->vnc2, NULL, &s->web2, err, sizeof(err)) != 0) {
                /* keep the main layer, report overlay error */
            }

            if (w <= 8 || h <= 8) {
                w = (s->width / 3) & ~1;
                h = (w * 9 / 16) & ~1;
                x = s->width - w - 24;
                y = s->height - h - 72;
            }
            w &= ~1;
            h &= ~1;
            if (w > s->width) w = s->width & ~1;
            if (h > s->height) h = s->height & ~1;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x + w > s->width) x = s->width - w;
            if (y + h > s->height) y = s->height - h;
            s->pip_x = x;
            s->pip_y = y;
            s->pip_w = w;
            s->pip_h = h;
            s->pip_on = (s->vs2 || s->vnc2) ? 1 : 0;
            s->input_layer = 0;
            if (s->pip_on) {
                int main_ok = (s->rdp || s->vnc || s->web) ? 1 : 0;
                int ov_ok = (s->vnc2 || s->web2) ? 1 : 0;
                if (!main_ok && ov_ok)
                    s->input_layer = 1;
            }
        }
    } else if (strcmp(type, "ui") == 0 || strcmp(type, "bars") == 0) {
        /* drawn locally, no decoder */
    } else {
        if (open_layer(type, arg, s->width, s->height, a->hwaccel,
                       s->audio_dev,
                       &s->vs, &s->vnc, &s->rdp, &s->web, err, sizeof(err)) != 0) {
            fprintf(stderr, "output %s: %s\n", s->name, err);
        }
    }

    os_mutex_lock(&s->lock);
    snprintf(s->st.error, sizeof(s->st.error), "%s", err);
    s->st.online = (s->kind == KIND_MEDIA) ? 0 : 1;
    os_mutex_unlock(&s->lock);
    os_mutex_unlock(&g_input_lock);
}

static void draw_pip(canvas *cv, screen_t *s)
{
    int x = s->pip_x, y = s->pip_y, w = s->pip_w, h = s->pip_h;
    if (w < 8 || h < 8 || x < 0 || y < 0 || x + w > cv->w || y + h > cv->h)
        return;

    uint32_t accent = accent_for(s->index);
    cv_rect(cv, x - 2, y - 2, w + 4, h + 4, accent);
    cv_rect(cv, x, y, w, h, RGB888(0, 0, 0));

    uint8_t *dst = cv->px + (size_t)y * cv->pitch + (size_t)x * 4;
    video_frame_info fi;
    if (s->vs2)
        video_source_draw(s->vs2, dst, cv->pitch, w, h, &fi);
    else if (s->vnc2)
        vnc_source_draw(s->vnc2, dst, cv->pitch, w, h, &fi);

    cv_outline(cv, x - 2, y - 2, w + 4, h + 4, 2, accent);
}

static void *screen_thread(void *arg)
{
    screen_t *s = arg;
    disp_output *o = s->out;
    canvas cv;
    double start = disp_now();
    double next_ui = start;
    double fps_t0 = start, frames = 0;
    double preview_t = -1, stat_t = -1;
    uint64_t last_serial = 0;
    int kind = s->kind;
    int hud_off = s->hud_off;

    while (g_run && !s->stop) {
        screen_sync(s, &g_app);
        video_source *vs = s->vs;
        vnc_source *vn = s->vnc;
        rdp_source *rd = s->rdp;
        int has_src = (vs != NULL || vn != NULL || rd != NULL);
        double now = disp_now();
        double t = now - start;

        os_mutex_lock(&s->lock);
        kind = s->kind;
        hud_off = s->hud_off;
        os_mutex_unlock(&s->lock);

        if (has_src) {
            uint64_t serial = vs ? video_source_serial(vs)
                             : vn ? vnc_source_serial(vn)
                                  : rdp_source_serial(rd);
            if (s->pip_on) {
                uint64_t s2 = 0;
                if (s->vs2)
                    s2 = video_source_serial(s->vs2);
                else if (s->vnc2)
                    s2 = vnc_source_serial(s->vnc2);
                serial = serial * 1000003ull + s2;
            }
            uint64_t oser = 0;
            if (s->vs2)
                oser = video_source_serial(s->vs2);
            else if (s->vnc2)
                oser = vnc_source_serial(s->vnc2);
            if (serial && oser)
                serial ^= (oser << 32) | (oser >> 32);
            if (serial == 0) {
                if (now >= next_ui) {
                    cv_init(&cv, disp_backbuffer(o), disp_pitch(o),
                            s->width, s->height);
                    cv_fill(&cv, RGB888(6, 8, 12));
                    draw_waiting(&cv, s, t);
                    disp_present(o);
                    s->last_present = disp_now();
                    frames++;
                    if (now - preview_t >= 1.0) {
                        preview_update(s, &cv);
                        preview_t = now;
                    }
                    next_ui = now + 0.2;
                }
                if (now - stat_t >= 0.5) {
                    screen_publish_stats(s, vs, vn, rd, g_fps);
                    stat_t = now;
                }
                disp_sleep_ms(40);
                continue;
            }
            if (serial == last_serial) {
                /* Static image: refresh 4x/s for a live preview. Idle video,
                 * VNC, web and paused playback: present once a second so the
                 * HUD can expire and the keep-alive watchdog sees a live
                 * output instead of needlessly reopening the source. */
                double delay = s->still ? 0.25 : 1.0;
                if (now < next_ui) {
                    disp_sleep_ms(4);
                    if (now - stat_t >= 0.5) {
                        screen_publish_stats(s, vs, vn, rd, g_fps);
                        stat_t = now;
                    }
                    continue;
                }
                next_ui = now + delay;
                if (!s->still && s->hud_until > now &&
                    s->hud_until < next_ui)
                    next_ui = s->hud_until;   /* hide the HUD on time */
            } else {
                last_serial = serial;
            }
        } else {
            if (now < next_ui) {
                disp_sleep_ms(1);
                continue;
            }
            next_ui += 1.0 / g_fps;
            if (next_ui < now)
                next_ui = now + 1.0 / g_fps;
        }

        cv_init(&cv, disp_backbuffer(o), disp_pitch(o), s->width, s->height);
        cv_fill(&cv, RGB888(6, 8, 12));

        video_frame_info fi;
        memset(&fi, 0, sizeof(fi));
        int have_video = 0;

        if (vs)
            have_video = video_source_draw(vs, cv.px, cv.pitch, s->width,
                                           s->height, &fi) > 0;
        else if (vn)
            have_video = vnc_source_draw(vn, cv.px, cv.pitch,
                                         s->width, s->height, &fi) > 0;
        else if (rd)
            have_video = rdp_source_draw(rd, cv.px, cv.pitch,
                                         s->width, s->height, &fi) > 0;

        if (has_src) {
            if (have_video) {
                os_mutex_lock(&s->lock);
                s->st.src_w = fi.src_w;
                s->st.src_h = fi.src_h;
                os_mutex_unlock(&s->lock);
                if (s->hud_until <= 0)
                    s->hud_until = now + 4.0;   /* brief info, then clean */
                if (s->pip_on)
                    draw_pip(&cv, s);
                if (!hud_off && now < s->hud_until)
                    draw_video_hud(&cv, s, &fi, t);
            } else {
                draw_waiting(&cv, s, t);
            }
        } else if (kind == KIND_UI) {
            draw_dashboard(&cv, s, t);
        } else if (kind == KIND_MEDIA) {
            draw_waiting(&cv, s, t);
        } else {
            draw_testpattern(&cv, s, t);
        }

        disp_present(o);
        s->last_present = disp_now();
        frames++;

        if (now - fps_t0 >= 1.0) {
            double f = frames / (now - fps_t0);
            os_mutex_lock(&s->lock);
            s->st.out_fps = f;
            s->st.src_w = fi.src_w;
            s->st.src_h = fi.src_h;
            os_mutex_unlock(&s->lock);
            frames = 0;
            fps_t0 = now;
        }
        if (now - preview_t >= 1.0) {
            preview_update(s, &cv);
            preview_t = now;
        }
        if (now - stat_t >= 0.5) {
            screen_publish_stats(s, vs, vn, rd, g_fps);
            stat_t = now;
        }
    }

    os_mutex_lock(&g_input_lock);
    close_layer(&s->vs, &s->vnc, &s->rdp, &s->web);
    close_layer(&s->vs2, &s->vnc2, NULL, &s->web2);
    os_mutex_unlock(&g_input_lock);
    return NULL;
}

/* --------------------------------------------------------- output control */

static screen_t *find_slot(app_t *a, int index)
{
    os_mutex_lock(&a->lock);
    screen_t *s = (index >= 0 && index < a->n) ? a->slot[index] : NULL;
    os_mutex_unlock(&a->lock);
    return s;
}

static screen_t *find_slot_by_name(app_t *a, const char *name)
{
    screen_t *found = NULL;
    int matches = 0;
    os_mutex_lock(&a->lock);
    for (int i = 0; i < a->n; i++) {
        if (strcmp(a->slot[i]->name, name) == 0) {
            found = a->slot[i];
            matches = 1;
            break;
        }
        /* legacy config/CLI: bare connector name, only if unique */
        if (strcmp(a->slot[i]->conn, name) == 0) {
            found = a->slot[i];
            matches++;
        }
    }
    if (matches > 1)
        found = NULL;
    os_mutex_unlock(&a->lock);
    return found;
}

/* Map a point from a screen rectangle into a VNC framebuffer, using the same
 * letterbox fit as vnc_source_draw().  Returns -1 outside the image. */
static int vnc_map_point(vnc_source *v, int scr_w, int scr_h,
                         int sx, int sy, int *vx, int *vy)
{
    int fw = 0, fh = 0;
    vnc_source_size(v, &fw, &fh);
    if (fw <= 0 || fh <= 0 || scr_w <= 0 || scr_h <= 0)
        return -1;
    double dar = (double)fw / (double)fh;
    int w = scr_w;
    int h = (int)lrint((double)scr_w / dar);
    if (h > scr_h) {
        h = scr_h;
        w = (int)lrint((double)scr_h * dar);
    }
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    w &= ~1;
    h &= ~1;
    int x = (scr_w - w) / 2;
    int y = (scr_h - h) / 2;
    if (sx < x || sx >= x + w || sy < y || sy >= y + h)
        return -1;
    *vx = (sx - x) * fw / w;
    *vy = (sy - y) * fh / h;
    return 0;
}

static int route_input(const char *output, const input_event *ev, void *ud)
{
    app_t *a = ud;
    int rc = -1;

    os_mutex_lock(&g_input_lock);
    screen_t *s = find_slot_by_name(a, output);
    if (!s) {
        os_mutex_unlock(&g_input_lock);
        return -1;
    }

    os_mutex_lock(&s->lock);
    int active = s->active;
    int W = s->width, H = s->height;
    int pip = s->pip_on;
    int px0 = s->pip_x, py0 = s->pip_y, pw = s->pip_w, ph = s->pip_h;
    os_mutex_unlock(&s->lock);

    if (active) {
        vnc_source *mvn = s->vnc;
        rdp_source *mrd = s->rdp;
        web_proc *mwp = s->web;
        vnc_source *ovn = pip ? s->vnc2 : NULL;
        web_proc *owp = pip ? s->web2 : NULL;

        int sx = 0, sy = 0;
        if (W > 0 && H > 0) {
            sx = (int)((int64_t)ev->x * W / 65535);
            sy = (int)((int64_t)ev->y * H / 65535);
        }
        int inside = 0;
        if (pip && pw > 0 && ph > 0 && W > 0 && H > 0)
            inside = sx >= px0 && sx < px0 + pw &&
                     sy >= py0 && sy < py0 + ph;

        if (ev->type == 0) {
            if (inside) {
                int lx = sx - px0;
                int ly = sy - py0;
                if (ovn) {
                    int vx = 0, vy = 0;
                    if (vnc_map_point(ovn, pw, ph, lx, ly, &vx, &vy) == 0) {
                        vnc_source_pointer(ovn, ev->mask, vx, vy);
                        if (ev->wheel)
                            vnc_source_wheel(ovn, ev->wheel, vx, vy);
                    }
                    s->input_layer = 1;   /* keyboard follows the PiP */
                    rc = 0;
                } else if (owp) {
                    if (W > 0 && H > 0) {
                        input_event e = *ev;
                        double dar = (double)W / (double)H;
                        int vw = pw;
                        int vh = (int)lrint((double)pw / dar);
                        if (vh > ph) {
                            vh = ph;
                            vw = (int)lrint((double)ph * dar);
                        }
                        if (vw < 2) vw = 2;
                        if (vh < 2) vh = 2;
                        vw &= ~1;
                        vh &= ~1;
                        int ox = (pw - vw) / 2;
                        int oy = (ph - vh) / 2;
                        if (lx >= ox && lx < ox + vw &&
                            ly >= oy && ly < oy + vh) {
                            int wx = (lx - ox) * W / vw;
                            int wy = (ly - oy) * H / vh;
                            e.x = (int)((int64_t)wx * 65535 / W);
                            e.y = (int)((int64_t)wy * 65535 / H);
                            if (e.x < 0) e.x = 0;
                            if (e.x > 65535) e.x = 65535;
                            if (e.y < 0) e.y = 0;
                            if (e.y > 65535) e.y = 65535;
                            web_proc_input(owp, &e);
                        }
                    }
                    s->input_layer = 1;
                    rc = 0;
                } else {
                    /* Image/media overlay: consume the click, never fall
                     * through to the main screen behind the small window. */
                    rc = 0;
                }
            } else if (mrd) {
                if (ev->wheel)
                    rdp_source_wheel(mrd, ev->wheel);
                rdp_source_pointer(mrd, ev->mask, ev->x, ev->y);
                s->input_layer = 0;
                rc = 0;
            } else if (mvn) {
                int vx = 0, vy = 0;
                if (vnc_map_point(mvn, W, H, sx, sy, &vx, &vy) == 0) {
                    vnc_source_pointer(mvn, ev->mask, vx, vy);
                    if (ev->wheel)
                        vnc_source_wheel(mvn, ev->wheel, vx, vy);
                    s->input_layer = 0;
                    rc = 0;
                }
            } else if (mwp) {
                web_proc_input(mwp, ev);
                s->input_layer = 0;
                rc = 0;
            }
        } else if (ev->type == 1) {
            /* Keyboard goes to the layer the pointer last entered. */
            if (s->input_layer == 1 && pip) {
                if (ovn) {
                    vnc_source_key(ovn, ev->keysym, ev->down);
                    rc = 0;
                } else if (owp) {
                    web_proc_input(owp, ev);
                    rc = 0;
                } else {
                    s->input_layer = 0;
                }
            }
            if (rc != 0) {
                if (mrd) {
                    rdp_source_keycode(mrd, ev->keycode, ev->down);
                    rc = 0;
                } else if (mvn) {
                    vnc_source_key(mvn, ev->keysym, ev->down);
                    rc = 0;
                } else if (mwp) {
                    web_proc_input(mwp, ev);
                    rc = 0;
                }
            }
        }
    }

    os_mutex_unlock(&g_input_lock);
    return rc;
}

static void screen_close_output(app_t *a, screen_t *s)
{
    if (!s->active)
        return;
    s->stop = 1;
    if (s->thread_started) {
        os_thread_join(s->thread);
        s->thread_started = 0;
    }
    s->stop = 0;
    disp_close(s->out);
    s->out = NULL;

    os_mutex_lock(&s->lock);
    s->active = 0;
    s->width = 0;
    s->height = 0;
    s->st.online = 0;
    s->st.in_fps = 0;
    s->st.out_fps = 0;
    s->st.pos = 0;
    s->st.dur = 0;
    s->st.src_w = 0;
    s->st.src_h = 0;
    s->st.codec[0] = 0;
    os_mutex_unlock(&s->lock);

    os_mutex_lock(&s->preview_lock);
    free(s->preview);
    s->preview = NULL;
    s->pw = s->ph = 0;
    os_mutex_unlock(&s->preview_lock);
    (void)a;
}

static void fb_native_size(int *w, int *h)
{
    *w = *h = 0;
    FILE *f = fopen("/sys/class/graphics/fb0/virtual_size", "r");
    if (!f)
        return;
    if (fscanf(f, "%d,%d", w, h) != 2)
        *w = *h = 0;
    fclose(f);
}

/* "Auto" mode: never downscale the source. The console mirrors /dev/fb0 1:1,
 * media uses the stream's own size, everything else runs at 1080p. */
static void pick_output_mode(screen_t *s, const char *type, const char *arg,
                             int *ow, int *oh)
{
    int sw = 0, sh = 0;
    if (strcmp(type, "screen") == 0)
        fb_native_size(&sw, &sh);
    else if (strcmp(type, "media") == 0)
        video_probe_size(arg, &sw, &sh);

    int n = s->info.mode_count;
    if (n <= 0)
        return;

    int best = -1;
    if (sw > 0 && sh > 0) {
        long best_score = -1;
        for (int i = 0; i < n; i++) {
            long mw = s->info.modes[i].width;
            long mh = s->info.modes[i].height;
            long dw = mw - sw, dh = mh - sh;
            long score;
            if (dw >= 0 && dh >= 0)
                score = dw + dh;              /* smallest mode that covers */
            else
                score = 1000000 - dw - dh;    /* only if nothing covers */
            if (best_score < 0 || score < best_score) {
                best_score = score;
                best = i;
            }
        }
    } else {
        for (int i = 0; i < n; i++)
            if (s->info.modes[i].width == 1920 &&
                s->info.modes[i].height == 1080) {
                best = i;
                break;
            }
        if (best < 0) {
            long best_score = -1;
            for (int i = 0; i < n; i++) {
                long mh = s->info.modes[i].height;
                long score = mh >= 720 ? mh - 720 : 1000000 - mh;
                if (best_score < 0 || score < best_score) {
                    best_score = score;
                    best = i;
                }
            }
        }
    }
    if (best >= 0) {
        *ow = s->info.modes[best].width;
        *oh = s->info.modes[best].height;
    }
}

static int apply_output(app_t *a, int index, const char *type, const char *arg,
                        int w, int h, int persist, char *err, size_t errsz)
{
    screen_t *s = find_slot(a, index);
    if (!s) {
        snprintf(err, errsz, "no such output");
        return -1;
    }

    os_mutex_lock(&s->lock);
    int connected = s->connected;
    int active = s->active;
    int old_w = s->want_w;
    int old_h = s->want_h;
    os_mutex_unlock(&s->lock);

    if (!connected) {
        snprintf(err, errsz, "%s is not connected", s->name);
        return -1;
    }

    /* Types that take no argument must not inherit whatever was left in the
     * panel's text box (a stale URL used to break "local screen"). */
    if (strcmp(type, "ui") == 0 || strcmp(type, "bars") == 0)
        arg = "";
    else if (strcmp(type, "screen") == 0 && arg && strstr(arg, "://"))
        arg = "";
    else if (strcmp(type, "remote") == 0 && (!arg || !*arg)) {
        snprintf(err, errsz, "请输入远程地址：用户:密码@主机 或 VNC 主机:端口");
        return -1;
    } else if (strcmp(type, "rdp") == 0 &&
               (!arg || !*arg ||
                !strchr(strip_proto(arg, "rdp"), '@') ||
                !strchr(strip_proto(arg, "rdp"), ':'))) {
        snprintf(err, errsz,
                 "RDP 需要“RDP:用户:密码@主机[:端口]”，例如 "
                 "RDP:Administrator:密码@10.0.0.5:3389");
        return -1;
    }

    /* 0x0 means "auto": pick a mode that does not downscale the source.
     * Persist the requested 0x0 so the next start probes again. */
    int req_w = w, req_h = h;
    if (w <= 0 || h <= 0) {
        int pw = 0, ph = 0;
        pick_output_mode(s, type, arg, &pw, &ph);
        if (pw > 0 && ph > 0) {
            w = pw;
            h = ph;
        }
    }

    int need_open = !active || w != old_w || h != old_h;
    if (need_open) {
        screen_close_output(a, s);

        disp_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.width = w;
        spec.height = h;

        char e[256] = "";
        disp_output *o = disp_open(s->ctx, s->conn, &spec, e, sizeof(e));
        if (!o) {
            os_mutex_lock(&s->lock);
            snprintf(s->open_err, sizeof(s->open_err), "%s", e);
            os_mutex_unlock(&s->lock);
            snprintf(err, errsz, "%s", e);
            return -1;
        }

        os_mutex_lock(&s->lock);
        s->out = o;
        s->active = 1;
        s->width = disp_width(o);
        s->height = disp_height(o);
        s->refresh = disp_refresh_hz(o);
        s->want_w = w;
        s->want_h = h;
        s->open_err[0] = 0;
        s->stop = 0;
        s->dirty = 1;
        os_mutex_unlock(&s->lock);
    }

    os_mutex_lock(&s->lock);
    snprintf(s->src_type, sizeof(s->src_type), "%s", type);
    snprintf(s->src_arg, sizeof(s->src_arg), "%s", arg ? arg : "");
    snprintf(s->source, sizeof(s->source), "%s",
             (arg && *arg) ? arg : type);
    s->still = (strcmp(type, "image") == 0);
    s->hud_off = (strcmp(type, "screen") == 0 ||
                  strcmp(type, "remote") == 0 ||
                  strcmp(type, "rdp") == 0 ||
                  strcmp(type, "vnc") == 0 ||
                  strcmp(type, "web") == 0);   /* mirror, not a preview */
    s->dirty = 1;
    os_mutex_unlock(&s->lock);

    if (!s->thread_started) {
        if (os_thread_create(&s->thread, screen_thread, s) != 0) {
            snprintf(err, errsz, "cannot start display thread");
            return -1;
        }
        s->thread_started = 1;
    }

    os_mutex_lock(&s->lock);
    s->ever_started = 1;
    s->user_off = 0;
    s->hotplug_off = 0;
    os_mutex_unlock(&s->lock);

    if (persist) {
        os_mutex_lock(&a->lock);
        config_set(&a->cfg, s->name, type, arg ? arg : "", req_w, req_h);
        a->cfg_dirty = 1;
        os_mutex_unlock(&a->lock);
    }
    return 0;
}

static int disable_output(app_t *a, int index, int persist)
{
    screen_t *s = find_slot(a, index);
    if (!s)
        return -1;

    screen_close_output(a, s);
    os_mutex_lock(&s->lock);
    s->user_off = 1;        /* keep the panel's "stop" from being undone by hotplug */
    s->hotplug_off = 0;
    os_mutex_unlock(&s->lock);
    if (persist) {
        os_mutex_lock(&a->lock);
        config_remove(&a->cfg, s->name);
        a->cfg_dirty = 1;
        os_mutex_unlock(&a->lock);
    }
    return 0;
}

/* Re-enumerate connectors on every DRM card and keep the slot table in sync. */
static void poll_outputs(app_t *a)
{
    int any = 0;
    os_mutex_lock(&a->lock);
    for (int ci = 0; ci < a->n_ctx; ci++) {
        disp_info infos[DISP_MAX_OUTPUTS];
        int n = disp_enumerate(a->ctx[ci], infos, DISP_MAX_OUTPUTS);
        if (n <= 0)
            continue;   /* transient DRM error: keep the current state */
        any = 1;

        for (int i = 0; i < n; i++) {
            char full[64];
            snprintf(full, sizeof(full), "card%d-%.48s", a->cardno[ci],
                     infos[i].name);
            screen_t *s = NULL;
            for (int k = 0; k < a->n; k++)
                if (strcmp(a->slot[k]->name, full) == 0) {
                    s = a->slot[k];
                    break;
                }
            if (!s && a->n < MAX_SLOTS) {
                s = calloc(1, sizeof(*s));
                if (!s)
                    continue;
                s->index = a->n;
                s->ctx = a->ctx[ci];
                s->cardno = a->cardno[ci];
                snprintf(s->conn, sizeof(s->conn), "%.63s", infos[i].name);
                snprintf(s->name, sizeof(s->name), "%s", full);
                s->connected = infos[i].connected;
                s->info = infos[i];
                s->width = infos[i].width;
                s->height = infos[i].height;
                s->refresh = infos[i].refresh;
                os_mutex_init(&s->lock);
                os_mutex_init(&s->preview_lock);
                snprintf(s->src_type, sizeof(s->src_type), "bars");
                s->want_w = 0;
                s->want_h = 0;
                screen_set_audio(s);
                a->slot[a->n++] = s;
            }
            if (!s)
                continue;
            os_mutex_lock(&s->lock);
            int was_connected = s->connected;
            s->connected = infos[i].connected;
            s->info = infos[i];
            if (!s->active) {
                s->width = infos[i].width;
                s->height = infos[i].height;
                s->refresh = infos[i].refresh;
            }
            os_mutex_unlock(&s->lock);
            if (was_connected != infos[i].connected)
                fprintf(stderr, "output %s: %s\n", s->name,
                        infos[i].connected ? "connected" : "disconnected");
        }

        /* connectors on this card that disappeared */
        for (int k = 0; k < a->n; k++) {
            screen_t *s = a->slot[k];
            if (s->ctx != a->ctx[ci])
                continue;
            int found = 0;
            for (int i = 0; i < n; i++)
                if (strcmp(infos[i].name, s->conn) == 0) {
                    found = 1;
                    break;
                }
            if (!found) {
                os_mutex_lock(&s->lock);
                s->connected = 0;
                os_mutex_unlock(&s->lock);
            }
        }
    }
    os_mutex_unlock(&a->lock);

    if (!any)
        return;

    /* auto close disconnected outputs, auto reopen restored ones */
    for (int k = 0; k < a->n; k++) {
        screen_t *s = a->slot[k];
        os_mutex_lock(&s->lock);
        int connected = s->connected;
        int active = s->active;
        int hot = s->hotplug_off;
        int user_off = s->user_off;
        int ever = s->ever_started;
        os_mutex_unlock(&s->lock);

        if (!connected && active) {
            screen_close_output(a, s);
            os_mutex_lock(&s->lock);
            s->hotplug_off = 1;
            os_mutex_unlock(&s->lock);
            fprintf(stderr, "output %s: source stopped (cable unplugged)\n", s->name);
        } else if (connected && !active && !user_off && (hot || !ever)) {
            double now = disp_now();
            if (now < s->next_retry)
                continue;
            char type[16], arg[1024];
            int w, h;
            os_mutex_lock(&s->lock);
            snprintf(type, sizeof(type), "%s", s->src_type);
            snprintf(arg, sizeof(arg), "%s", s->src_arg);
            w = s->want_w;
            h = s->want_h;
            os_mutex_unlock(&s->lock);
            char err[256];
            if (apply_output(a, s->index, type, arg, w, h, 0, err, sizeof(err)) == 0) {
                os_mutex_lock(&s->lock);
                s->hotplug_off = 0;
                s->next_retry = 0;
                os_mutex_unlock(&s->lock);
                fprintf(stderr, "output %s: source restored (%s)\n", s->name, type);
            } else {
                /* DRM can stay busy for a while (compositor/X server). Retry,
                 * but do not hammer the log or the GPU every second. */
                s->next_retry = now + 5.0;
                fprintf(stderr, "output %s: restore failed: %s\n", s->name, err);
            }
        }
    }
}

/* ------------------------------------------------------------------- web */

static void jesc(char *dst, size_t cap, const char *s);
static void jappend(char *buf, size_t cap, size_t *off, const char *fmt, ...);

/* Sources that consume keyboard/mouse events. */
static int type_takes_input(const char *t)
{
    return t && (strcmp(t, "remote") == 0 || strcmp(t, "rdp") == 0 ||
                 strcmp(t, "vnc") == 0 || strcmp(t, "web") == 0);
}

/* Local VM consoles exposed by libvirt/QEMU as Unix sockets. The panel lists
 * them so the user can pick a VM instead of typing the socket path. */
typedef struct {
    char name[128];
    char path[512];
} vnc_host_info;

static int vnc_host_cmp(const void *a, const void *b)
{
    return strcmp(((const vnc_host_info *)a)->name,
                  ((const vnc_host_info *)b)->name);
}

static int detect_vnc_hosts(vnc_host_info *out, int max)
{
    static const char *dirs[] = { "/run/vms", "/var/run/vms" };
    const size_t suffix_len = strlen(".vnc.sock");
    int n = 0;

    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]) && n < max; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir)
            continue;
        struct dirent *e;
        while ((e = readdir(dir)) != NULL && n < max) {
            const char *nm = e->d_name;
            size_t len = strlen(nm);
            if (len <= suffix_len ||
                strcmp(nm + len - suffix_len, ".vnc.sock") != 0)
                continue;

            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dirs[d], nm);
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISSOCK(st.st_mode))
                continue;

            size_t nl = len - suffix_len;
            char name[128];
            if (nl >= sizeof(name))
                nl = sizeof(name) - 1;
            memcpy(name, nm, nl);
            name[nl] = 0;

            int dup = 0;
            for (int i = 0; i < n; i++) {
                if (strcmp(out[i].name, name) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup)
                continue;

            snprintf(out[n].name, sizeof(out[n].name), "%s", name);
            snprintf(out[n].path, sizeof(out[n].path), "%s", path);
            n++;
        }
        closedir(dir);
    }
    qsort(out, (size_t)n, sizeof(out[0]), vnc_host_cmp);
    return n;
}

static int app_state_json(char *buf, size_t cap, void *ud)
{
    app_t *a = ud;
    disp_sysinfo si;
    disp_get_sysinfo(&si);

    size_t off = 0;
    {
        char host[128];
        jesc(host, sizeof(host), si.host);
        jappend(buf, cap, &off,
                "{\"host\":\"%s\",\"uptime\":%.0f,\"load\":%.2f,"
                "\"mem_total\":%llu,\"mem_free\":%llu,\"web_port\":%d,"
                "\"hwaccel\":%d,\"types\":%s,\"outputs\":[",
                host, si.uptime_s, si.load1, si.mem_total_mb, si.mem_free_mb,
                a->web_port, a->hwaccel,
                "[{\"id\":\"media\",\"label\":\"视频/流\"},"
                "{\"id\":\"image\",\"label\":\"图片\"},"
                "{\"id\":\"screen\",\"label\":\"本机画面\"},"
                "{\"id\":\"remote\",\"label\":\"远程连接\"},"
                "{\"id\":\"web\",\"label\":\"网页\"},"
                "{\"id\":\"pip\",\"label\":\"画中画\"},"
                "{\"id\":\"ui\",\"label\":\"仪表盘\"},"
                "{\"id\":\"bars\",\"label\":\"测试图\"}]");
    }

    os_mutex_lock(&a->lock);
    int n = a->n;
    screen_t *list[MAX_SLOTS];
    for (int i = 0; i < n; i++)
        list[i] = a->slot[i];
    os_mutex_unlock(&a->lock);

    for (int i = 0; i < n; i++) {
        screen_t *s = list[i];
        char name[128], src_type[32], src_arg[1400], err[700], oerr[700];
        int connected, active, width, height, refresh;
        int online, src_w, src_h, wd = 0;
        double in_fps, out_fps, pos, dur;
        char codec[64];
        disp_info info;

        os_mutex_lock(&s->lock);
        connected = s->connected;
        active = s->active;
        width = s->width;
        height = s->height;
        refresh = s->refresh;
        info = s->info;
        snprintf(src_type, sizeof(src_type), "%s", s->src_type);
        snprintf(src_arg, sizeof(src_arg), "%s", s->src_arg);
        snprintf(err, sizeof(err), "%s", s->st.error);
        snprintf(oerr, sizeof(oerr), "%s", s->open_err);
        online = s->st.online;
        in_fps = s->st.in_fps;
        out_fps = s->st.out_fps;
        pos = s->st.pos;
        dur = s->st.dur;
        src_w = s->st.src_w;
        src_h = s->st.src_h;
        snprintf(codec, sizeof(codec), "%s", s->st.codec);
        wd = s->wd_restarts;
        os_mutex_unlock(&s->lock);

        int input_ok = 0;
        if (type_takes_input(src_type)) {
            input_ok = 1;
        } else if (strcmp(src_type, "pip") == 0) {
            char mt[16], ot[16], ma[1024], oa[1024];
            int rx = 0, ry = 0, rw = 0, rh = 0;
            if (parse_pip(src_arg, mt, sizeof(mt), ma, sizeof(ma),
                          ot, sizeof(ot), oa, sizeof(oa),
                          &rx, &ry, &rw, &rh) == 0)
                input_ok = type_takes_input(mt) || type_takes_input(ot);
        }

        jesc(name, sizeof(name), s->name);
        char jtype[64], jarg[1500], jerr[800], joerr[800], jcodec[160];
        jesc(jtype, sizeof(jtype), src_type);
        jesc(jarg, sizeof(jarg), src_arg);
        jesc(jerr, sizeof(jerr), err);
        jesc(joerr, sizeof(joerr), oerr);
        jesc(jcodec, sizeof(jcodec), codec);

        char modes[900];
        size_t mo = 0;
        modes[0] = 0;
        for (int k = 0; k < info.mode_count && k < 12; k++) {
            int m = snprintf(modes + mo, sizeof(modes) - mo,
                             "%s{\"w\":%d,\"h\":%d,\"r\":%d}",
                             k ? "," : "", info.modes[k].width,
                             info.modes[k].height, info.modes[k].refresh);
            if (m < 0 || (size_t)m >= sizeof(modes) - mo)
                break;
            mo += (size_t)m;
        }

        jappend(buf, cap, &off,
                "%s{\"index\":%d,\"name\":\"%s\",\"connected\":%d,"
                "\"active\":%d,\"input_ok\":%d,"
                "\"width\":%d,\"height\":%d,\"refresh\":%d,"
                "\"modes\":[%s],"
                "\"source_type\":\"%s\",\"source_arg\":\"%s\","
                "\"online\":%d,\"codec\":\"%s\",\"fps\":%.2f,\"out_fps\":%.2f,"
                "\"pos\":%.2f,\"dur\":%.2f,\"src_w\":%d,\"src_h\":%d,"
                "\"watchdog\":%d,\"error\":\"%s\",\"open_error\":\"%s\"}",
                i ? "," : "", s->index, name, connected, active, input_ok,
                width, height, refresh, modes, jtype, jarg, online, jcodec,
                in_fps,
                out_fps, pos, dur, src_w, src_h, wd, jerr, joerr);
    }

    jappend(buf, cap, &off, "],\"inputs\":[");

    input_device devs[32];
    int nd = input_list(devs, 32);
    for (int i = 0; i < nd; i++) {
        char jid[600], jname[300], jout[160], jgrp[300];
        jesc(jid, sizeof(jid), devs[i].id);
        jesc(jname, sizeof(jname), devs[i].name);
        jesc(jout, sizeof(jout), devs[i].output);
        jesc(jgrp, sizeof(jgrp), devs[i].group);
        jappend(buf, cap, &off,
                "%s{\"id\":\"%s\",\"group\":\"%s\",\"name\":\"%s\","
                "\"output\":\"%s\",\"ready\":%d,\"kind\":%d,"
                "\"events\":%lu,\"last_ms\":%ld}",
                i ? "," : "", jid, jgrp, jname, jout, devs[i].ready,
                devs[i].kind, devs[i].events, devs[i].last_ms);
    }
    jappend(buf, cap, &off, "],\"vnc_hosts\":[");

    vnc_host_info hosts[32];
    int nh = detect_vnc_hosts(hosts, 32);
    for (int i = 0; i < nh; i++) {
        char jname[300], jpath[1200];
        jesc(jname, sizeof(jname), hosts[i].name);
        jesc(jpath, sizeof(jpath), hosts[i].path);
        jappend(buf, cap, &off,
                "%s{\"name\":\"%s\",\"path\":\"%s\"}",
                i ? "," : "", jname, jpath);
    }
    jappend(buf, cap, &off, "]}");
    return (int)off;
}

static int app_apply(int index, const char *type, const char *arg,
                     int w, int h, void *ud)
{
    app_t *a = ud;
    os_mutex_lock(&a->lock);
    if ((a->qt + 1) % 64 == a->qh) {
        os_mutex_unlock(&a->lock);
        return -1;
    }
    app_cmd *c = &a->q[a->qt];
    memset(c, 0, sizeof(*c));
    c->index = index;
    snprintf(c->type, sizeof(c->type), "%s", type ? type : "media");
    snprintf(c->arg, sizeof(c->arg), "%s", arg ? arg : "");
    c->w = w;
    c->h = h;
    a->qt = (a->qt + 1) % 64;
    os_mutex_unlock(&a->lock);
    return 0;
}

static int app_disable(int index, void *ud)
{
    app_t *a = ud;
    os_mutex_lock(&a->lock);
    if ((a->qt + 1) % 64 == a->qh) {
        os_mutex_unlock(&a->lock);
        return -1;
    }
    app_cmd *c = &a->q[a->qt];
    memset(c, 0, sizeof(*c));
    c->index = index;
    c->disable = 1;
    a->qt = (a->qt + 1) % 64;
    os_mutex_unlock(&a->lock);
    return 0;
}

static int app_copy_preview(int index, uint8_t *rgb, size_t cap,
                            int *w, int *h, uint64_t *serial, void *ud)
{
    app_t *a = ud;
    screen_t *s = find_slot(a, index);
    if (!s)
        return -1;

    os_mutex_lock(&s->preview_lock);
    if (!s->preview || s->pw <= 0 || s->ph <= 0) {
        os_mutex_unlock(&s->preview_lock);
        return -1;
    }
    size_t need = (size_t)s->pw * (size_t)s->ph * 3;
    if (need > cap) {
        os_mutex_unlock(&s->preview_lock);
        return -1;
    }
    memcpy(rgb, s->preview, need);
    *w = s->pw;
    *h = s->ph;
    *serial = s->preview_serial;
    os_mutex_unlock(&s->preview_lock);
    return 0;
}

/* Only these sources consume keyboard/mouse events.  The panel hides the
 * others, and the API rejects them as well.  A picture-in-picture is allowed
 * when its main or overlay layer is RDP/VNC/web. */
static int output_takes_input(screen_t *s)
{
    if (!s)
        return 0;
    os_mutex_lock(&s->lock);
    int ok = type_takes_input(s->src_type);
    if (!ok && strcmp(s->src_type, "pip") == 0) {
        char mt[16], ot[16], ma[1024], oa[1024];
        int rx = 0, ry = 0, rw = 0, rh = 0;
        if (parse_pip(s->src_arg, mt, sizeof(mt), ma, sizeof(ma),
                      ot, sizeof(ot), oa, sizeof(oa),
                      &rx, &ry, &rw, &rh) == 0)
            ok = type_takes_input(mt) || type_takes_input(ot);
    }
    os_mutex_unlock(&s->lock);
    return ok;
}

static int web_bind_input(const char *id, const char *out, void *ud)
{
    app_t *a = ud;
    if (!id || !*id)
        return -1;
    if (out && *out && !output_takes_input(find_slot_by_name(a, out)))
        return -1;
    if (input_bind(id, out) != 0)
        return -1;
    os_mutex_lock(&a->lock);
    if (out && *out)
        config_bind_input(&a->cfg, id, out);
    else
        config_unbind_input(&a->cfg, id);
    a->cfg_dirty = 1;
    os_mutex_unlock(&a->lock);
    return 0;
}

static void app_request_quit(void *ud)
{
    (void)ud;
    g_run = 0;
}

/* Name based wrappers used by the web API (connector names are stable). */
static int web_apply(const char *out, const char *type, const char *arg,
                     int w, int h, void *ud)
{
    app_t *a = ud;
    screen_t *s = find_slot_by_name(a, out);
    if (!s)
        return -1;
    return app_apply(s->index, type, arg, w, h, ud);
}

static int web_disable(const char *out, void *ud)
{
    app_t *a = ud;
    screen_t *s = find_slot_by_name(a, out);
    if (!s)
        return -1;
    return app_disable(s->index, ud);
}

static int web_copy_preview(const char *out, uint8_t *rgb, size_t cap,
                            int *w, int *h, uint64_t *serial, void *ud)
{
    app_t *a = ud;
    screen_t *s = find_slot_by_name(a, out);
    if (!s || !s->active)
        return -2;
    return app_copy_preview(s->index, rgb, cap, w, h, serial, ud);
}

static void process_commands(app_t *a)
{
    for (;;) {
        app_cmd c;
        os_mutex_lock(&a->lock);
        if (a->qh == a->qt) {
            os_mutex_unlock(&a->lock);
            break;
        }
        c = a->q[a->qh];
        a->qh = (a->qh + 1) % 64;
        os_mutex_unlock(&a->lock);

        char err[256] = "";
        if (c.disable) {
            disable_output(a, c.index, 1);
        } else {
            if (apply_output(a, c.index, c.type, c.arg, c.w, c.h, 1,
                             err, sizeof(err)) != 0) {
                fprintf(stderr, "apply output %d failed: %s\n", c.index, err);
            }
        }
    }
}

/* ---------------------------------------------------------------- helpers */

static void jesc(char *dst, size_t cap, const char *s)
{
    size_t o = 0;
    for (; *s && o + 8 < cap;) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f) {
            o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
            s++;
        } else if (c < 0x80) {
            if (c == '"' || c == '\\')
                dst[o++] = '\\';
            dst[o++] = (char)c;
            s++;
        } else {
            /* Copy a valid UTF-8 sequence; replace bad bytes with '?' so the
             * JSON stays parseable even for truncated or non-UTF-8 errors. */
            int len = (c & 0xe0) == 0xc0 ? 2 :
                      (c & 0xf0) == 0xe0 ? 3 :
                      (c & 0xf8) == 0xf0 ? 4 : 0;
            int ok = len > 0 && o + (size_t)len + 8 < cap;
            for (int k = 1; ok && k < len; k++)
                if (((unsigned char)s[k] & 0xc0) != 0x80)
                    ok = 0;
            if (ok) {
                for (int k = 0; k < len; k++)
                    dst[o++] = s[k];
                s += len;
            } else {
                dst[o++] = '?';
                s++;
            }
        }
    }
    dst[o] = 0;
}

static void jappend(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
    if (*off >= cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= cap - *off)
        *off = cap - 1;
    else
        *off += (size_t)n;
}

/* ------------------------------------------------------------------ init */

typedef struct {
    char conn[64];
    int w, h;
    char type[16];
    char arg[1024];
} cli_spec;

static cli_spec g_specs[MAX_SLOTS];
static int g_nspec;

static void split_source(const char *src, char *type, size_t tn,
                         char *arg, size_t an)
{
    const char *colon = strchr(src, ':');
    if (colon && colon - src < 16) {
        char head[16];
        size_t hl = (size_t)(colon - src);
        memcpy(head, src, hl);
        head[hl] = 0;
        const char *ct = canon_layer_type(head);
        if (strcmp(ct, "image") == 0 || strcmp(ct, "monitor") == 0 ||
            strcmp(ct, "vnc") == 0 || strcmp(ct, "media") == 0 ||
            strcmp(ct, "remote") == 0 || strcmp(ct, "rdp") == 0 ||
            strcmp(ct, "screen") == 0 || strcmp(ct, "web") == 0 ||
            strcmp(ct, "ui") == 0 || strcmp(ct, "bars") == 0) {
            snprintf(type, tn, "%s", ct);
            snprintf(arg, an, "%s", colon + 1);
            return;
        }
    }
    const char *ct = canon_layer_type(src);
    if (strcmp(ct, "ui") == 0 || strcmp(ct, "bars") == 0 ||
        strcmp(ct, "screen") == 0) {
        snprintf(type, tn, "%s", ct);
        arg[0] = 0;
    } else {
        snprintf(type, tn, "media");
        snprintf(arg, an, "%s", src);
    }
}

static int parse_spec(const char *text, cli_spec *sp)
{
    char tmp[1400];
    snprintf(tmp, sizeof(tmp), "%s", text);
    char *eq = strchr(tmp, '=');
    char *lhs = tmp;
    const char *rhs = "bars";
    if (eq) {
        *eq = 0;
        rhs = eq + 1;
    }
    char *at = strchr(lhs, '@');
    if (at) {
        *at = 0;
        sscanf(at + 1, "%dx%d", &sp->w, &sp->h);
    }
    snprintf(sp->conn, sizeof(sp->conn), "%.63s", lhs);
    split_source(rhs, sp->type, sizeof(sp->type), sp->arg, sizeof(sp->arg));
    return 0;
}

static void usage(const char *prog)
{
    printf(
        "HDMI Display - independent multi-screen video wall\n"
        "\n"
        "Usage: %s [options] [CONNECTOR[@WxH]=SOURCE ...]\n"
        "\n"
        "SOURCE: ui | bars | image:PATH|URL | vnc:HOST[:PORT] | rdp:USER:PASS@HOST[:PORT] |\n"
        "        screen[:DISPLAY] | web:URL | rtsp://... | file\n"
        "\n"
        "Options:\n"
        "  -l, --list            list connectors/monitors and exit\n"
        "  -d, --device PATH     DRM device (default /dev/dri/card0)\n"
        "  -c, --config PATH     config file (default hdmi_display.conf)\n"
        "      --no-config       ignore the config file\n"
        "  -w, --web-port N      HTTP control panel port (default 8080, 0 = off)\n"
        "  -b, --web-bind ADDR   bind address (default 0.0.0.0)\n"
        "  -t, --web-token TOK   require ?token=TOK on the web UI\n"
        "      --web-dir DIR     static panel directory (default ./web)\n"
        "      --web-socket PATH Unix socket for the fnOS gateway\n"
        "      --web-prefix P    gateway path prefix, e.g. /app/hdmi-display\n"
        "  -f, --fps N           UI refresh rate (default 30)\n"
        "      --hwaccel         try VAAPI decoding\n"
        "      --audio DEV       media audio: auto | off | hw:C,D (default auto)\n"
                        "      --demo            ignore config, open up to 3 test screens\n"
        "  -v, --verbose\n"
        "  -h, --help\n",
        prog);
}

static void auto_assign(app_t *a)
{
    int opened = 0;
    static const char *types[3] = { "bars", "bars", "bars" };
    for (int i = 0; i < a->n && opened < 3; i++) {
        screen_t *s = a->slot[i];
        os_mutex_lock(&s->lock);
        int connected = s->connected;
        os_mutex_unlock(&s->lock);
        if (!connected)
            continue;
        char err[256];
        if (apply_output(a, s->index, types[opened], "", 0, 0, 0,
                         err, sizeof(err)) == 0)
            opened++;
    }
}

/* Keeps the wall alive: if an output has not flipped a frame for 10s the
 * source is reopened; if a worker thread died it is restarted. */
static void watchdog_check(app_t *a)
{
    double now = disp_now();
    for (int i = 0; i < a->n; i++) {
        screen_t *s = a->slot[i];
        os_mutex_lock(&s->lock);
        int active = s->active;
        int started = s->thread_started;
        double last = s->last_present;
        os_mutex_unlock(&s->lock);

        if (!active)
            continue;

        if (!started) {
            s->stop = 0;
            s->dirty = 1;
            if (os_thread_create(&s->thread, screen_thread, s) == 0) {
                s->thread_started = 1;
                os_mutex_lock(&s->lock);
                s->wd_restarts++;
                os_mutex_unlock(&s->lock);
                fprintf(stderr, "watchdog: restarted worker for %s\n", s->name);
            }
            continue;
        }

        os_mutex_lock(&s->lock);
        int rdp_live = (strcmp(s->st.codec, "rdp (freerdp)") == 0) &&
                       s->st.online;
        os_mutex_unlock(&s->lock);

        if (last > 0.0 && now - last > 10.0 && !rdp_live) {
            os_mutex_lock(&s->lock);
            s->dirty = 1;
            s->wd_restarts++;
            os_mutex_unlock(&s->lock);
            s->last_present = now;
            fprintf(stderr, "watchdog: %s produced no frame for %.1fs, "
                            "reopening source\n", s->name, now - last);
            continue;
        }

        /* A native RDP stream stops producing frames when the session drops.
         * Reopen it so a lost connection recovers without a restart. */
        os_mutex_lock(&s->lock);
        int rdp_failed = (strcmp(s->st.codec, "rdp (freerdp)") == 0) &&
                         !s->st.online && s->st.error[0] != 0;
        os_mutex_unlock(&s->lock);
        if (rdp_failed) {
            os_mutex_lock(&s->lock);
            s->dirty = 1;
            s->wd_restarts++;
            os_mutex_unlock(&s->lock);
            fprintf(stderr, "watchdog: %s RDP session ended, reconnecting\n",
                    s->name);
        }
    }
}

static void resolve_audio(app_t *a, const char *override)
{
    const char *v = (override && *override) ? override : a->cfg.audio;
    a->audio_dev[0] = 0;
    a->audio_auto = 0;
    if (!v || !*v || strcmp(v, "off") == 0)
        return;
    if (strcmp(v, "auto") == 0) {
        a->audio_auto = 1;   /* resolved per output from its GPU */
        return;
    }
    snprintf(a->audio_dev, sizeof(a->audio_dev), "%s", v);
}

static void screen_set_audio(screen_t *s)
{
    s->audio_dev[0] = 0;
    if (g_app.audio_auto) {
        if (alsa_detect_for_drm_conn(s->cardno, s->conn, s->audio_dev,
                                     sizeof(s->audio_dev)) != 0)
            s->audio_dev[0] = 0;
    } else {
        snprintf(s->audio_dev, sizeof(s->audio_dev), "%s", g_app.audio_dev);
    }
    fprintf(stderr, "audio %s (card%d): %s\n", s->name, s->cardno,
            s->audio_dev[0] ? s->audio_dev : "none");
}

int main(int argc, char **argv)
{
    const char *device = NULL;
    const char *cfg_path = "hdmi_display.conf";
    int no_config = 0;
    int demo = 0;
    int web_port = 8080;
    int web_port_set = 0;
    const char *web_bind = "0.0.0.0";
    const char *web_token = NULL;
    const char *web_dir = NULL;
    const char *web_socket = NULL;
    const char *web_prefix = NULL;
    const char *audio_opt = NULL;
    int want_list = 0;
    int hwaccel = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(a, "-l") == 0 || strcmp(a, "--list") == 0) {
            want_list = 1;
        } else if ((strcmp(a, "-d") == 0 || strcmp(a, "--device") == 0) && i + 1 < argc) {
            device = argv[++i];
        } else if ((strcmp(a, "-c") == 0 || strcmp(a, "--config") == 0) && i + 1 < argc) {
            cfg_path = argv[++i];
        } else if (strcmp(a, "--no-config") == 0) {
            no_config = 1;
        } else if ((strcmp(a, "-w") == 0 || strcmp(a, "--web-port") == 0) && i + 1 < argc) {
            web_port = atoi(argv[++i]);
            web_port_set = 1;
        } else if ((strcmp(a, "-b") == 0 || strcmp(a, "--web-bind") == 0) && i + 1 < argc) {
            web_bind = argv[++i];
        } else if ((strcmp(a, "-t") == 0 || strcmp(a, "--web-token") == 0) && i + 1 < argc) {
            web_token = argv[++i];
        } else if (strcmp(a, "--web-dir") == 0 && i + 1 < argc) {
            web_dir = argv[++i];
        } else if (strcmp(a, "--web-socket") == 0 && i + 1 < argc) {
            web_socket = argv[++i];
        } else if (strcmp(a, "--web-prefix") == 0 && i + 1 < argc) {
            web_prefix = argv[++i];
        } else if ((strcmp(a, "-f") == 0 || strcmp(a, "--fps") == 0) && i + 1 < argc) {
            g_fps = atoi(argv[++i]);
        } else if (strcmp(a, "--hwaccel") == 0) {
            hwaccel = 1;
        } else if (strcmp(a, "--audio") == 0 && i + 1 < argc) {
            audio_opt = argv[++i];
        } else if (strcmp(a, "--demo") == 0) {
            demo = 1;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            g_verbose = 1;
        } else if (a[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", a);
            usage(argv[0]);
            return 1;
        } else {
            if (g_nspec >= MAX_SLOTS) {
                fprintf(stderr, "too many outputs\n");
                return 1;
            }
            parse_spec(a, &g_specs[g_nspec++]);
        }
    }

    if (g_fps < 1) g_fps = 1;
    if (g_fps > 120) g_fps = 120;
    if (demo)
        g_nspec = 0;
    if (web_port < 0 || web_port > 65535)
        web_port = 8080;

    int single = app_singleton(getenv("HDMI_INSTANCE") && *getenv("HDMI_INSTANCE")
                               ? getenv("HDMI_INSTANCE") : "hdmi_display");
    if (!single) {
        fprintf(stderr, "another hdmi_display instance is already running "
                        "(close it before starting a new one)\n");
        return 0;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    setvbuf(stdout, NULL, _IOLBF, 0);

    memset(&g_app, 0, sizeof(g_app));
    os_mutex_init(&g_app.lock);
    os_mutex_init(&g_input_lock);
    g_app.web_port = web_port;
    g_app.hwaccel = hwaccel;
    snprintf(g_app.web_bind, sizeof(g_app.web_bind), "%s", web_bind);
    if (web_token)
        snprintf(g_app.web_token, sizeof(g_app.web_token), "%s", web_token);

    char err[256] = "";
    if (device) {
        if (disp_init(&g_app.ctx[0], device, err, sizeof(err)) != 0) {
            fprintf(stderr, "display init failed: %s\n", err);
            return 1;
        }
        const char *p = strstr(device, "card");
        g_app.cardno[0] = p ? atoi(p + 4) : 0;
        g_app.n_ctx = 1;
    } else {
        for (int i = 0; i < DISP_MAX_CARDS; i++) {
            char path[64], e[256] = "";
            snprintf(path, sizeof(path), "/dev/dri/card%d", i);
            if (access(path, R_OK) != 0)
                continue;
            disp_ctx *c = NULL;
            if (disp_init(&c, path, e, sizeof(e)) == 0) {
                g_app.ctx[g_app.n_ctx] = c;
                g_app.cardno[g_app.n_ctx] = i;
                g_app.n_ctx++;
            } else {
                fprintf(stderr, "display %s: %s\n", path, e);
            }
        }
        if (g_app.n_ctx == 0) {
            fprintf(stderr, "display init failed: no usable /dev/dri/card*\n");
            return 1;
        }
    }
    snprintf(g_backend, sizeof(g_backend), "%s", disp_backend_name());
    fprintf(stderr, "display cards:");
    for (int i = 0; i < g_app.n_ctx; i++)
        fprintf(stderr, " %d", g_app.cardno[i]);
    fprintf(stderr, "\n");

    if (want_list) {
        for (int i = 0; i < g_app.n_ctx; i++) {
            printf("== card%d ==\n", g_app.cardno[i]);
            disp_list(g_app.ctx[i], stdout);
            disp_shutdown(g_app.ctx[i]);
        }
        return 0;
    }

    video_global_init();

    /* create one slot per connector on every card */
    for (int ci = 0; ci < g_app.n_ctx; ci++) {
        disp_info infos[DISP_MAX_OUTPUTS];
        int ninfo = disp_enumerate(g_app.ctx[ci], infos, DISP_MAX_OUTPUTS);
        for (int i = 0; i < ninfo && g_app.n < MAX_SLOTS; i++) {
            screen_t *s = calloc(1, sizeof(*s));
            if (!s)
                break;
            s->index = g_app.n;
            s->ctx = g_app.ctx[ci];
            s->cardno = g_app.cardno[ci];
            snprintf(s->conn, sizeof(s->conn), "%.63s", infos[i].name);
            snprintf(s->name, sizeof(s->name), "card%d-%.48s", s->cardno, s->conn);
            s->connected = infos[i].connected;
            s->info = infos[i];
            s->width = infos[i].width;
            s->height = infos[i].height;
            s->refresh = infos[i].refresh;
            os_mutex_init(&s->lock);
            os_mutex_init(&s->preview_lock);
            snprintf(s->src_type, sizeof(s->src_type), "bars");
            g_app.slot[g_app.n++] = s;
        }
    }

    config_init(&g_app.cfg, cfg_path);
    int have_cfg = 0;
    int loaded = -1;
    if (!no_config && !demo && g_nspec == 0) {
        loaded = config_load(&g_app.cfg);
        have_cfg = (loaded >= 0);
        if (!web_port_set && g_app.cfg.web_port > 0)
            g_app.web_port = g_app.cfg.web_port;
    }
    /* Resolve audio before any source starts: the render thread reads it on
     * the first sync and would otherwise miss the restored outputs. */
    resolve_audio(&g_app, audio_opt);
    for (int i = 0; i < g_app.n; i++)
        screen_set_audio(g_app.slot[i]);
    fprintf(stderr, "audio: %s\n",
            g_app.audio_auto ? "auto (per GPU)"
                             : (g_app.audio_dev[0] ? g_app.audio_dev : "off"));
    /* Every connected output starts on the test pattern. Saved sources are
     * intentionally not re-applied here: the panel is the place where the
     * user chooses what each screen should show after startup. */
    for (int i = 0; i < g_app.n; i++) {
        screen_t *s = g_app.slot[i];
        os_mutex_lock(&s->lock);
        int connected = s->connected;
        os_mutex_unlock(&s->lock);
        if (!connected)
            continue;
        char e[256];
        apply_output(&g_app, s->index, "bars", "", 0, 0, 0, e, sizeof(e));
    }
    g_app.cfg.web_port = g_app.web_port;

    input_init(route_input, &g_app);
    for (int i = 0; i < g_app.cfg.input_count; i++)
        input_bind(g_app.cfg.inputs[i].device, g_app.cfg.inputs[i].output);

    if (g_nspec > 0) {
        for (int i = 0; i < g_nspec; i++) {
            cli_spec *sp = &g_specs[i];
            screen_t *s = find_slot_by_name(&g_app, sp->conn);
            if (!s) {
                /* allow numeric enumeration index */
                int idx = atoi(sp->conn);
                if (idx >= 0 && idx < g_app.n)
                    s = g_app.slot[idx];
            }
            if (!s) {
                fprintf(stderr, "output '%s' not found\n", sp->conn);
                continue;
            }
            char e[256];
            if (apply_output(&g_app, s->index, sp->type, sp->arg,
                             sp->w, sp->h, 0, e, sizeof(e)) != 0)
                fprintf(stderr, "output %s: %s\n", s->name, e);
        }
    } else if (demo || (!have_cfg && g_nspec == 0)) {
        auto_assign(&g_app);
    }

    int active = 0;
    for (int i = 0; i < g_app.n; i++)
        if (g_app.slot[i]->active)
            active++;

    if (active == 0 && (demo || (!have_cfg && g_nspec == 0))) {
        auto_assign(&g_app);
        for (int i = 0; i < g_app.n; i++)
            if (g_app.slot[i]->active)
                active++;
    }

    printf("HDMI Display  backend: %s  outputs: %d/%d\n",
           g_backend, active, g_app.n);
    for (int i = 0; i < g_app.n; i++) {
        screen_t *s = g_app.slot[i];
        printf("  [%d] %-12s %s  %dx%d  <- %s\n", i, s->name,
               s->active ? "ON " : "off", s->width, s->height,
               s->active ? s->source : "-");
    }

    web_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.state_json = app_state_json;
    ops.apply = web_apply;
    ops.disable = web_disable;
    ops.copy_preview = web_copy_preview;
    ops.bind_input = web_bind_input;
    ops.request_quit = app_request_quit;
    ops.token = g_app.web_token[0] ? g_app.web_token : NULL;

    if (!web_dir)
        web_dir = "web";

    web_config wc;
    memset(&wc, 0, sizeof(wc));
    wc.port = g_app.web_port;
    wc.bind_addr = g_app.web_bind;
    wc.socket_path = web_socket;
    wc.prefix = web_prefix;
    wc.web_dir = web_dir;

    if (wc.port > 0 || (web_socket && *web_socket)) {
        if (web_start(&wc, &ops, &g_app, err, sizeof(err)) != 0) {
            fprintf(stderr, "web server: %s (continuing without it)\n", err);
            g_app.web_port = 0;
        } else {
            if (wc.port > 0) {
                disp_sysinfo si;
                disp_get_sysinfo(&si);
                printf("web control: http://%s:%d/%s%s\n",
                       si.host[0] ? si.host : "localhost", g_app.web_port,
                       g_app.web_token[0] ? "?token=" : "",
                       g_app.web_token[0] ? g_app.web_token : "");
            }
            if (web_socket && *web_socket)
                printf("web gateway: unix:%s%s%s\n", web_socket,
                       web_prefix && *web_prefix ? " prefix " : "",
                       web_prefix && *web_prefix ? web_prefix : "");
        }
    }

    g_app.started = disp_now();
    double next_poll = g_app.started + 1.0;

    while (g_run) {
        process_commands(&g_app);

        double now = disp_now();
        if (now >= next_poll) {
            poll_outputs(&g_app);
            watchdog_check(&g_app);
            next_poll = now + 1.0;
        }

        int pump_rc = 0;
        for (int i = 0; i < g_app.n_ctx; i++)
            if (disp_pump(g_app.ctx[i]) != 0)
                pump_rc = -1;
        if (pump_rc != 0) {
            g_run = 0;
            break;
        }
        os_mutex_lock(&g_app.lock);
        if (g_app.cfg_dirty) {
            if (config_save(&g_app.cfg) != 0)
                fprintf(stderr, "config: cannot save %s\n", g_app.cfg.path);
            g_app.cfg_dirty = 0;
        }
        os_mutex_unlock(&g_app.lock);
        disp_sleep_ms(20);
    }

    g_run = 0;
    for (int i = 0; i < g_app.n; i++)
        screen_close_output(&g_app, g_app.slot[i]);

    input_shutdown();
    os_mutex_destroy(&g_input_lock);

    web_stop();

    if (g_app.cfg_dirty && config_save(&g_app.cfg) != 0)
        fprintf(stderr, "config: cannot save %s\n", g_app.cfg.path);

    for (int i = 0; i < g_app.n; i++) {
        screen_t *s = g_app.slot[i];
        os_mutex_lock(&s->preview_lock);
        free(s->preview);
        s->preview = NULL;
        os_mutex_unlock(&s->preview_lock);
        os_mutex_destroy(&s->preview_lock);
        os_mutex_destroy(&s->lock);
        free(s);
    }
    g_app.n = 0;

    for (int i = 0; i < g_app.n_ctx; i++)
        disp_shutdown(g_app.ctx[i]);
    video_global_deinit();
    printf("bye\n");
    return 0;
}
