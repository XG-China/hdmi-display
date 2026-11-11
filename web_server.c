/*
 * web_server.c - small HTTP server + MJPEG preview + admin panel.
 *
 *   GET /                      static panel (web_dir/index.html)
 *   GET /style.css, /app.js    static assets
 *   GET /api/state             JSON snapshot
 *   GET /api/apply?out=N&type=T&arg=...&w=..&h=..
 *   GET /api/disable?out=N
 *   GET /api/quit
 *   GET /preview/N.mjpg        live MJPEG preview
 *   GET /snapshot/N.jpg        single JPEG
 *
 * The same handler listens on an optional Unix socket for the fnOS gateway
 * (fygo-cinema style): the gateway forwards /app/hdmi-display/... and the
 * configured prefix is stripped before routing.
 */
#define _GNU_SOURCE

#include "web_server.h"
#include "os_thread.h"
#include "display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

typedef int sock_t;
#define SOCK_INVALID (-1)
#define sock_close close

#define PREVIEW_MAX_W 640
#define PREVIEW_MAX_H 400
#define MAX_REQUEST   8192
#define MAX_WEB_FILE  (8 * 1024 * 1024)

static struct {
    volatile int running;
    sock_t listen_fd;
    sock_t gateway_fd;
    os_thread_t thread;
    os_thread_t gateway_thread;
    int thread_started;
    int gateway_thread_started;
    web_ops ops;
    void *ud;
    char prefix[128];
    char web_dir[1024];
    char version[24];
    char socket_path[256];
} g_web;

static volatile long g_clients;

static int send_all(sock_t fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        int chunk = len > 65536 ? 65536 : (int)len;
        int n = (int)send(fd, p, chunk, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void set_timeouts(sock_t fd, int ms)
{
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static void http_reply_ex(sock_t fd, int code, const char *ctype,
                          const void *body, size_t len, const char *cache)
{
    const char *reason = code == 200 ? "OK" :
                         code == 401 ? "Unauthorized" :
                         code == 404 ? "Not Found" :
                         code == 503 ? "Service Unavailable" : "Error";
    char hdr[640];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: %s\r\n"
                     "Connection: close\r\n\r\n",
                     code, reason, ctype, len,
                     cache && *cache ? cache : "no-store");
    if (n > 0)
        send_all(fd, hdr, (size_t)n);
    if (body && len)
        send_all(fd, body, len);
}

static void http_reply(sock_t fd, int code, const char *ctype,
                       const void *body, size_t len)
{
    http_reply_ex(fd, code, ctype, body, len, "no-store");
}

static void http_text(sock_t fd, int code, const char *ctype, const char *body)
{
    http_reply(fd, code, ctype, body, strlen(body));
}

static void http_redirect(sock_t fd, const char *location)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 301 Moved Permanently\r\n"
                     "Location: %s\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n\r\n", location);
    if (n > 0)
        send_all(fd, hdr, (size_t)n);
}

/* Small distribution files live next to the running executable (app/bin). */
static void serve_app_file(sock_t fd, const char *name, const char *ctype)
{
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        http_text(fd, 404, "text/plain", "agent path unavailable");
        return;
    }
    exe[n] = 0;
    char *slash = strrchr(exe, '/');
    if (!slash) {
        http_text(fd, 404, "text/plain", "agent path unavailable");
        return;
    }
    *slash = 0;

    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", exe, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        http_text(fd, 404, "text/plain; charset=utf-8",
                  "未找到代理文件，请重新安装应用。");
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        http_text(fd, 500, "text/plain", "read error");
        return;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > 64 * 1024 * 1024) {
        fclose(f);
        http_text(fd, 500, "text/plain", "bad file size");
        return;
    }
    rewind(f);
    void *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        http_text(fd, 500, "text/plain", "read error");
        return;
    }
    fclose(f);
    http_reply(fd, 200, ctype, buf, (size_t)sz);
    free(buf);
}

/* ---------- static admin panel ---------- */

static const char *mime_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return NULL;
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".css")  == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".js")   == 0) return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    if (strcmp(dot, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".jpg")  == 0) return "image/jpeg";
    if (strcmp(dot, ".ico")  == 0) return "image/x-icon";
    if (strcmp(dot, ".woff2") == 0) return "font/woff2";
    return NULL;
}

