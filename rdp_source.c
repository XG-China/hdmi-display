/*
 * rdp_source.c - Microsoft RDP source using libfreerdp directly.
 *
 * No Xvfb, no xfreerdp process, no XTest:
 *
 *   Windows RDP server  <->  libfreerdp (GFX/AVC444 decode)  ->  BGRA canvas
 *                        <-  freerdp_input_* (scancodes, absolute mouse)
 *
 * The FreeRDP GDI decoder renders into its own primary buffer; EndPaint copies
 * that into the canvas consumed by the DRM/KMS screen thread and overlays the
 * cursor shape announced by the server. Keyboard input uses evdev scancodes
 * directly, so the guest layout is applied by Windows itself.
 */
#define _GNU_SOURCE
#include "rdp_source.h"
#include "os_thread.h"
#include "rdp_audio.h"

#include <freerdp/freerdp.h>
#include <freerdp/client.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/gdi/video.h>
#include <freerdp/graphics.h>
#include <freerdp/input.h>
#include <freerdp/update.h>
#include <freerdp/pointer.h>
#include <freerdp/event.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/channels/video.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/client/video.h>
#include <freerdp/codec/color.h>
#include <freerdp/scancode.h>

#include <winpr/synch.h>

#include <openssl/provider.h>

#include <linux/input-event-codes.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <libswscale/swscale.h>

struct rdp_source {
    os_thread_t th;
    os_mutex_t lock;      /* framebuffer + cursor */
    os_mutex_t in_lock;   /* input serialization / connection state */
    volatile int running;
    volatile int connected;

    char host[128];
    char user[128];
    char pass[256];
    int  port;
    int  out_w, out_h;

    unsigned rdp_layout;

    freerdp *inst;
    rdpContext *context;
    void *pending_gfx;    /* RdpgfxClientContext* connected before gdi_init */
    void *pending_video;  /* VideoClientContext* */

    uint32_t *fb;
    int fb_w, fb_h;

    uint64_t serial;
    int online;
    int64_t last_rx;
    double fps;
    double fps_t0;
    int fps_frames;
    char err[256];

    int prev_mask;
    int px, py;           /* cursor position, guest pixels */
    int last_sent_x, last_sent_y; /* last position sent to the guest */
    int64_t last_ptr_ms;  /* throttle cursor-driven presents to ~60 Hz */
    int64_t last_move_ms; /* throttle RDP motion PDUs (gaming mice) */

    uint32_t *cur_px;     /* BGRA cursor image from the server */
    int cur_w, cur_h;
    int cur_hx, cur_hy;
    int cur_visible;

    unsigned char keydown[512];

    struct SwsContext *sws;
    int sws_fw, sws_fh, sws_w, sws_h;
};

/* The FreeRDP context is extended so every callback can find its source. */
typedef struct {
    rdpContext context;
    rdp_source *src;
} hdmi_rdp_context;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* OpenSSL 3 moved MD4/RC4 (used by NTLM/NLA) to the legacy provider. Load it
 * once before the first FreeRDP connection. */
static pthread_once_t g_crypto_once = PTHREAD_ONCE_INIT;
static void crypto_init_once(void)
{
    OSSL_PROVIDER_load(NULL, "default");
    OSSL_PROVIDER_load(NULL, "legacy");
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_err(rdp_source *r, const char *fmt, ...)
{
    va_list ap;
    os_mutex_lock(&r->lock);
    va_start(ap, fmt);
    vsnprintf(r->err, sizeof(r->err), fmt, ap);
    va_end(ap);
    r->online = 0;
    os_mutex_unlock(&r->lock);
}

/* Read the host's configured XKB layout and map it to an RDP layout id. */
static void detect_keyboard_layout(char *name, size_t cap, unsigned *rdp_id)
{
    snprintf(name, cap, "us");
    FILE *f = fopen("/etc/default/keyboard", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *p = strstr(line, "XKBLAYOUT");
            if (!p)
                continue;
            p = strchr(p, '=');
            if (!p)
                continue;
            p++;
            while (*p == '"' || *p == '\'' || *p == ' ' || *p == '\t')
                p++;
            char *end = p;
            while (*end && *end != '"' && *end != '\'' && *end != '\n' &&
                   *end != ' ' && *end != '\t')
                end++;
            *end = 0;
            if (*p)
                snprintf(name, cap, "%s", p);
            break;
        }
        fclose(f);
    }
    if (strcmp(name, "us") == 0) *rdp_id = 0x409;
    else if (strcmp(name, "cn") == 0) *rdp_id = 0x804;
    else if (strcmp(name, "gb") == 0) *rdp_id = 0x809;
    else if (strcmp(name, "de") == 0) *rdp_id = 0x407;
    else if (strcmp(name, "fr") == 0) *rdp_id = 0x40c;
    else if (strcmp(name, "es") == 0) *rdp_id = 0x40a;
    else if (strcmp(name, "it") == 0) *rdp_id = 0x410;
    else if (strcmp(name, "ru") == 0) *rdp_id = 0x419;
    else if (strcmp(name, "jp") == 0) *rdp_id = 0x411;
    else if (strcmp(name, "kr") == 0) *rdp_id = 0x412;
    else if (strcmp(name, "nl") == 0) *rdp_id = 0x413;
    else if (strcmp(name, "pt") == 0) *rdp_id = 0x816;
    else if (strcmp(name, "br") == 0) *rdp_id = 0x416;
    else if (strcmp(name, "pl") == 0) *rdp_id = 0x415;
    else if (strcmp(name, "tr") == 0) *rdp_id = 0x41f;
    else if (strcmp(name, "cz") == 0) *rdp_id = 0x405;
    else if (strcmp(name, "se") == 0) *rdp_id = 0x41d;
    else if (strcmp(name, "dk") == 0) *rdp_id = 0x406;
    else if (strcmp(name, "no") == 0) *rdp_id = 0x414;
    else if (strcmp(name, "fi") == 0) *rdp_id = 0x40b;
    else if (strcmp(name, "ch") == 0) *rdp_id = 0x807;
    else if (strcmp(name, "at") == 0) *rdp_id = 0x407;
    else *rdp_id = 0x409;
}

