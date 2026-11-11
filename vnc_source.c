/*
 * vnc_source.c - minimal RFB / VNC client.
 *
 * Supports RFB 3.3 and 3.8, security type "None", Raw + CopyRect encodings
 * and DesktopSize.  Enough to show a VM console (QEMU/Proxmox/libvirt),
 * x11vnc, TigerVNC or any standard VNC server on an HDMI output.
 */
#define _GNU_SOURCE

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define sock_close close

#include "vnc_source.h"
#include "os_thread.h"
#include "display.h"

#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct vnc_source {
    os_thread_t th;
    os_mutex_t lock;
    os_mutex_t wlock;   /* serializes socket writes (session + input injection) */
    volatile int running;

    char host[256];
    int  port;
    int  is_unix;
    int  out_w, out_h;
    sock_t sock;

    uint32_t *fb;
    int fb_w, fb_h;
    uint8_t *scratch;
    size_t scratch_cap;

    uint64_t serial;
    int online;
    int64_t last_rx;
    char err[256];

    struct SwsContext *sws;   /* display thread only */
    int sws_fw, sws_fh, sws_w, sws_h;
};

static void set_err(vnc_source *v, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    os_mutex_lock(&v->lock);
    snprintf(v->err, sizeof(v->err), "%s", buf);
    os_mutex_unlock(&v->lock);
    if (v->is_unix)
        fprintf(stderr, "vnc %s: %s\n", v->host, buf);
    else
        fprintf(stderr, "vnc %s:%d: %s\n", v->host, v->port, buf);
}

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int read_full(vnc_source *v, void *buf, size_t n)
{
    unsigned char *p = buf;
    while (n > 0 && v->running) {
        int r = (int)recv(v->sock, (char *)p, (int)n, 0);
        if (r > 0) {
            p += r;
            n -= (size_t)r;
            continue;
        }
        if (r == 0)
            return -1;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            continue;
        return -1;
    }
    return n == 0 ? 0 : -1;
}

static int write_full(vnc_source *v, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    int rc = 0;
    os_mutex_lock(&v->wlock);
    while (n > 0 && v->running) {
        int r = (int)send(v->sock, (const char *)p, (int)n, 0);
        if (r > 0) {
            p += r;
            n -= (size_t)r;
            continue;
        }
        if (r == 0) {
            rc = -1;
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            continue;
        rc = -1;
        break;
    }
    if (n != 0)
        rc = -1;
    os_mutex_unlock(&v->wlock);
    return rc;
}

static sock_t unix_connect(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return SOCK_INVALID;
    struct sockaddr_un sa;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        sock_close(fd);
        return SOCK_INVALID;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%.*s",
             (int)sizeof(sa.sun_path) - 1, path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        sock_close(fd);
        return SOCK_INVALID;
    }
    return fd;
}

static sock_t tcp_connect(const char *host, int port)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return SOCK_INVALID;

    sock_t fd = SOCK_INVALID;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == SOCK_INVALID)
            continue;
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0)
            break;
        sock_close(fd);
        fd = SOCK_INVALID;
    }
    freeaddrinfo(res);
    return fd;
}