static void hash_file(uint64_t *h, const char *name)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", g_web.web_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            *h ^= buf[i];
            *h *= 1099511628211ULL;
        }
    }
    fclose(f);
}

static void compute_web_version(void)
{
    uint64_t h = 1469598103934665603ULL;
    hash_file(&h, "style.css");
    hash_file(&h, "app.js");
    snprintf(g_web.version, sizeof(g_web.version), "%016llx",
             (unsigned long long)h);
}

static void serve_static(sock_t fd, const char *name)
{
    if (!g_web.web_dir[0]) {
        http_text(fd, 500, "text/plain; charset=utf-8",
                  "web assets are not installed (start with --web-dir)");
        return;
    }
    if (!name || !*name)
        name = "index.html";
    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
        http_text(fd, 404, "text/plain", "bad path");
        return;
    }
    const char *ctype = mime_for(name);
    if (!ctype) {
        http_text(fd, 404, "text/plain", "not found");
        return;
    }

    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", g_web.web_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        http_text(fd, 404, "text/plain", "not found");
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        http_text(fd, 500, "text/plain", "read error");
        return;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > MAX_WEB_FILE) {
        fclose(f);
        http_text(fd, 500, "text/plain", "bad file size");
        return;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf || (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz)) {
        free(buf);
        fclose(f);
        http_text(fd, 500, "text/plain", "read error");
        return;
    }
    fclose(f);
    buf[sz] = 0;

    int is_index = strcmp(name, "index.html") == 0;
    if (is_index) {
        static const char marker[] = "?v=__V__";
        const size_t ml = sizeof(marker) - 1;
        /* Each marker expands by at most 12 bytes; 1.5x is ample for the
         * panel and the bounds checks below fall back to the original file
         * if anything unexpected happens. */
        size_t cap = (size_t)sz + (size_t)sz / 2 + 64;
        char *out = malloc(cap);
        if (out) {
            size_t o = 0;
            const char *p = buf, *end = buf + sz;
            int ok = 1;
            while (p < end) {
                const char *m = memmem(p, (size_t)(end - p), marker, ml);
                const char *seg_end = m ? m : end;
                size_t seg = (size_t)(seg_end - p);
                if (o + seg >= cap) {
                    ok = 0;
                    break;
                }
                memcpy(out + o, p, seg);
                o += seg;
                if (!m)
                    break;
                size_t rem = cap - o;
                int n = snprintf(out + o, rem, "?v=%s", g_web.version);
                if (n < 0 || (size_t)n >= rem) {
                    ok = 0;
                    break;
                }
                o += (size_t)n;
                p = m + ml;
            }
            if (ok) {
                free(buf);
                buf = out;
                sz = (long)o;
            } else {
                free(out);
            }
        }
    }

    http_reply_ex(fd, 200, ctype, buf, (size_t)sz,
                  is_index ? "no-store"
                           : "public, max-age=31536000, immutable");
    free(buf);
}

static void url_decode(char *s)
{
    char *d = s;
    for (const char *p = s; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            int hi = p[1] >= 'a' ? p[1] - 'a' + 10 : p[1] >= 'A' ? p[1] - 'A' + 10 : p[1] - '0';
            int lo = p[2] >= 'a' ? p[2] - 'a' + 10 : p[2] >= 'A' ? p[2] - 'A' + 10 : p[2] - '0';
            *d++ = (char)((hi << 4) | lo);
            p += 2;
        } else if (*p == '+') {
            *d++ = ' ';
        } else {
            *d++ = *p;
        }
    }
    *d = 0;
}