/* Linux evdev keycodes are the PC/AT set 1 scancodes for the standard block.
 * Only the extended keys need an explicit translation. */
static uint32_t evdev_scancode(uint16_t code, int *valid)
{
    *valid = 1;
    switch (code) {
    case KEY_KPENTER:  return MAKE_RDP_SCANCODE(0x1c, TRUE);
    case KEY_RIGHTCTRL:return MAKE_RDP_SCANCODE(0x1d, TRUE);
    case KEY_KPSLASH:  return MAKE_RDP_SCANCODE(0x35, TRUE);
    case KEY_SYSRQ:    return MAKE_RDP_SCANCODE(0x37, TRUE);
    case KEY_RIGHTALT: return MAKE_RDP_SCANCODE(0x38, TRUE);
    case KEY_HOME:     return MAKE_RDP_SCANCODE(0x47, TRUE);
    case KEY_UP:       return MAKE_RDP_SCANCODE(0x48, TRUE);
    case KEY_PAGEUP:   return MAKE_RDP_SCANCODE(0x49, TRUE);
    case KEY_LEFT:     return MAKE_RDP_SCANCODE(0x4b, TRUE);
    case KEY_RIGHT:    return MAKE_RDP_SCANCODE(0x4d, TRUE);
    case KEY_END:      return MAKE_RDP_SCANCODE(0x4f, TRUE);
    case KEY_DOWN:     return MAKE_RDP_SCANCODE(0x50, TRUE);
    case KEY_PAGEDOWN: return MAKE_RDP_SCANCODE(0x51, TRUE);
    case KEY_INSERT:   return MAKE_RDP_SCANCODE(0x52, TRUE);
    case KEY_DELETE:   return MAKE_RDP_SCANCODE(0x53, TRUE);
    case KEY_LEFTMETA: return MAKE_RDP_SCANCODE(0x5b, TRUE);
    case KEY_RIGHTMETA:return MAKE_RDP_SCANCODE(0x5c, TRUE);
    case KEY_COMPOSE:  return MAKE_RDP_SCANCODE(0x5d, TRUE);
    case KEY_KPEQUAL:  return MAKE_RDP_SCANCODE(0x59, FALSE);
    case KEY_KPCOMMA:  return MAKE_RDP_SCANCODE(0x7e, FALSE);
    case KEY_PAUSE:    return MAKE_RDP_SCANCODE(0x45, FALSE);
    default: break;
    }
    /* ESC..F12 (1..88) are already set 1 scancodes, including the keypad. */
    if (code >= KEY_ESC && code <= KEY_F12)
        return MAKE_RDP_SCANCODE(code, FALSE);
    *valid = 0;
    return 0;
}

/* ------------------------------------------------------------- rendering */

static void draw_arrow_at(uint32_t *dst, int stride, int sx, int sy, int scale,
                          int max_x, int max_y)
{
    static const char *arrow[] = {
        "X           ", "XX          ", "X.X         ",
        "X..X        ", "X...X       ", "X....X      ",
        "X.....X     ", "X......X    ", "X.......X   ",
        "X........X  ", "X.....XXXXX ", "X..X..X     ",
        "X.X X..X    ", "XX  X..X    ", "X    X..X   ",
        "     X..X   ", "      X..X  ", "      X..X  ",
        "       XX   "
    };
    if (scale < 1) scale = 1;
    for (int y = 0; y < 19; y++) {
        for (int x = 0; x < 12; x++) {
            char c = arrow[y][x];
            if (c != 'X' && c != '.')
                continue;
            uint32_t color = c == 'X' ? 0xff000000u : 0xffffffffu;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    int fx = sx + x * scale + dx;
                    int fy = sy + y * scale + dy;
                    if (fx >= 0 && fx < max_x && fy >= 0 && fy < max_y)
                        dst[(size_t)fy * stride + fx] = color;
                }
            }
        }
    }
}

/* Overlay the server cursor on the presented frame.  The frame may be scaled
 * to the output, so the cursor position and shape follow the same mapping. */