static void set_socket_timeout(sock_t fd, int ms)
{
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int fb_resize(vnc_source *v, int w, int h)
{
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return -1;
    uint32_t *nf = realloc(v->fb, (size_t)w * (size_t)h * sizeof(uint32_t));
    if (!nf)
        return -1;
    v->fb = nf;
    v->fb_w = w;
    v->fb_h = h;
    memset(v->fb, 0, (size_t)w * (size_t)h * sizeof(uint32_t));
    return 0;
}

static int send_fb_update(vnc_source *v, int incremental)
{
    unsigned char msg[10];
    msg[0] = 3;
    msg[1] = (unsigned char)incremental;
    msg[2] = 0; msg[3] = 0;
    msg[4] = 0; msg[5] = 0;
    msg[6] = (unsigned char)(v->fb_w >> 8);
    msg[7] = (unsigned char)(v->fb_w & 0xff);
    msg[8] = (unsigned char)(v->fb_h >> 8);
    msg[9] = (unsigned char)(v->fb_h & 0xff);
    return write_full(v, msg, sizeof(msg));
}

static int handshake(vnc_source *v, char *err, size_t errsz)
{
    unsigned char ver[13];
    if (read_full(v, ver, 12) != 0) {
        snprintf(err, errsz, "no RFB greeting");
        return -1;
    }
    ver[12] = 0;
    int maj = 0, min = 0;
    if (sscanf((char *)ver, "RFB %3d.%3d", &maj, &min) != 2) {
        snprintf(err, errsz, "bad RFB greeting");
        return -1;
    }

    const char *reply = (maj > 3 || (maj == 3 && min >= 8)) ?
                        "RFB 003.008\n" : "RFB 003.003\n";
    if (write_full(v, reply, 12) != 0) {
        snprintf(err, errsz, "cannot send version");
        return -1;
    }

    if (maj > 3 || (maj == 3 && min >= 7)) {
        unsigned char n = 0;
        if (read_full(v, &n, 1) != 0 || n == 0) {
            snprintf(err, errsz, "server rejected connection");
            return -1;
        }
        unsigned char types[64];
        if (n > sizeof(types))
            n = sizeof(types);
        if (read_full(v, types, n) != 0) {
            snprintf(err, errsz, "cannot read security types");
            return -1;
        }
        int none = 0;
        for (int i = 0; i < n; i++)
            if (types[i] == 1)
                none = 1;
        if (!none) {
            snprintf(err, errsz, "VNC authentication required (password auth "
                                 "is not supported)");
            return -1;
        }
        unsigned char chosen = 1;
        if (write_full(v, &chosen, 1) != 0) {
            snprintf(err, errsz, "cannot select security type");
            return -1;
        }
        /* RFB 3.8 sends a 4-byte SecurityResult even for "None"; reading it
         * as part of ServerInit shifts the header and yields 0x0. */
        unsigned char sres[4];
        if (read_full(v, sres, 4) != 0 || be32(sres) != 0) {
            snprintf(err, errsz, "VNC security handshake failed");
            return -1;
        }
    } else {
        unsigned char sec[4];
        if (read_full(v, sec, 4) != 0) {
            snprintf(err, errsz, "cannot read security type");
            return -1;
        }
        uint32_t s = be32(sec);
        if (s == 0) {
            unsigned char lenb[4];
            if (read_full(v, lenb, 4) == 0) {
                uint32_t l = be32(lenb);
                if (l > 0 && l < 1024) {
                    char reason[1024];
                    read_full(v, reason, l);
                }
            }
            snprintf(err, errsz, "server refused connection");
            return -1;
        }
        if (s != 1) {
            snprintf(err, errsz, "VNC authentication required (unsupported)");
            return -1;
        }
    }

    unsigned char shared = 1;
    if (write_full(v, &shared, 1) != 0) {
        snprintf(err, errsz, "cannot send ClientInit");
        return -1;
    }

    unsigned char si[24];
    if (read_full(v, si, 24) != 0) {
        snprintf(err, errsz, "cannot read ServerInit");
        return -1;
    }
    int w = (si[0] << 8) | si[1];
    int h = (si[2] << 8) | si[3];
    uint32_t name_len = be32(si + 20);
    if (name_len > 0 && name_len < 4096) {
        char name[4096];
        read_full(v, name, name_len);
    }
    if (fb_resize(v, w, h) != 0) {
        snprintf(err, errsz, "bad framebuffer size %dx%d", w, h);
        return -1;
    }

    unsigned char spf[20];
    memset(spf, 0, sizeof(spf));
    spf[0] = 0;        /* SetPixelFormat */
    spf[4] = 32;       /* bits per pixel */
    spf[5] = 24;       /* depth */
    spf[6] = 0;        /* little endian */
    spf[7] = 1;        /* true colour */
    spf[8] = 0; spf[9] = 255;
    spf[10] = 0; spf[11] = 255;
    spf[12] = 0; spf[13] = 255;
    spf[14] = 16;      /* red shift */
    spf[15] = 8;       /* green shift */
    spf[16] = 0;       /* blue shift */
    if (write_full(v, spf, sizeof(spf)) != 0) {
        snprintf(err, errsz, "cannot set pixel format");
        return -1;
    }

    unsigned char se[4 + 3 * 4];
    memset(se, 0, sizeof(se));
    se[0] = 2;         /* SetEncodings */
    se[2] = 0;
    se[3] = 3;
    /* Raw */
    se[7] = 0;
    /* CopyRect */
    se[11] = 1;
    /* DesktopSize (-223) */
    se[12] = 0xff; se[13] = 0xff; se[14] = 0xff; se[15] = 0x21;
    if (write_full(v, se, sizeof(se)) != 0) {
        snprintf(err, errsz, "cannot set encodings");
        return -1;
    }

    /* SetDesktopSize is only sent after the server proves support with an
     * ExtendedDesktopSize rectangle (see update_loop). */

    if (send_fb_update(v, 0) != 0) {
        snprintf(err, errsz, "cannot request framebuffer");
        return -1;
    }
    return 0;
}

/* Ensure the scratch buffer can hold n bytes (thread only). */
static uint8_t *scratch_get(vnc_source *v, size_t n)
{
    if (n > v->scratch_cap) {
        uint8_t *p = realloc(v->scratch, n);
        if (!p)
            return NULL;
        v->scratch = p;
        v->scratch_cap = n;
    }
    return v->scratch;
}

static void blit_raw(vnc_source *v, int x, int y, int w, int h,
                     const uint32_t *src)
{
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= v->fb_h)
            continue;
        int dx = x < 0 ? 0 : x;
        int sw = w - (dx - x);
        if (dx + sw > v->fb_w)
            sw = v->fb_w - dx;
        if (sw <= 0)
            continue;
        memcpy(v->fb + (size_t)dy * v->fb_w + dx,
               src + (size_t)row * w + (dx - x),
               (size_t)sw * sizeof(uint32_t));
    }
}