static int get_param(const char *query, const char *name, char *out, size_t outsz)
{
    if (!query || !*query)
        return 0;
    size_t nl = strlen(name);
    const char *p = query;
    while (p && *p) {
        if (strncmp(p, name, nl) == 0 && p[nl] == '=') {
            const char *v = p + nl + 1;
            const char *e = strchr(v, '&');
            size_t len = e ? (size_t)(e - v) : strlen(v);
            if (len >= outsz)
                len = outsz - 1;
            memcpy(out, v, len);
            out[len] = 0;
            url_decode(out);
            return 1;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return 0;
}

static int authorized(const char *query)
{
    if (!g_web.ops.token || !*g_web.ops.token)
        return 1;
    char tok[256];
    if (!get_param(query, "token", tok, sizeof(tok)))
        return 0;
    return strcmp(tok, g_web.ops.token) == 0;
}

/* ---------- MJPEG ---------- */

typedef struct {
    AVCodecContext *enc;
    struct SwsContext *sws;
    AVFrame *frame;
    AVPacket *pkt;
    int w, h;
    int64_t pts;
} mjpeg_t;

static void mjpeg_close(mjpeg_t *m)
{
    av_packet_free(&m->pkt);
    av_frame_free(&m->frame);
    avcodec_free_context(&m->enc);
    if (m->sws) {
        sws_freeContext(m->sws);
        m->sws = NULL;
    }
    memset(m, 0, sizeof(*m));
}

static int mjpeg_open(mjpeg_t *m, int w, int h)
{
    memset(m, 0, sizeof(*m));
    w &= ~1;
    h &= ~1;
    if (w < 16 || h < 16)
        return -1;

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec)
        return -1;

    m->enc = avcodec_alloc_context3(codec);
    if (!m->enc)
        goto fail;
    m->enc->width = w;
    m->enc->height = h;
    m->enc->pix_fmt = AV_PIX_FMT_YUV420P;
    m->enc->color_range = AVCOL_RANGE_JPEG;
    m->enc->time_base.num = 1;
    m->enc->time_base.den = 15;
    m->enc->flags |= AV_CODEC_FLAG_QSCALE;
    m->enc->global_quality = FF_QP2LAMBDA * 7;
    m->enc->max_b_frames = 0;
    if (avcodec_open2(m->enc, codec, NULL) < 0)
        goto fail;

    m->frame = av_frame_alloc();
    m->pkt = av_packet_alloc();
    if (!m->frame || !m->pkt)
        goto fail;
    m->frame->format = m->enc->pix_fmt;
    m->frame->width = w;
    m->frame->height = h;
    if (av_frame_get_buffer(m->frame, 32) < 0)
        goto fail;

    m->sws = sws_getContext(w, h, AV_PIX_FMT_RGB24, w, h, AV_PIX_FMT_YUV420P,
                            SWS_BILINEAR, NULL, NULL, NULL);
    if (!m->sws)
        goto fail;

    m->w = w;
    m->h = h;
    return 0;

fail:
    mjpeg_close(m);
    return -1;
}

static int mjpeg_encode(mjpeg_t *m, const uint8_t *rgb)
{
    const uint8_t *src[4] = { rgb, NULL, NULL, NULL };
    int src_lines[4] = { m->w * 3, 0, 0, 0 };

    sws_scale(m->sws, src, src_lines, 0, m->h, m->frame->data, m->frame->linesize);
    m->frame->pts = m->pts++;
    if (avcodec_send_frame(m->enc, m->frame) < 0)
        return -1;
    return avcodec_receive_packet(m->enc, m->pkt) == 0 ? 1 : 0;
}

static void serve_mjpeg(sock_t fd, const char *name)
{
    uint8_t *rgb = malloc((size_t)PREVIEW_MAX_W * PREVIEW_MAX_H * 3);
    if (!rgb) {
        http_text(fd, 500, "text/plain", "out of memory");
        return;
    }

    int w = 0, h = 0;
    uint64_t serial = 0;
    int ready = 0;
    for (int i = 0; i < 50 && g_web.running; i++) {
        if (g_web.ops.copy_preview(name, rgb,
                                   (size_t)PREVIEW_MAX_W * PREVIEW_MAX_H * 3,
                                   &w, &h, &serial, g_web.ud) == 0 &&
            w > 0 && h > 0) {
            ready = 1;
            break;
        }
        disp_sleep_ms(100);
    }
    if (!ready) {
        http_text(fd, 503, "text/plain", "preview not ready");
        free(rgb);
        return;
    }

    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n";
    if (send_all(fd, hdr, strlen(hdr)) != 0) {
        free(rgb);
        return;
    }

    mjpeg_t m;
    memset(&m, 0, sizeof(m));
    uint64_t last = serial;   /* encode the frame we already have once */

    while (g_web.running) {
        if (g_web.ops.copy_preview(name, rgb,
                                   (size_t)PREVIEW_MAX_W * PREVIEW_MAX_H * 3,
                                   &w, &h, &serial, g_web.ud) != 0) {
            disp_sleep_ms(100);
            last = 0;
            continue;
        }
        if (w <= 0 || h <= 0) {
            disp_sleep_ms(100);
            continue;
        }
        if (m.enc && (w != m.w || h != m.h)) {
            mjpeg_close(&m);
            last = 0;
        }
        if (!m.enc && mjpeg_open(&m, w, h) != 0) {
            disp_sleep_ms(200);
            continue;
        }
        if (serial == last) {
            disp_sleep_ms(20);
            continue;
        }
        last = serial;

        int r = mjpeg_encode(&m, rgb);
        if (r > 0) {
            char part[160];
            int n = snprintf(part, sizeof(part),
                             "--frame\r\nContent-Type: image/jpeg\r\n"
                             "Content-Length: %d\r\n\r\n", m.pkt->size);
            if (n <= 0 || send_all(fd, part, (size_t)n) != 0 ||
                send_all(fd, m.pkt->data, (size_t)m.pkt->size) != 0 ||
                send_all(fd, "\r\n", 2) != 0) {
                av_packet_unref(m.pkt);
                break;
            }
            av_packet_unref(m.pkt);
        }
        disp_sleep_ms(20);
    }

    mjpeg_close(&m);
    free(rgb);
}

static void serve_snapshot(sock_t fd, const char *name)
{
    uint8_t *rgb = malloc((size_t)PREVIEW_MAX_W * PREVIEW_MAX_H * 3);
    if (!rgb) {
        http_text(fd, 500, "text/plain", "out of memory");
        return;
    }
    int w = 0, h = 0;
    uint64_t serial = 0;
    if (g_web.ops.copy_preview(name, rgb,
                               (size_t)PREVIEW_MAX_W * PREVIEW_MAX_H * 3,
                               &w, &h, &serial, g_web.ud) != 0) {
        free(rgb);
        http_text(fd, 503, "text/plain", "preview not ready");
        return;
    }

    mjpeg_t m;
    if (mjpeg_open(&m, w, h) != 0) {
        free(rgb);
        http_text(fd, 500, "text/plain", "encoder init failed");
        return;
    }
    int r = mjpeg_encode(&m, rgb);
    if (r > 0) {
        http_reply(fd, 200, "image/jpeg", m.pkt->data, (size_t)m.pkt->size);
        av_packet_unref(m.pkt);
    } else {
        http_text(fd, 500, "text/plain", "encode failed");
    }
    mjpeg_close(&m);
    free(rgb);
}

static int path_name(const char *path, const char *prefix, const char *suffix,
                     char *out, size_t outsz)
{
    size_t pl = strlen(prefix), sl = strlen(suffix), len = strlen(path);
    if (len <= pl + sl)
        return -1;
    if (strncmp(path, prefix, pl) != 0)
        return -1;
    if (strcmp(path + len - sl, suffix) != 0)
        return -1;
    size_t n = len - pl - sl;
    if (n >= outsz)
        return -1;
    memcpy(out, path + pl, n);
    out[n] = 0;
    url_decode(out);
    return 0;
}

/* ---------- request handling ---------- */

static void handle_client(sock_t fd, int gateway)
{
    char *req = malloc(MAX_REQUEST);
    if (!req) {
        sock_close(fd);
        return;
    }
    int len = 0;
    while (len < MAX_REQUEST - 1) {
        int n = (int)recv(fd, req + len, MAX_REQUEST - 1 - len, 0);
        if (n <= 0)
            break;
        len += n;
        req[len] = 0;
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    if (len <= 0) {
        free(req);
        sock_close(fd);
        return;
    }

    char method[8] = {0};
    char path[2048] = {0};
    if (sscanf(req, "%7s %2047s", method, path) != 2) {
        http_text(fd, 400, "text/plain", "bad request");
        free(req);
        sock_close(fd);
        return;
    }
    free(req);

    char *query = strchr(path, '?');
    if (query) {
        *query = 0;
        query++;
    }

    /* fnOS gateway requests carry /app/<app>/...; strip the prefix. */
    int prefix_exact = 0;
    if (g_web.prefix[0]) {
        size_t pl = strlen(g_web.prefix);
        if (strncmp(path, g_web.prefix, pl) == 0 &&
            (path[pl] == '/' || path[pl] == 0)) {
            if (path[pl] == 0)
                prefix_exact = 1;
            memmove(path, path + pl, strlen(path + pl) + 1);
            if (!path[0])
                snprintf(path, sizeof(path), "/");
        }
    }
    if (prefix_exact) {
        char loc[256];
        snprintf(loc, sizeof(loc), "%s/", g_web.prefix);
        http_redirect(fd, loc);
        sock_close(fd);
        return;
    }

    if (strcmp(path, "/favicon.ico") == 0) {
        http_reply(fd, 204, "text/plain", NULL, 0);
        sock_close(fd);
        return;
    }

    /* The gateway is already administrator-only; direct TCP keeps the token. */
    if (!gateway && !authorized(query)) {
        http_text(fd, 401, "text/plain; charset=utf-8",
                  "unauthorized: open /?token=YOUR_TOKEN");
        sock_close(fd);
        return;
    }

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        serve_static(fd, "index.html");
    } else if (strcmp(path, "/style.css") == 0 ||
               strcmp(path, "/app.js") == 0) {
        serve_static(fd, path + 1);
    } else if (strcmp(path, "/api/state") == 0) {
        enum { STATE_JSON_CAP = 65536 };
        char *json = malloc(STATE_JSON_CAP);
        if (!json) {
            http_text(fd, 500, "text/plain", "out of memory");
        } else {
            json[0] = 0;
            g_web.ops.state_json(json, STATE_JSON_CAP, g_web.ud);
            http_text(fd, 200, "application/json; charset=utf-8", json);
            free(json);
        }
    } else if (strcmp(path, "/api/apply") == 0) {
        char out[64] = "", type[32] = "", arg[1200] = "", ws[16] = "", hs[16] = "";
        int w = 0, h = 0;
        if (!get_param(query, "out", out, sizeof(out)) || !out[0]) {
            http_text(fd, 400, "application/json", "{\"ok\":false,\"error\":\"bad out\"}");
        } else {
            get_param(query, "type", type, sizeof(type));
            get_param(query, "arg", arg, sizeof(arg));
            if (get_param(query, "w", ws, sizeof(ws)))
                w = atoi(ws);
            if (get_param(query, "h", hs, sizeof(hs)))
                h = atoi(hs);
            if (!type[0])
                snprintf(type, sizeof(type), "media");
            if (w < 0) w = 0;
            if (h < 0) h = 0;
            if (g_web.ops.apply(out, type, arg, w, h, g_web.ud) != 0)
                http_text(fd, 400, "application/json", "{\"ok\":false,\"error\":\"queue full\"}");
            else
                http_text(fd, 200, "application/json", "{\"ok\":true}");
        }
    } else if (strcmp(path, "/api/disable") == 0) {
        char out[64] = "";
        if (!get_param(query, "out", out, sizeof(out)) || !out[0]) {
            http_text(fd, 400, "application/json", "{\"ok\":false,\"error\":\"bad out\"}");
        } else if (g_web.ops.disable(out, g_web.ud) != 0) {
            http_text(fd, 400, "application/json", "{\"ok\":false,\"error\":\"queue full\"}");
        } else {
            http_text(fd, 200, "application/json", "{\"ok\":true}");
        }
    } else if (strcmp(path, "/api/input/bind") == 0) {
        char id[256] = "", out[64] = "";
        if (!get_param(query, "id", id, sizeof(id)) || !id[0]) {
            http_text(fd, 400, "application/json", "{\"ok\":false,\"error\":\"bad id\"}");
        } else {
            get_param(query, "out", out, sizeof(out));
            if (!g_web.ops.bind_input ||
                g_web.ops.bind_input(id, out, g_web.ud) != 0)
                http_text(fd, 400, "application/json", "{\"ok\":false,\"error\":\"bind failed\"}");
            else
                http_text(fd, 200, "application/json", "{\"ok\":true}");
        }
    } else if (strcmp(path, "/api/quit") == 0) {
        http_text(fd, 200, "application/json", "{\"ok\":true}");
        g_web.ops.request_quit(g_web.ud);
    } else if (strcmp(path, "/rdp/enable-rdp.ps1") == 0) {
        serve_app_file(fd, "enable-rdp.ps1", "text/plain; charset=utf-8");
    } else if (strncmp(path, "/preview/", 9) == 0) {
        char name[96];
        if (path_name(path, "/preview/", ".mjpg", name, sizeof(name)) != 0)
            http_text(fd, 404, "text/plain", "bad preview path");
        else
            serve_mjpeg(fd, name);
    } else if (strncmp(path, "/snapshot/", 10) == 0) {
        char name[96];
        if (path_name(path, "/snapshot/", ".jpg", name, sizeof(name)) != 0)
            http_text(fd, 404, "text/plain", "bad snapshot path");
        else
            serve_snapshot(fd, name);
    } else {
        serve_static(fd, path + 1);
    }

    sock_close(fd);
}

typedef struct {
    sock_t fd;
    int gateway;
} listener_arg;

typedef struct {
    sock_t fd;
    int gateway;
} client_arg;

static void *client_thread(void *arg)
{
    client_arg *c = arg;
    handle_client(c->fd, c->gateway);
    free(c);
    g_clients--;
    return NULL;
}

static void *accept_thread(void *arg)
{
    listener_arg *l = arg;
    sock_t lfd = l->fd;
    int gateway = l->gateway;
    free(l);

    while (g_web.running) {
        sock_t fd = accept(lfd, NULL, NULL);
        if (fd == SOCK_INVALID) {
            if (!g_web.running)
                break;
            disp_sleep_ms(50);
            continue;
        }
        set_timeouts(fd, 15000);
        /* Preview streams keep one thread each; cap them so a burst of
         * clients cannot exhaust the wall process. */
        if (g_clients >= 32) {
            sock_close(fd);
            continue;
        }
        client_arg *c = malloc(sizeof(*c));
        if (!c) {
            sock_close(fd);
            continue;
        }
        c->fd = fd;
        c->gateway = gateway;
        g_clients++;
        os_thread_t t;
        if (os_thread_create(&t, client_thread, c) == 0) {
            os_thread_detach(t);
        } else {
            g_clients--;
            free(c);
            sock_close(fd);
        }
    }
    return NULL;
}

static sock_t listen_tcp(int port, const char *bind_addr, char *err, size_t errsz)
{
    if (port <= 0 || port > 65535) {
        snprintf(err, errsz, "invalid web port %d", port);
        return SOCK_INVALID;
    }
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == SOCK_INVALID) {
        snprintf(err, errsz, "socket() failed");
        return SOCK_INVALID;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind_addr && *bind_addr && strcmp(bind_addr, "0.0.0.0") != 0) {
        if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1) {
            snprintf(err, errsz, "invalid bind address %s", bind_addr);
            sock_close(fd);
            return SOCK_INVALID;
        }
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        snprintf(err, errsz, "bind %s:%d failed (port in use?)",
                 bind_addr ? bind_addr : "0.0.0.0", port);
        sock_close(fd);
        return SOCK_INVALID;
    }
    if (listen(fd, 16) != 0) {
        snprintf(err, errsz, "listen failed");
        sock_close(fd);
        return SOCK_INVALID;
    }
    return fd;
}