static void blend_cursor_locked(rdp_source *r, uint32_t *dst, int stride,
                                int origin_x, int origin_y,
                                int disp_w, int disp_h)
{
    if (!dst || !r->cur_visible || r->fb_w <= 0 || r->fb_h <= 0)
        return;

    int sx = origin_x + (int)((int64_t)r->px * disp_w / r->fb_w);
    int sy = origin_y + (int)((int64_t)r->py * disp_h / r->fb_h);
    int scale = (disp_w >= 1600) ? 2 : 1;
    int max_x = origin_x + disp_w;
    int max_y = origin_y + disp_h;

    if (!r->cur_px || r->cur_w <= 0 || r->cur_h <= 0) {
        draw_arrow_at(dst, stride, sx, sy, scale, max_x, max_y);
        return;
    }
    for (int y = 0; y < r->cur_h; y++) {
        int fy = origin_y +
                 (int)((int64_t)(r->py - r->cur_hy + y) * disp_h / r->fb_h);
        if (fy < origin_y || fy >= max_y)
            continue;
        for (int x = 0; x < r->cur_w; x++) {
            int fx = origin_x +
                     (int)((int64_t)(r->px - r->cur_hx + x) * disp_w / r->fb_w);
            if (fx < origin_x || fx >= max_x)
                continue;
            uint32_t s = r->cur_px[(size_t)y * r->cur_w + x];
            unsigned a = s >> 24;
            if (!a)
                continue;
            size_t off = (size_t)fy * stride + fx;
            uint32_t d = dst[off];
            unsigned sr = (s >> 16) & 0xff;
            unsigned sg = (s >> 8) & 0xff;
            unsigned sb = s & 0xff;
            unsigned dr = (d >> 16) & 0xff;
            unsigned dg = (d >> 8) & 0xff;
            unsigned db = d & 0xff;
            dr = (sr * a + dr * (255 - a)) / 255;
            dg = (sg * a + dg * (255 - a)) / 255;
            db = (sb * a + db * (255 - a)) / 255;
            dst[off] = 0xff000000u | (dr << 16) | (dg << 8) | db;
        }
    }
}

/* -------------------------------------------------- FreeRDP client glue */

static BOOL rdp_begin_paint(rdpContext *context)
{
    return TRUE;
}

static BOOL rdp_end_paint(rdpContext *context)
{
    hdmi_rdp_context *hc = (hdmi_rdp_context *)context;
    rdp_source *r = hc->src;
    rdpGdi *gdi = context->gdi;
    if (!r || !gdi || !gdi->primary_buffer || gdi->width <= 0 || gdi->height <= 0)
        return TRUE;

    int w = gdi->width;
    int h = gdi->height;
    int stride = (int)gdi->stride;

    os_mutex_lock(&r->lock);
    if (w != r->fb_w || h != r->fb_h || !r->fb) {
        uint32_t *nb = realloc(r->fb, (size_t)w * (size_t)h * 4);
        if (!nb) {
            os_mutex_unlock(&r->lock);
            return TRUE;
        }
        r->fb = nb;
        r->fb_w = w;
        r->fb_h = h;
        if (!r->px && !r->py) {
            r->px = w / 2;
            r->py = h / 2;
        }
    }
    for (int y = 0; y < h; y++)
        memcpy(r->fb + (size_t)y * w,
               gdi->primary_buffer + (size_t)y * stride,
               (size_t)w * 4);
    r->serial++;
    r->online = 1;
    r->last_rx = now_ms();

    double now = now_s();
    r->fps_frames++;
    if (now - r->fps_t0 >= 1.0) {
        r->fps = r->fps_frames / (now - r->fps_t0);
        r->fps_frames = 0;
        r->fps_t0 = now;
    }
    os_mutex_unlock(&r->lock);
    return TRUE;
}

static BOOL rdp_desktop_resize(rdpContext *context)
{
    return TRUE;
}

static BOOL rdp_ptr_new(rdpContext *context, rdpPointer *pointer)
{
    return pointer != NULL;
}

static void rdp_ptr_free(rdpContext *context, rdpPointer *pointer)
{
}

static BOOL rdp_ptr_set(rdpContext *context, rdpPointer *pointer)
{
    hdmi_rdp_context *hc = (hdmi_rdp_context *)context;
    rdp_source *r = hc->src;
    if (!r || !pointer || !pointer->width || !pointer->height)
        return TRUE;
    if (pointer->width > 512 || pointer->height > 512)
        return TRUE;

    uint32_t *px = calloc((size_t)pointer->width * pointer->height, 4);
    if (!px)
        return TRUE;
    gdiPalette *palette = context->gdi ? &context->gdi->palette : NULL;
    if (!freerdp_image_copy_from_pointer_data(
            (BYTE *)px, PIXEL_FORMAT_BGRA32, 0, 0, 0,
            pointer->width, pointer->height,
            pointer->xorMaskData, pointer->lengthXorMask,
            pointer->andMaskData, pointer->lengthAndMask,
            pointer->xorBpp, palette)) {
        free(px);
        return TRUE;
    }

    os_mutex_lock(&r->lock);
    free(r->cur_px);
    r->cur_px = px;
    r->cur_w = (int)pointer->width;
    r->cur_h = (int)pointer->height;
    r->cur_hx = (int)pointer->xPos;
    r->cur_hy = (int)pointer->yPos;
    r->cur_visible = 1;
    os_mutex_unlock(&r->lock);
    return TRUE;
}

static BOOL rdp_ptr_set_null(rdpContext *context)
{
    hdmi_rdp_context *hc = (hdmi_rdp_context *)context;
    rdp_source *r = hc->src;
    if (!r)
        return TRUE;
    os_mutex_lock(&r->lock);
    r->cur_visible = 0;
    os_mutex_unlock(&r->lock);
    return TRUE;
}