static int update_loop(vnc_source *v)
{
    while (v->running) {
        unsigned char type;
        if (read_full(v, &type, 1) != 0)
            return -1;

        if (type == 0) {                    /* FramebufferUpdate */
            unsigned char hdr[3];
            if (read_full(v, hdr, 3) != 0)
                return -1;
            int nrect = (hdr[1] << 8) | hdr[2];
            int need_full = 0;

            for (int i = 0; i < nrect; i++) {
                unsigned char rh[12];
                if (read_full(v, rh, 12) != 0)
                    return -1;
                int x = (rh[0] << 8) | rh[1];
                int y = (rh[2] << 8) | rh[3];
                int w = (rh[4] << 8) | rh[5];
                int h = (rh[6] << 8) | rh[7];
                int32_t enc = (int32_t)be32(rh + 8);

                if (enc == 0) {             /* Raw */
                    if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
                        return -1;
                    size_t bytes = (size_t)w * (size_t)h * 4;
                    uint8_t *buf = scratch_get(v, bytes);
                    if (!buf || read_full(v, buf, bytes) != 0)
                        return -1;
                    blit_raw(v, x, y, w, h, (const uint32_t *)buf);
                } else if (enc == 1) {      /* CopyRect */
                    unsigned char cr[4];
                    if (read_full(v, cr, 4) != 0)
                        return -1;
                    int sx = (cr[0] << 8) | cr[1];
                    int sy = (cr[2] << 8) | cr[3];
                    if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
                        return -1;
                    size_t bytes = (size_t)w * (size_t)h * 4;
                    uint8_t *tmp = scratch_get(v, bytes);
                    if (!tmp)
                        return -1;
                    for (int row = 0; row < h; row++) {
                        int syy = sy + row;
                        if (syy < 0 || syy >= v->fb_h)
                            continue;
                        int sxx = sx < 0 ? 0 : sx;
                        int cw = w - (sxx - sx);
                        if (sxx + cw > v->fb_w)
                            cw = v->fb_w - sxx;
                        if (cw <= 0)
                            continue;
                        memcpy(tmp + (size_t)row * w * 4,
                               v->fb + (size_t)syy * v->fb_w + sxx,
                               (size_t)cw * 4);
                    }
                    blit_raw(v, x, y, w, h, (const uint32_t *)tmp);
                } else if (enc == -223) {   /* DesktopSize */
                    if (fb_resize(v, w, h) != 0)
                        return -1;
                    need_full = 1;
                } else {
                    set_err(v, "unsupported encoding %d", enc);
                    return -1;
                }
            }

            os_mutex_lock(&v->lock);
            v->serial++;
            v->last_rx = (int64_t)(disp_now() * 1000.0);
            v->online = 1;
            os_mutex_unlock(&v->lock);

            if (send_fb_update(v, need_full ? 0 : 1) != 0)
                return -1;
        } else if (type == 1) {             /* SetColourMapEntries */
            unsigned char hdr[5];
            if (read_full(v, hdr, 5) != 0)
                return -1;
            int count = (hdr[3] << 8) | hdr[4];
            size_t bytes = (size_t)count * 6;
            uint8_t *tmp = scratch_get(v, bytes);
            if (!tmp || (bytes && read_full(v, tmp, bytes) != 0))
                return -1;
        } else if (type == 2) {             /* Bell */
            /* nothing */
        } else if (type == 3) {             /* ServerCutText */
            unsigned char hdr[7];
            if (read_full(v, hdr, 7) != 0)
                return -1;
            uint32_t len = be32(hdr + 3);
            if (len > 16 * 1024 * 1024)
                return -1;
            uint8_t *tmp = scratch_get(v, len ? len : 1);
            if (!tmp || (len && read_full(v, tmp, len) != 0))
                return -1;
        } else {
            set_err(v, "unexpected server message %d", type);
            return -1;
        }
    }
    return 0;
}