static sock_t listen_unix(const char *path, char *err, size_t errsz)
{
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        snprintf(err, errsz, "gateway socket path too long");
        return SOCK_INVALID;
    }
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);

    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            snprintf(err, errsz, "%s exists and is not a socket", path);
            return SOCK_INVALID;
        }
        unlink(path);
    }

    sock_t fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == SOCK_INVALID) {
        snprintf(err, errsz, "unix socket() failed");
        return SOCK_INVALID;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        snprintf(err, errsz, "bind %s failed: %s", path, strerror(errno));
        sock_close(fd);
        return SOCK_INVALID;
    }
    chmod(path, 0600);
    if (listen(fd, 16) != 0) {
        snprintf(err, errsz, "unix listen failed");
        sock_close(fd);
        unlink(path);
        return SOCK_INVALID;
    }
    return fd;
}

int web_start(const web_config *cfg, const web_ops *ops, void *ud,
              char *err, size_t errsz)
{
    if (!cfg) {
        snprintf(err, errsz, "missing web config");
        return -1;
    }
    if (!ops || !ops->state_json || !ops->apply || !ops->disable ||
        !ops->copy_preview) {
        snprintf(err, errsz, "incomplete web callbacks");
        return -1;
    }

    int have_socket = cfg->socket_path && *cfg->socket_path;
    if ((cfg->port <= 0 || cfg->port > 65535) && !have_socket) {
        snprintf(err, errsz, "no web listener configured");
        return -1;
    }

    memset(&g_web, 0, sizeof(g_web));
    g_web.ops = *ops;
    g_web.ud = ud;
    g_web.listen_fd = SOCK_INVALID;
    g_web.gateway_fd = SOCK_INVALID;
    if (cfg->prefix)
        snprintf(g_web.prefix, sizeof(g_web.prefix), "%s", cfg->prefix);
    if (cfg->web_dir)
        snprintf(g_web.web_dir, sizeof(g_web.web_dir), "%s", cfg->web_dir);
    if (have_socket)
        snprintf(g_web.socket_path, sizeof(g_web.socket_path), "%s", cfg->socket_path);
    compute_web_version();

    signal(SIGPIPE, SIG_IGN);

    if (cfg->port > 0) {
        g_web.listen_fd = listen_tcp(cfg->port, cfg->bind_addr, err, errsz);
        if (g_web.listen_fd == SOCK_INVALID) {
            /* A busy port must not take the gateway panel down with it. */
            fprintf(stderr, "web: %s\n", err);
        }
    }
    if (have_socket) {
        g_web.gateway_fd = listen_unix(g_web.socket_path, err, errsz);
        if (g_web.gateway_fd == SOCK_INVALID)
            fprintf(stderr, "web: %s\n", err);
    }
    if (g_web.listen_fd == SOCK_INVALID && g_web.gateway_fd == SOCK_INVALID) {
        if (!err[0])
            snprintf(err, errsz, "no web listener could be started");
        return -1;
    }

    g_web.running = 1;

    if (g_web.listen_fd != SOCK_INVALID) {
        listener_arg *l = malloc(sizeof(*l));
        if (!l) {
            snprintf(err, errsz, "out of memory");
            goto fail;
        }
        l->fd = g_web.listen_fd;
        l->gateway = 0;
        if (os_thread_create(&g_web.thread, accept_thread, l) != 0) {
            free(l);
            snprintf(err, errsz, "cannot create web thread");
            goto fail;
        }
        g_web.thread_started = 1;
    }
    if (g_web.gateway_fd != SOCK_INVALID) {
        listener_arg *l = malloc(sizeof(*l));
        if (!l) {
            snprintf(err, errsz, "out of memory");
            goto fail;
        }
        l->fd = g_web.gateway_fd;
        l->gateway = 1;
        if (os_thread_create(&g_web.gateway_thread, accept_thread, l) != 0) {
            free(l);
            snprintf(err, errsz, "cannot create gateway thread");
            goto fail;
        }
        g_web.gateway_thread_started = 1;
    }
    return 0;

fail:
    g_web.running = 0;
    if (g_web.listen_fd != SOCK_INVALID) {
        shutdown(g_web.listen_fd, SHUT_RDWR);
        sock_close(g_web.listen_fd);
        g_web.listen_fd = SOCK_INVALID;
    }
    if (g_web.gateway_fd != SOCK_INVALID) {
        shutdown(g_web.gateway_fd, SHUT_RDWR);
        sock_close(g_web.gateway_fd);
        g_web.gateway_fd = SOCK_INVALID;
    }
    if (g_web.thread_started) {
        os_thread_join(g_web.thread);
        g_web.thread_started = 0;
    }
    if (g_web.gateway_thread_started) {
        os_thread_join(g_web.gateway_thread);
        g_web.gateway_thread_started = 0;
    }
    if (g_web.socket_path[0])
        unlink(g_web.socket_path);
    return -1;
}

void web_stop(void)
{
    if (!g_web.running && !g_web.thread_started && !g_web.gateway_thread_started)
        return;
    g_web.running = 0;
    if (g_web.listen_fd != SOCK_INVALID) {
        shutdown(g_web.listen_fd, SHUT_RDWR);
        sock_close(g_web.listen_fd);
        g_web.listen_fd = SOCK_INVALID;
    }
    if (g_web.gateway_fd != SOCK_INVALID) {
        shutdown(g_web.gateway_fd, SHUT_RDWR);
        sock_close(g_web.gateway_fd);
        g_web.gateway_fd = SOCK_INVALID;
    }
    if (g_web.thread_started) {
        os_thread_join(g_web.thread);
        g_web.thread_started = 0;
    }
    if (g_web.gateway_thread_started) {
        os_thread_join(g_web.gateway_thread);
        g_web.gateway_thread_started = 0;
    }
    if (g_web.socket_path[0])
        unlink(g_web.socket_path);
    for (int i = 0; i < 40 && g_clients > 0; i++)
        disp_sleep_ms(50);
    disp_sleep_ms(150);
}