static BOOL rdp_ptr_set_default(rdpContext *context)
{
    hdmi_rdp_context *hc = (hdmi_rdp_context *)context;
    rdp_source *r = hc->src;
    if (!r)
        return TRUE;
    os_mutex_lock(&r->lock);
    free(r->cur_px);
    r->cur_px = NULL;
    r->cur_w = r->cur_h = 0;
    r->cur_hx = r->cur_hy = 0;
    r->cur_visible = 1;
    os_mutex_unlock(&r->lock);
    return TRUE;
}

static BOOL rdp_ptr_set_position(rdpContext *context, UINT32 x, UINT32 y)
{
    hdmi_rdp_context *hc = (hdmi_rdp_context *)context;
    rdp_source *r = hc->src;
    if (!r)
        return TRUE;
    os_mutex_lock(&r->lock);
    if (r->px != (int)x || r->py != (int)y) {
        r->px = (int)x;
        r->py = (int)y;
        int64_t t = now_ms();
        if (t - r->last_ptr_ms >= 15) {
            r->serial++;
            r->last_ptr_ms = t;
        }
    }
    os_mutex_unlock(&r->lock);
    return TRUE;
}

static void rdp_channel_connected(void *context, const ChannelConnectedEventArgs *e)
{
    rdpContext *ctx = (rdpContext *)context;
    hdmi_rdp_context *hc = (hdmi_rdp_context *)ctx;
    rdp_source *r = hc->src;
    if (!r || !e || !e->name)
        return;
    if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
        if (ctx->gdi)
            gdi_graphics_pipeline_init(ctx->gdi, (RdpgfxClientContext *)e->pInterface);
        else
            r->pending_gfx = e->pInterface;
    } else if (strcmp(e->name, VIDEO_CONTROL_DVC_CHANNEL_NAME) == 0) {
        if (ctx->gdi)
            gdi_video_control_init(ctx->gdi, (VideoClientContext *)e->pInterface);
        else
            r->pending_video = e->pInterface;
    }
}

static void rdp_channel_disconnected(void *context, const ChannelDisconnectedEventArgs *e)
{
    rdpContext *ctx = (rdpContext *)context;
    if (!e || !e->name)
        return;
    if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0 && ctx->gdi)
        gdi_graphics_pipeline_uninit(ctx->gdi, (RdpgfxClientContext *)e->pInterface);
    else if (strcmp(e->name, VIDEO_CONTROL_DVC_CHANNEL_NAME) == 0 && ctx->gdi)
        gdi_video_control_uninit(ctx->gdi, (VideoClientContext *)e->pInterface);
}

static void send_modifier_releases(rdp_source *r)
{
    static const uint32_t mods[] = {
        MAKE_RDP_SCANCODE(0x2a, FALSE), /* left shift */
        MAKE_RDP_SCANCODE(0x36, FALSE), /* right shift */
        MAKE_RDP_SCANCODE(0x1d, FALSE), /* left ctrl */
        MAKE_RDP_SCANCODE(0x1d, TRUE),  /* right ctrl */
        MAKE_RDP_SCANCODE(0x38, FALSE), /* left alt */
        MAKE_RDP_SCANCODE(0x38, TRUE),  /* right alt */
        MAKE_RDP_SCANCODE(0x5b, TRUE),  /* left win */
        MAKE_RDP_SCANCODE(0x5c, TRUE),  /* right win */
    };
    rdpInput *in = r->context ? r->context->input : NULL;
    if (!in)
        return;
    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++)
        freerdp_input_send_keyboard_event_ex(in, FALSE, FALSE, mods[i]);
}

static BOOL rdp_pre_connect(freerdp *instance)
{
    if (!instance || !instance->context)
        return FALSE;
    PubSub_SubscribeChannelConnected(instance->context->pubSub,
                                     rdp_channel_connected);
    PubSub_SubscribeChannelDisconnected(instance->context->pubSub,
                                        rdp_channel_disconnected);
    return TRUE;
}

static BOOL rdp_post_connect(freerdp *instance)
{
    rdpContext *context = instance->context;
    hdmi_rdp_context *hc = (hdmi_rdp_context *)context;
    rdp_source *r = hc->src;

    if (!gdi_init(instance, PIXEL_FORMAT_BGRA32))
        return FALSE;

    context->update->BeginPaint = rdp_begin_paint;
    context->update->EndPaint = rdp_end_paint;
    context->update->DesktopResize = rdp_desktop_resize;

    if (context->graphics) {
        rdpPointer pointer = { 0 };
        pointer.size = sizeof(rdpPointer);
        pointer.New = rdp_ptr_new;
        pointer.Free = rdp_ptr_free;
        pointer.Set = rdp_ptr_set;
        pointer.SetNull = rdp_ptr_set_null;
        pointer.SetDefault = rdp_ptr_set_default;
        pointer.SetPosition = rdp_ptr_set_position;
        graphics_register_pointer(context->graphics, &pointer);
    }

    if (r->pending_gfx) {
        gdi_graphics_pipeline_init(context->gdi,
                                   (RdpgfxClientContext *)r->pending_gfx);
        r->pending_gfx = NULL;
    }
    if (r->pending_video) {
        gdi_video_control_init(context->gdi,
                               (VideoClientContext *)r->pending_video);
        r->pending_video = NULL;
    }

    os_mutex_lock(&r->in_lock);
    r->connected = 1;
    r->prev_mask = 0;
    r->last_sent_x = -1;
    r->last_sent_y = -1;
    r->last_move_ms = 0;
    os_mutex_unlock(&r->in_lock);

    os_mutex_lock(&r->lock);
    r->online = 1;
    r->err[0] = 0;
    r->fps_t0 = now_s();
    r->fps_frames = 0;
    os_mutex_unlock(&r->lock);

    /* Numpad digits must type digits even when the physical NumLock is off. */
    freerdp_input_send_synchronize_event(context->input, KBD_SYNC_NUM_LOCK);
    send_modifier_releases(r);
    /* Clear mouse buttons left down by a previous session. */
    freerdp_input_send_mouse_event(context->input, PTR_FLAGS_BUTTON1, 0, 0);
    freerdp_input_send_mouse_event(context->input, PTR_FLAGS_BUTTON2, 0, 0);
    freerdp_input_send_mouse_event(context->input, PTR_FLAGS_BUTTON3, 0, 0);
    return TRUE;
}