static void *vnc_thread(void *arg)
{
    vnc_source *v = arg;

    while (v->running) {
        v->sock = v->is_unix ? unix_connect(v->host)
                             : tcp_connect(v->host, v->port);
        if (v->sock == SOCK_INVALID) {
            set_err(v, "connect failed");
        } else {
            set_socket_timeout(v->sock, 500);
            char err[256] = "";
            if (handshake(v, err, sizeof(err)) == 0) {
                os_mutex_lock(&v->lock);
                snprintf(v->err, sizeof(v->err), "%s", "");
                v->online = 1;
                v->last_rx = (int64_t)(disp_now() * 1000.0);
                os_mutex_unlock(&v->lock);
                if (update_loop(v) == 0) {
                    /* stopped by close */
                }
            } else {
                set_err(v, "%s", err);
            }
            os_mutex_lock(&v->lock);
            v->online = 0;
            os_mutex_unlock(&v->lock);
            sock_close(v->sock);
            v->sock = SOCK_INVALID;
        }

        for (int i = 0; i < 20 && v->running; i++)
            disp_sleep_ms(100);
    }
    return NULL;
}

vnc_source *vnc_source_open(const char *arg, int out_w, int out_h,
                            char *err, size_t errsz)
{
    if (!arg || !*arg) {
        snprintf(err, errsz, "empty VNC address");
        return NULL;
    }
    signal(SIGPIPE, SIG_IGN);

    const char *addr = arg;
    if (strncasecmp(addr, "vnc://", 6) == 0)
        addr += 6;
    else if (strncasecmp(addr, "vnc:", 4) == 0)
        addr += 4;

    int is_unix = 0;
    if (strncmp(addr, "unix:", 5) == 0) {
        addr += 5;
        is_unix = 1;
    } else if (addr[0] == '/') {
        is_unix = 1;
    }

    char host[256];
    snprintf(host, sizeof(host), "%s", addr);
    int port = 5900;
    if (!is_unix) {
        char *colon = strrchr(host, ':');
        if (colon && colon[1]) {
            int p = atoi(colon + 1);
            if (p > 0 && p < 65536) {
                port = p;
                *colon = 0;
            }
        }
    }
    if (!host[0]) {
        snprintf(err, errsz, "empty VNC host");
        return NULL;
    }

    vnc_source *v = calloc(1, sizeof(*v));
    if (!v) {
        snprintf(err, errsz, "out of memory");
        return NULL;
    }
    snprintf(v->host, sizeof(v->host), "%s", host);
    v->port = port;
    v->is_unix = is_unix;
    v->out_w = out_w;
    v->out_h = out_h;
    v->sock = SOCK_INVALID;
    v->running = 1;
    os_mutex_init(&v->lock);
    os_mutex_init(&v->wlock);

    if (os_thread_create(&v->th, vnc_thread, v) != 0) {
        os_mutex_destroy(&v->wlock);
        os_mutex_destroy(&v->lock);
        free(v);
        snprintf(err, errsz, "cannot create VNC thread");
        return NULL;
    }
    return v;
}

void vnc_source_close(vnc_source *v)
{
    if (!v)
        return;
    v->running = 0;
    if (v->sock != SOCK_INVALID)
        shutdown(v->sock, SHUT_RDWR);
    os_thread_join(v->th);
    if (v->sws)
        sws_freeContext(v->sws);
    free(v->fb);
    free(v->scratch);
    os_mutex_destroy(&v->wlock);
    os_mutex_destroy(&v->lock);
    free(v);
}

uint64_t vnc_source_serial(vnc_source *v)
{
    os_mutex_lock(&v->lock);
    uint64_t s = v->serial;
    os_mutex_unlock(&v->lock);
    return s;
}