static void rdp_post_disconnect(freerdp *instance)
{
    if (!instance || !instance->context)
        return;
    hdmi_rdp_context *hc = (hdmi_rdp_context *)instance->context;
    rdp_source *r = hc->src;
    PubSub_UnsubscribeChannelConnected(instance->context->pubSub,
                                       rdp_channel_connected);
    PubSub_UnsubscribeChannelDisconnected(instance->context->pubSub,
                                          rdp_channel_disconnected);
    gdi_free(instance);
    if (r) {
        os_mutex_lock(&r->in_lock);
        r->connected = 0;
        os_mutex_unlock(&r->in_lock);
        os_mutex_lock(&r->lock);
        r->online = 0;
        r->serial++;
        os_mutex_unlock(&r->lock);
    }
}

static BOOL rdp_client_new(freerdp *instance, rdpContext *context)
{
    instance->PreConnect = rdp_pre_connect;
    instance->PostConnect = rdp_post_connect;
    instance->PostDisconnect = rdp_post_disconnect;
    return TRUE;
}

static void rdp_client_free(freerdp *instance, rdpContext *context)
{
}

static int rdp_client_start(rdpContext *context)
{
    return 0;
}

static int rdp_client_stop(rdpContext *context)
{
    return 0;
}

static BOOL rdp_global_init(void)
{
    return TRUE;
}

static void rdp_global_uninit(void)
{
}

/* --------------------------------------------------------------- thread */

static void *rdp_thread(void *arg)
{
    rdp_source *r = arg;
    freerdp *inst = r->inst;
    rdpContext *ctx = r->context;
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    BOOL connected = FALSE;

    if (!inst || !ctx) {
        set_err(r, "RDP 初始化失败");
        return NULL;
    }

    if (!freerdp_connect(inst)) {
        UINT32 e = freerdp_get_last_error(ctx);
        set_err(r, "RDP 连接失败：%s (0x%08x)",
                freerdp_get_last_error_string(e), e);
        return NULL;
    }
    connected = TRUE;

    while (r->running && !freerdp_shall_disconnect_context(ctx)) {
        DWORD n = freerdp_get_event_handles(ctx, handles,
                                            ARRAYSIZE(handles));
        if (n == 0) {
            set_err(r, "RDP 事件句柄获取失败");
            break;
        }
        DWORD st = WaitForMultipleObjects(n, handles, FALSE, 100);
        if (st == WAIT_FAILED) {
            set_err(r, "RDP 事件等待失败");
            break;
        }
        if (!freerdp_check_event_handles(ctx)) {
            UINT32 e = freerdp_get_last_error(ctx);
            if (e != FREERDP_ERROR_SUCCESS) {
                set_err(r, "RDP 会话中断：%s (0x%08x)",
                        freerdp_get_last_error_string(e), e);
            } else {
                set_err(r, "RDP 会话已断开");
            }
            break;
        }

        /* A static desktop produces no paint updates. Bump the serial once a
         * second so the wall keeps presenting and the watchdog sees a live
         * source instead of reopening it. */
        os_mutex_lock(&r->lock);
        int64_t last = r->last_rx;
        os_mutex_unlock(&r->lock);
        if (now_ms() - last > 1000) {
            os_mutex_lock(&r->lock);
            r->serial++;
            r->last_rx = now_ms();
            os_mutex_unlock(&r->lock);
        }
    }

    os_mutex_lock(&r->in_lock);
    r->connected = 0;
    os_mutex_unlock(&r->in_lock);
    if (connected) {
        freerdp_disconnect(inst);
    } else {
        freerdp_abort_connect_context(ctx);
    }
    os_mutex_lock(&r->lock);
    r->online = 0;
    os_mutex_unlock(&r->lock);
    return NULL;
}

/* --------------------------------------------------------------- public */