int vnc_source_draw(vnc_source *v, uint8_t *dst, int dst_pitch,
                    int scr_w, int scr_h, video_frame_info *info)
{
    memset(info, 0, sizeof(*info));
    if (!v || !dst)
        return 0;

    int drawn = 0;
    os_mutex_lock(&v->lock);
    if (v->fb && v->fb_w > 0 && v->fb_h > 0 && v->serial > 0) {
        double dar = (double)v->fb_w / (double)v->fb_h;
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

        if (!v->sws || v->sws_fw != v->fb_w || v->sws_fh != v->fb_h ||
            v->sws_w != w || v->sws_h != h) {
            v->sws = sws_getCachedContext(v->sws, v->fb_w, v->fb_h,
                                          AV_PIX_FMT_BGRA, w, h,
                                          AV_PIX_FMT_BGRA, SWS_BILINEAR,
                                          NULL, NULL, NULL);
            v->sws_fw = v->fb_w;
            v->sws_fh = v->fb_h;
            v->sws_w = w;
            v->sws_h = h;
        }
        if (v->sws) {
            int x = (scr_w - w) / 2;
            int y = (scr_h - h) / 2;
            const uint8_t *src[4] = { (const uint8_t *)v->fb, NULL, NULL, NULL };
            int src_lines[4] = { v->fb_w * 4, 0, 0, 0 };
            uint8_t *d[4] = { dst + (size_t)y * dst_pitch + (size_t)x * 4,
                              NULL, NULL, NULL };
            int dl[4] = { dst_pitch, 0, 0, 0 };
            sws_scale(v->sws, src, src_lines, 0, v->fb_h, d, dl);

            info->x = x;
            info->y = y;
            info->w = w;
            info->h = h;
            info->src_w = v->fb_w;
            info->src_h = v->fb_h;
            info->serial = v->serial;
            info->online = v->online;
            snprintf(info->codec, sizeof(info->codec), "vnc");
            drawn = 1;
        }
    }
    info->online = v->online;
    os_mutex_unlock(&v->lock);
    return drawn;
}

void vnc_source_status(vnc_source *v, char *codec, size_t codecsz,
                       double *pos_s, double *dur_s, double *fps,
                       int *online, char *err, size_t errsz)
{
    if (!v) {
        if (codec && codecsz) snprintf(codec, codecsz, "vnc");
        if (pos_s) *pos_s = 0;
        if (dur_s) *dur_s = 0;
        if (fps) *fps = 0;
        if (online) *online = 0;
        if (err && errsz) err[0] = 0;
        return;
    }
    os_mutex_lock(&v->lock);
    if (codec && codecsz) snprintf(codec, codecsz, "vnc");
    if (pos_s) *pos_s = 0;
    if (dur_s) *dur_s = 0;
    if (fps) *fps = 0;
    if (online) *online = v->online;
    if (err && errsz) snprintf(err, errsz, "%s", v->err);
    os_mutex_unlock(&v->lock);
}

void vnc_source_pointer(vnc_source *v, int mask, int x, int y)
{
    if (!v || v->sock == SOCK_INVALID || !v->running)
        return;
    uint8_t m[6];
    m[0] = 5;
    m[1] = (uint8_t)(mask & 0xff);
    m[2] = (uint8_t)(x >> 8);
    m[3] = (uint8_t)x;
    m[4] = (uint8_t)(y >> 8);
    m[5] = (uint8_t)y;
    write_full(v, m, sizeof(m));
}

void vnc_source_wheel(vnc_source *v, int delta, int x, int y)
{
    if (!v || v->sock == SOCK_INVALID || !v->running || !delta)
        return;
    int button = delta > 0 ? 8 : 16;   /* RFB buttons 4/5 */
    int clicks = delta > 0 ? delta : -delta;
    if (clicks > 8)
        clicks = 8;
    for (int i = 0; i < clicks; i++) {
        vnc_source_pointer(v, button, x, y);
        vnc_source_pointer(v, 0, x, y);
    }
}

void vnc_source_key(vnc_source *v, uint32_t keysym, int down)
{    if (!v || v->sock == SOCK_INVALID || !v->running)
        return;
    uint8_t m[8];
    memset(m, 0, sizeof(m));
    m[0] = 4;
    m[1] = (uint8_t)(down ? 1 : 0);
    m[4] = (uint8_t)(keysym >> 24);
    m[5] = (uint8_t)(keysym >> 16);
    m[6] = (uint8_t)(keysym >> 8);
    m[7] = (uint8_t)keysym;
    write_full(v, m, sizeof(m));
}

void vnc_source_size(vnc_source *v, int *w, int *h)
{
    if (!v) {
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    os_mutex_lock(&v->lock);
    if (w) *w = v->fb_w;
    if (h) *h = v->fb_h;
    os_mutex_unlock(&v->lock);
}