rdp_source *rdp_source_open(const char *arg, int out_w, int out_h,
                            const char *audio_dev,
                            char *err, size_t errsz)
{
    if (!arg || !*arg) {
        snprintf(err, errsz, "请输入 RDP 地址：用户:密码@主机[:端口]");
        return NULL;
    }
    const char *p = arg;
    if (strncasecmp(p, "rdp://", 6) == 0)
        p += 6;
    else if (strncasecmp(p, "rdp:", 4) == 0)
        p += 4;

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", p);
    int use_admin = 1;
    char *opt = strstr(tmp, "?normal");
    if (opt) {
        *opt = 0;
        use_admin = 0;
    } else {
        opt = strstr(tmp, "?admin");
        if (opt)
            *opt = 0;
    }
    char *at = strrchr(tmp, '@');
    if (!at) {
        snprintf(err, errsz, "RDP 格式应为：用户:密码@主机[:端口]");
        return NULL;
    }
    *at = 0;
    char *hostport = at + 1;
    char *colon = strchr(tmp, ':');
    if (!colon) {
        snprintf(err, errsz, "RDP 缺少密码：用户:密码@主机");
        return NULL;
    }
    *colon = 0;
    const char *user = tmp;
    const char *pass = colon + 1;
    if (!*user || !*hostport) {
        snprintf(err, errsz, "RDP 地址不完整");
        return NULL;
    }

    int port = 3389;
    char host[128];
    snprintf(host, sizeof(host), "%s", hostport);
    char *hc = strrchr(host, ':');
    if (hc && hc[1]) {
        int v = atoi(hc + 1);
        if (v > 0 && v < 65536) {
            port = v;
            *hc = 0;
        }
    }

    if (out_w < 320 || out_h < 240) {
        out_w = 1920;
        out_h = 1080;
    }
    out_w &= ~1;
    out_h &= ~1;

    rdp_source *r = calloc(1, sizeof(*r));
    if (!r) {
        snprintf(err, errsz, "out of memory");
        return NULL;
    }
    if (strlen(user) >= sizeof(r->user) ||
        strlen(pass) >= sizeof(r->pass) ||
        strlen(host) >= sizeof(r->host)) {
        snprintf(err, errsz, "RDP 用户名/密码/主机名过长");
        free(r);
        return NULL;
    }
    snprintf(r->host, sizeof(r->host), "%s", host);
    snprintf(r->user, sizeof(r->user), "%s", user);
    snprintf(r->pass, sizeof(r->pass), "%s", pass);
    r->port = port;
    r->out_w = out_w;
    r->out_h = out_h;
    r->running = 1;
    r->cur_visible = 1;
    r->px = out_w / 2;
    r->py = out_h / 2;
    r->fps_t0 = now_s();
    os_mutex_init(&r->lock);
    os_mutex_init(&r->in_lock);

    pthread_once(&g_crypto_once, crypto_init_once);

    {
        char layout[16];
        detect_keyboard_layout(layout, sizeof(layout), &r->rdp_layout);
        fprintf(stderr, "rdp: keyboard layout %s (0x%x)\n",
                layout, r->rdp_layout);
    }

    RDP_CLIENT_ENTRY_POINTS ep;
    memset(&ep, 0, sizeof(ep));
    ep.Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
    ep.Version = RDP_CLIENT_INTERFACE_VERSION;
    ep.GlobalInit = rdp_global_init;
    ep.GlobalUninit = rdp_global_uninit;
    ep.ContextSize = sizeof(hdmi_rdp_context);
    ep.ClientNew = rdp_client_new;
    ep.ClientFree = rdp_client_free;
    ep.ClientStart = rdp_client_start;
    ep.ClientStop = rdp_client_stop;

    r->context = freerdp_client_context_new(&ep);
    if (!r->context) {
        snprintf(err, errsz, "无法创建 FreeRDP 客户端");
        goto fail;
    }
    ((hdmi_rdp_context *)r->context)->src = r;
    r->inst = r->context->instance;
    rdp_audio_install_provider();

    {
        char varg[256], uarg[192], parg[320], sizearg[48], kbdarg[48];
        char soundarg[192];
        snprintf(varg, sizeof(varg), "/v:%s:%d", r->host, r->port);
        snprintf(uarg, sizeof(uarg), "/u:%s", r->user);
        snprintf(parg, sizeof(parg), "/p:%s", r->pass);
        snprintf(sizearg, sizeof(sizearg), "/size:%dx%d", out_w, out_h);
        snprintf(kbdarg, sizeof(kbdarg), "/kbd:layout:0x%x",
                 r->rdp_layout ? r->rdp_layout : 0x409u);
        soundarg[0] = 0;
        if (audio_dev && *audio_dev && strcmp(audio_dev, "off") != 0) {
            if (rdp_audio_make_arg(audio_dev, soundarg, sizeof(soundarg)) == 0)
                fprintf(stderr, "rdp: audio %s\n", soundarg);
            else
                soundarg[0] = 0;
        }

        const char *args[16];
        int argc = 0;
        args[argc++] = "hdmi-rdp";
        args[argc++] = varg;
        args[argc++] = uarg;
        args[argc++] = parg;
        args[argc++] = sizearg;
        args[argc++] = "/sec:nla";
        args[argc++] = "/cert:ignore";
        args[argc++] = "/gfx:AVC444";
        args[argc++] = "/network:lan";
        if (soundarg[0])
            args[argc++] = soundarg;
        else
            args[argc++] = "/audio-mode:2";
        args[argc++] = kbdarg;
        if (use_admin)
            args[argc++] = "+admin";   /* attach to the console session */
        args[argc++] = "-clipboard";
        args[argc++] = "/log-level:OFF";
        char **argv = calloc((size_t)argc + 1, sizeof(char *));
        if (!argv) {
            snprintf(err, errsz, "out of memory");
            goto fail;
        }
        for (int i = 0; i < argc; i++)
            argv[i] = strdup(args[i]);
        int rc = freerdp_client_settings_parse_command_line(
            r->context->settings, argc, argv, FALSE);
        for (int i = 0; i < argc; i++)
            free(argv[i]);
        free(argv);
        if (rc != 0) {
            snprintf(err, errsz, "RDP 参数解析失败（%d）", rc);
            goto fail;
        }
    }

    rdpSettings *st = r->context->settings;
    freerdp_settings_set_uint32(st, FreeRDP_KeyboardLayout, r->rdp_layout);
    freerdp_settings_set_bool(st, FreeRDP_SoftwareGdi, TRUE);
    freerdp_settings_set_bool(st, FreeRDP_FastPathInput, TRUE);
    freerdp_settings_set_bool(st, FreeRDP_FastPathOutput, TRUE);

    /* Keep FreeRDP's default caches and visual flags.  The Windows server
     * tested here accepts AVC444 but rejects AVC444v2 and the extra cache
     * combinations, so the stable baseline is the right default: LAN profile,
     * AVC444, FastPath, and the normal desktop look (themes/animations). */

    if (os_thread_create(&r->th, rdp_thread, r) != 0) {
        snprintf(err, errsz, "cannot start RDP thread");
        goto fail;
    }
    return r;

fail:
    if (r->context)
        freerdp_client_context_free(r->context);
    os_mutex_destroy(&r->lock);
    os_mutex_destroy(&r->in_lock);
    free(r->fb);
    free(r);
    return NULL;
}

void rdp_source_close(rdp_source *r)
{
    if (!r)
        return;
    r->running = 0;
    os_mutex_lock(&r->in_lock);
    r->connected = 0;
    os_mutex_unlock(&r->in_lock);
    if (r->context)
        freerdp_abort_connect_context(r->context);
    os_thread_join(r->th);

    os_mutex_lock(&r->in_lock);
    rdpContext *ctx = r->context;
    r->context = NULL;
    r->inst = NULL;
    os_mutex_unlock(&r->in_lock);
    if (ctx)
        freerdp_client_context_free(ctx);

    free(r->fb);
    free(r->cur_px);
    if (r->sws)
        sws_freeContext(r->sws);
    os_mutex_destroy(&r->lock);
    os_mutex_destroy(&r->in_lock);
    free(r);
}

uint64_t rdp_source_serial(rdp_source *r)
{
    if (!r)
        return 0;
    os_mutex_lock(&r->lock);
    uint64_t s = r->serial;
    os_mutex_unlock(&r->lock);
    return s;
}

int rdp_source_draw(rdp_source *r, uint8_t *dst, int dst_pitch,
                    int scr_w, int scr_h, video_frame_info *info)
{
    memset(info, 0, sizeof(*info));
    if (!r || !dst || scr_w < 2 || scr_h < 2)
        return 0;

    int drawn = 0;
    os_mutex_lock(&r->lock);
    if (r->fb && r->fb_w > 0 && r->fb_h > 0) {
        double dar = (double)r->fb_w / (double)r->fb_h;
        int w = scr_w;
        int h = (int)((double)scr_w / dar);
        if (h > scr_h) {
            h = scr_h;
            w = (int)((double)scr_h * dar);
        }
        w &= ~1;
        h &= ~1;
        if (w < 2) w = 2;
        if (h < 2) h = 2;
        if (w > scr_w) w = scr_w;
        if (h > scr_h) h = scr_h;
        int ox = (scr_w - w) / 2, oy = (scr_h - h) / 2;

        if (w == r->fb_w && h == r->fb_h) {
            for (int y = 0; y < h; y++)
                memcpy(dst + (size_t)(y + oy) * dst_pitch + (size_t)ox * 4,
                       r->fb + (size_t)y * w, (size_t)w * 4);
        } else {
            if (!r->sws || r->sws_fw != r->fb_w || r->sws_fh != r->fb_h ||
                r->sws_w != w || r->sws_h != h) {
                r->sws = sws_getCachedContext(r->sws, r->fb_w, r->fb_h,
                                              AV_PIX_FMT_BGRA, w, h,
                                              AV_PIX_FMT_BGRA,
                                              SWS_FAST_BILINEAR,
                                              NULL, NULL, NULL);
                r->sws_fw = r->fb_w;
                r->sws_fh = r->fb_h;
                r->sws_w = w;
                r->sws_h = h;
            }
            if (r->sws) {
                const uint8_t *src[4] = {(const uint8_t *)r->fb, NULL, NULL, NULL};
                int src_stride[4] = {r->fb_w * 4, 0, 0, 0};
                uint8_t *d[4] = {dst + (size_t)oy * dst_pitch +
                                       (size_t)ox * 4,
                                 NULL, NULL, NULL};
                int dl[4] = {dst_pitch, 0, 0, 0};
                sws_scale(r->sws, src, src_stride, 0, r->fb_h, d, dl);
            }
        }
        blend_cursor_locked(r, (uint32_t *)dst, dst_pitch / 4,
                            ox, oy, w, h);
        info->x = ox;
        info->y = oy;
        info->w = w;
        info->h = h;
        info->src_w = r->fb_w;
        info->src_h = r->fb_h;
        info->serial = r->serial;
        info->stream_fps = r->fps;
        info->pos_s = 0;
        info->dur_s = 0;
        snprintf(info->codec, sizeof(info->codec), "rdp (freerdp)");
        info->online = r->online;
        drawn = 1;
    }
    os_mutex_unlock(&r->lock);
    return drawn;
}

void rdp_source_status(rdp_source *r, char *codec, size_t codecsz,
                       double *fps, int *online, char *err, size_t errsz)
{
    if (!r)
        return;
    os_mutex_lock(&r->lock);
    if (codec)
        snprintf(codec, codecsz, "rdp (freerdp)");
    if (fps)
        *fps = r->fps;
    if (online)
        *online = r->online;
    if (err)
        snprintf(err, errsz, "%s", r->err);
    os_mutex_unlock(&r->lock);
}

void rdp_source_pointer(rdp_source *r, int buttons, int x, int y)
{
    if (!r)
        return;
    int px = (int)((int64_t)x * r->out_w / 65535);
    int py = (int)((int64_t)y * r->out_h / 65535);
    if (px < 0) px = 0;
    if (px >= r->out_w) px = r->out_w - 1;
    if (py < 0) py = 0;
    if (py >= r->out_h) py = r->out_h - 1;

    /* Update the locally composited cursor even while reconnecting. */
    os_mutex_lock(&r->lock);
    if (r->px != px || r->py != py) {
        r->px = px;
        r->py = py;
        int64_t t = now_ms();
        if (t - r->last_ptr_ms >= 15) {
            r->serial++;
            r->last_ptr_ms = t;
        }
    }
    os_mutex_unlock(&r->lock);

    os_mutex_lock(&r->in_lock);
    if (!r->connected || !r->context || !r->context->input) {
        os_mutex_unlock(&r->in_lock);
        return;
    }
    rdpInput *in = r->context->input;
    int changed = r->prev_mask ^ buttons;
    if (changed) {
        fprintf(stderr, "rdp: mouse buttons %d -> %d at %d,%d\n",
                r->prev_mask, buttons, px, py);
        /* Make sure the guest pointer is at the click position first. */
        if (r->last_sent_x != px || r->last_sent_y != py) {
            freerdp_input_send_mouse_event(in, PTR_FLAGS_MOVE,
                                           (UINT16)px, (UINT16)py);
            r->last_sent_x = px;
            r->last_sent_y = py;
        }
        for (int b = 0; b < 3; b++) {
            if (!(changed & (1 << b)))
                continue;
            UINT16 bf = b == 0 ? PTR_FLAGS_BUTTON1 :
                        b == 1 ? PTR_FLAGS_BUTTON3 : PTR_FLAGS_BUTTON2;
            if (buttons & (1 << b))
                bf |= PTR_FLAGS_DOWN;
            BOOL ok = freerdp_input_send_mouse_event(in, bf, (UINT16)px,
                                                     (UINT16)py);
            if (!ok)
                fprintf(stderr, "rdp: button send failed at %d,%d\n", px, py);
        }
        r->prev_mask = buttons;
        r->last_move_ms = now_ms();
    } else {
        /* Gaming mice can report at 1000 Hz; coalesce motion to ~250 Hz. */
        int64_t t = now_ms();
        if (t - r->last_move_ms >= 4) {
            BOOL ok = freerdp_input_send_mouse_event(in, PTR_FLAGS_MOVE,
                                                     (UINT16)px, (UINT16)py);
            static int64_t last_log;
            if (!ok && t - last_log >= 1000) {
                fprintf(stderr, "rdp: move send failed at %d,%d\n", px, py);
                last_log = t;
            }
            r->last_sent_x = px;
            r->last_sent_y = py;
            r->last_move_ms = t;
        }
    }
    os_mutex_unlock(&r->in_lock);
}

void rdp_source_keycode(rdp_source *r, uint16_t code, int down)
{
    if (!r || !code)
        return;
    int valid = 0;
    uint32_t sc = evdev_scancode(code, &valid);
    if (!valid)
        return;

    os_mutex_lock(&r->in_lock);
    if (!r->connected || !r->context || !r->context->input) {
        os_mutex_unlock(&r->in_lock);
        return;
    }
    int repeat = 0;
    if (code < sizeof(r->keydown)) {
        if (down && r->keydown[code])
            repeat = 1;
        r->keydown[code] = down ? 1 : 0;
    }
    BOOL ok = freerdp_input_send_keyboard_event_ex(r->context->input,
                                                   down ? TRUE : FALSE,
                                                   repeat ? TRUE : FALSE, sc);
    if (!ok)
        fprintf(stderr, "rdp: key send failed code=%u\n", (unsigned)code);
    os_mutex_unlock(&r->in_lock);
}

void rdp_source_wheel(rdp_source *r, int delta)
{
    if (!r || !delta)
        return;
    int steps = delta > 0 ? delta : -delta;
    if (steps > 20)
        steps = 20;   /* safety limit for a single evdev event */
    os_mutex_lock(&r->in_lock);
    if (r->connected && r->context && r->context->input) {
        /* RDP wheel rotation is in 120-unit notches (WHEEL_DELTA), not one
         * unit per evdev click.  Sending 1 made Windows scroll ~120x slower. */
        for (int i = 0; i < steps; i++) {
            UINT16 flags = PTR_FLAGS_WHEEL;
            if (delta > 0)
                flags |= 0x78;   /* +120 */
            else
                flags |= PTR_FLAGS_WHEEL_NEGATIVE | (0x100 - 0x78);
            if (!freerdp_input_send_mouse_event(r->context->input, flags, 0, 0))
                fprintf(stderr, "rdp: wheel send failed\n");
        }
        fprintf(stderr, "rdp: wheel %d (x120)\n", delta);
    }
    os_mutex_unlock(&r->in_lock);
}
