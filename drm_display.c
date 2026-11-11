/*
 * drm_display.c - Linux DRM/KMS backend (atomic modesetting, no desktop).
 *
 * Every output owns a connector + CRTC + primary plane and two dumb buffers.
 * Frames are presented with blocking atomic commits, which gives tear-free
 * double buffering without any page-flip event bookkeeping.
 */
#if defined(__linux__)

#define _GNU_SOURCE

#include "display.h"

#include <errno.h>
#include <fcntl.h>
#include "os_thread.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <emmintrin.h>
#include <unistd.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#define MAX_OUT 8

struct disp_output {
    disp_ctx *ctx;
    int index;

    char     name[64];
    uint32_t conn_id;
    uint32_t crtc_id;
    uint32_t crtc_index;
    uint32_t plane_id;

    drmModeModeInfo mode;
    int width;
    int height;

    uint32_t fb[2];
    uint32_t handle[2];
    uint32_t pitch[2];
    uint64_t size[2];
    uint8_t *map[2];
    int back;

    /* Cached (write-back) canvas. The dumb buffer is write-combined memory;
     * drawing directly into it is several times slower than drawing into
     * normal RAM and copying the finished frame once. */
    uint8_t *shadow;
    int shadow_pitch;

    uint32_t conn_crtc_prop;
    uint32_t crtc_mode_prop, crtc_active_prop;
    uint32_t plane_fb_prop, plane_crtc_prop;
    uint32_t plane_src_x, plane_src_y, plane_src_w, plane_src_h;
    uint32_t plane_dst_x, plane_dst_y, plane_dst_w, plane_dst_h;

    unsigned long frames;
    int commit_err_logged;
};

struct disp_ctx {
    int  fd;
    char path[64];
    char driver[32];
    int  master;

    struct disp_output *out[MAX_OUT];
    int n_out;

    uint32_t used_crtc_mask;
    uint32_t used_plane[MAX_OUT];
    int      n_used_plane;

    os_mutex_t lock;
};

static void set_err(char *err, size_t errsz, const char *fmt, ...)
{
    if (!err || errsz == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

static const char *conn_type_name(uint32_t t)
{
    switch (t) {
    case DRM_MODE_CONNECTOR_VGA:        return "VGA";
    case DRM_MODE_CONNECTOR_DVII:       return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID:       return "DVI-D";
    case DRM_MODE_CONNECTOR_DVIA:       return "DVI-A";
    case DRM_MODE_CONNECTOR_Composite:  return "COMPOSITE";
    case DRM_MODE_CONNECTOR_SVIDEO:     return "SVIDEO";
    case DRM_MODE_CONNECTOR_LVDS:       return "LVDS";
    case DRM_MODE_CONNECTOR_Component:  return "COMPONENT";
    case DRM_MODE_CONNECTOR_9PinDIN:    return "DIN";
    case DRM_MODE_CONNECTOR_DisplayPort:return "DP";
    case DRM_MODE_CONNECTOR_HDMIA:      return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB:      return "HDMI-B";
    case DRM_MODE_CONNECTOR_TV:         return "TV";
    case DRM_MODE_CONNECTOR_eDP:        return "eDP";
    case DRM_MODE_CONNECTOR_VIRTUAL:    return "VIRTUAL";
    case DRM_MODE_CONNECTOR_DSI:        return "DSI";
    default:                            return "CONN";
    }
}

static void connector_name(drmModeConnector *c, char *buf, size_t n)
{
    snprintf(buf, n, "%s-%u", conn_type_name(c->connector_type),
             (unsigned)c->connector_type_id);
}

static uint32_t object_prop_id(int fd, uint32_t id, uint32_t type, const char *name)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, id, type);
    if (!props)
        return 0;
    uint32_t found = 0;
    for (uint32_t i = 0; i < props->count_props && !found; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (p) {
            if (strcmp(p->name, name) == 0)
                found = p->prop_id;
            drmModeFreeProperty(p);
        }
    }
    drmModeFreeObjectProperties(props);
    return found;
}

static int object_prop_value(int fd, uint32_t id, uint32_t type,
                             const char *name, uint64_t *value)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, id, type);
    if (!props)
        return -1;
    int ret = -1;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (p) {
            if (strcmp(p->name, name) == 0) {
                *value = props->prop_values[i];
                ret = 0;
            }
            drmModeFreeProperty(p);
        }
        if (ret == 0)
            break;
    }
    drmModeFreeObjectProperties(props);
    return ret;
}

/* Disable every CRTC/plane so the framebuffer console stops drawing. */
static void disable_all(disp_ctx *ctx)
{
    drmModeRes *res = drmModeGetResources(ctx->fd);
    if (!res)
        return;
    drmModePlaneRes *pres = drmModeGetPlaneResources(ctx->fd);
    drmModeAtomicReq *req = drmModeAtomicAlloc();

    if (req) {
        for (int i = 0; i < res->count_connectors; i++) {
            uint32_t p = object_prop_id(ctx->fd, res->connectors[i],
                                        DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
            if (p)
                drmModeAtomicAddProperty(req, res->connectors[i], p, 0);
        }
        for (int i = 0; i < res->count_crtcs; i++) {
            uint32_t p = object_prop_id(ctx->fd, res->crtcs[i],
                                        DRM_MODE_OBJECT_CRTC, "ACTIVE");
            if (p)
                drmModeAtomicAddProperty(req, res->crtcs[i], p, 0);
        }
        if (pres) {
            for (uint32_t i = 0; i < pres->count_planes; i++) {
                uint32_t fb = object_prop_id(ctx->fd, pres->planes[i],
                                             DRM_MODE_OBJECT_PLANE, "FB_ID");
                uint32_t cr = object_prop_id(ctx->fd, pres->planes[i],
                                             DRM_MODE_OBJECT_PLANE, "CRTC_ID");
                if (fb)
                    drmModeAtomicAddProperty(req, pres->planes[i], fb, 0);
                if (cr)
                    drmModeAtomicAddProperty(req, pres->planes[i], cr, 0);
            }
        }
        if (drmModeAtomicCommit(ctx->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) != 0)
            fprintf(stderr, "drm: warning: could not clear all CRTCs: %s\n",
                    strerror(errno));
        drmModeAtomicFree(req);
    }
    if (pres)
        drmModeFreePlaneResources(pres);
    drmModeFreeResources(res);
}

int disp_init(disp_ctx **out_ctx, const char *device, char *err, size_t errsz)
{
    const char *path = (device && *device) ? device : "/dev/dri/card0";
    disp_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        set_err(err, errsz, "out of memory");
        return -1;
    }

    ctx->fd = open(path, O_RDWR | O_CLOEXEC);
    if (ctx->fd < 0) {
        set_err(err, errsz, "cannot open %s: %s (are you root? is a desktop running?)",
                path, strerror(errno));
        free(ctx);
        return -1;
    }
    snprintf(ctx->path, sizeof(ctx->path), "%s", path);
    snprintf(ctx->driver, sizeof(ctx->driver), "unknown");

    drmVersion *ver = drmGetVersion(ctx->fd);
    if (ver) {
        snprintf(ctx->driver, sizeof(ctx->driver), "%s", ver->name ? ver->name : "unknown");
        drmFreeVersion(ver);
    }

    if (drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        set_err(err, errsz, "driver '%s' does not support atomic modesetting", ctx->driver);
        close(ctx->fd);
        free(ctx);
        return -1;
    }
    drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    if (drmSetMaster(ctx->fd) != 0) {
        fprintf(stderr, "drm: warning: cannot become DRM master: %s\n",
                strerror(errno));
        fprintf(stderr, "drm: listing is still possible, opening outputs is not "
                        "(stop X/Wayland/display manager first)\n");
        ctx->master = 0;
    } else {
        ctx->master = 1;
    }

    os_mutex_init(&ctx->lock);
    if (ctx->master)
        disable_all(ctx);
    *out_ctx = ctx;
    return 0;
}

void disp_shutdown(disp_ctx *ctx)
{
    if (!ctx)
        return;
    while (ctx->n_out > 0)
        disp_close(ctx->out[ctx->n_out - 1]);
    drmDropMaster(ctx->fd);
    close(ctx->fd);
    os_mutex_destroy(&ctx->lock);
    free(ctx);
}

void disp_list(disp_ctx *ctx, FILE *fp)
{
    drmModeRes *res = drmModeGetResources(ctx->fd);
    if (!res) {
        fprintf(fp, "cannot enumerate DRM resources\n");
        return;
    }
    fprintf(fp, "DRM device %s  driver: %s\n", ctx->path, ctx->driver);
    int idx = 0;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(ctx->fd, res->connectors[i]);
        if (!c)
            continue;
        char name[64];
        connector_name(c, name, sizeof(name));
        if (c->connection == DRM_MODE_CONNECTED) {
            char modes[128] = "";
            if (c->count_modes > 0)
                snprintf(modes, sizeof(modes), "%dx%d@%d",
                         c->modes[0].hdisplay, c->modes[0].vdisplay,
                         c->modes[0].vrefresh);
            fprintf(fp, "  [%d] %-12s connected    %s\n", idx, name, modes);
            idx++;
        } else {
            fprintf(fp, "  [ ] %-12s %s\n", name,
                    c->connection == DRM_MODE_DISCONNECTED ? "disconnected" : "unknown");
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
}

int disp_enumerate(disp_ctx *ctx, disp_info *out, int max)
{
    drmModeRes *res = drmModeGetResources(ctx->fd);
    if (!res)
        return 0;

    int n = 0;
    for (int i = 0; i < res->count_connectors && n < max; i++) {
        drmModeConnector *c = drmModeGetConnector(ctx->fd, res->connectors[i]);
        if (!c)
            continue;

        disp_info *d = &out[n];
        memset(d, 0, sizeof(*d));
        connector_name(c, d->name, sizeof(d->name));
        d->connected = (c->connection == DRM_MODE_CONNECTED);

        int preferred = -1;
        for (int m = 0; m < c->count_modes; m++) {
            const drmModeModeInfo *mo = &c->modes[m];
            int found = -1;
            for (int k = 0; k < d->mode_count; k++) {
                if (d->modes[k].width == mo->hdisplay &&
                    d->modes[k].height == mo->vdisplay) {
                    found = k;
                    if (mo->vrefresh > (uint32_t)d->modes[k].refresh)
                        d->modes[k].refresh = mo->vrefresh;
                    break;
                }
            }
            if (found < 0 && d->mode_count < DISP_MAX_MODES) {
                found = d->mode_count++;
                d->modes[found].width = mo->hdisplay;
                d->modes[found].height = mo->vdisplay;
                d->modes[found].refresh = mo->vrefresh;
            }
            if ((mo->type & DRM_MODE_TYPE_PREFERRED) && preferred < 0)
                preferred = found;
        }

        if (preferred > 0) {
            disp_mode tmp = d->modes[0];
            d->modes[0] = d->modes[preferred];
            d->modes[preferred] = tmp;
        }
        if (d->mode_count > 0) {
            d->width = d->modes[0].width;
            d->height = d->modes[0].height;
            d->refresh = d->modes[0].refresh;
        }
        n++;
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    return n;
}

static int output_used(disp_ctx *ctx, uint32_t conn_id)
{
    for (int i = 0; i < ctx->n_out; i++)
        if (ctx->out[i]->conn_id == conn_id)
            return 1;
    return 0;
}

/* Resolve "0" (connected index), "HDMI-A-1" or "card0-HDMI-A-1". */
static drmModeConnector *resolve_connector(disp_ctx *ctx, drmModeRes *res,
                                           const char *name,
                                           char *err, size_t errsz)
{
    int want_index = -1;
    if (name && *name) {
        int all_digits = 1;
        for (const char *p = name; *p; p++)
            if (*p < '0' || *p > '9') { all_digits = 0; break; }
        if (all_digits)
            want_index = atoi(name);
    }

    const char *want_name = name;
    char stripped[64];
    if (name && strncmp(name, "card", 4) == 0) {
        const char *dash = strchr(name, '-');
        if (dash && dash[1]) {
            snprintf(stripped, sizeof(stripped), "%s", dash + 1);
            want_name = stripped;
        }
    }

    int idx = 0;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(ctx->fd, res->connectors[i]);
        if (!c)
            continue;
        int match = 0;
        if (c->connection == DRM_MODE_CONNECTED) {
            if (!name || (want_index < 0 && (!*name)))
                match = !output_used(ctx, c->connector_id);
            else if (want_index >= 0)
                match = (idx == want_index);
            else {
                char cn[64];
                connector_name(c, cn, sizeof(cn));
                match = (strcmp(cn, want_name) == 0);
            }
            idx++;
        }
        if (match)
            return c;   /* caller frees */
        drmModeFreeConnector(c);
    }

    if (name && *name)
        set_err(err, errsz, "connector '%s' not found or not connected", name);
    else
        set_err(err, errsz, "no free connected connector left");
    return NULL;
}

static const drmModeModeInfo *pick_mode(drmModeConnector *c, int width, int height)
{
    if (width > 0 && height > 0) {
        for (int i = 0; i < c->count_modes; i++)
            if (c->modes[i].hdisplay == width && c->modes[i].vdisplay == height)
                return &c->modes[i];
    }
    for (int i = 0; i < c->count_modes; i++)
        if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED)
            return &c->modes[i];
    for (int i = 0; i < c->count_modes; i++)
        if (c->modes[i].hdisplay == 1920 && c->modes[i].vdisplay == 1080)
            return &c->modes[i];
    return c->count_modes > 0 ? &c->modes[0] : NULL;
}

static int find_plane(disp_ctx *ctx, uint32_t crtc_index, uint32_t crtc_id,
                      uint32_t *out_plane, char *err, size_t errsz)
{
    drmModePlaneRes *pres = drmModeGetPlaneResources(ctx->fd);
    if (!pres) {
        set_err(err, errsz, "cannot enumerate planes");
        return -1;
    }

    uint32_t best = 0;
    int best_score = -1;
    for (uint32_t i = 0; i < pres->count_planes; i++) {
        int used = 0;
        for (int j = 0; j < ctx->n_used_plane; j++)
            if (ctx->used_plane[j] == pres->planes[i])
                used = 1;
        if (used)
            continue;

        drmModePlane *p = drmModeGetPlane(ctx->fd, pres->planes[i]);
        if (!p)
            continue;
        if (!(p->possible_crtcs & (1u << crtc_index))) {
            drmModeFreePlane(p);
            continue;
        }
        uint64_t type = 0;
        if (object_prop_value(ctx->fd, p->plane_id, DRM_MODE_OBJECT_PLANE,
                              "type", &type) == 0 &&
            type == DRM_PLANE_TYPE_PRIMARY) {
            int score = 0;
            if (p->crtc_id == 0)
                score = 1;
            else if (p->crtc_id == crtc_id)
                score = 2;
            if (score > best_score) {
                best_score = score;
                best = p->plane_id;
            }
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pres);

    if (!best) {
        set_err(err, errsz, "no free primary plane for CRTC %u", crtc_index);
        return -1;
    }
    *out_plane = best;
    return 0;
}

static void destroy_output(disp_output *o)
{
    if (!o)
        return;
    disp_ctx *ctx = o->ctx;

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (req) {
        if (o->plane_fb_prop)
            drmModeAtomicAddProperty(req, o->plane_id, o->plane_fb_prop, 0);
        if (o->plane_crtc_prop)
            drmModeAtomicAddProperty(req, o->plane_id, o->plane_crtc_prop, 0);
        if (o->conn_crtc_prop)
            drmModeAtomicAddProperty(req, o->conn_id, o->conn_crtc_prop, 0);
        if (o->crtc_active_prop)
            drmModeAtomicAddProperty(req, o->crtc_id, o->crtc_active_prop, 0);
        os_mutex_lock(&ctx->lock);
        drmModeAtomicCommit(ctx->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
        os_mutex_unlock(&ctx->lock);
        drmModeAtomicFree(req);
    }

    for (int i = 0; i < 2; i++) {
        if (o->map[i] && o->map[i] != MAP_FAILED)
            munmap(o->map[i], o->size[i]);
        if (o->fb[i])
            drmModeRmFB(ctx->fd, o->fb[i]);
        if (o->handle[i]) {
            struct drm_mode_destroy_dumb dreq;
            memset(&dreq, 0, sizeof(dreq));
            dreq.handle = o->handle[i];
            drmIoctl(ctx->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        }
    }

    if (o->crtc_index < 32)
        ctx->used_crtc_mask &= ~(1u << o->crtc_index);
    for (int i = 0; i < ctx->n_used_plane; i++) {
        if (ctx->used_plane[i] == o->plane_id) {
            ctx->used_plane[i] = ctx->used_plane[ctx->n_used_plane - 1];
            ctx->n_used_plane--;
            break;
        }
    }
    for (int i = 0; i < ctx->n_out; i++) {
        if (ctx->out[i] == o) {
            ctx->out[i] = ctx->out[ctx->n_out - 1];
            ctx->n_out--;
            break;
        }
    }
    free(o->shadow);
    free(o);
}

static int make_buffer(disp_output *o, int idx, char *err, size_t errsz)
{
    disp_ctx *ctx = o->ctx;
    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof(creq));
    creq.width = o->width;
    creq.height = o->height;
    creq.bpp = 32;

    if (drmIoctl(ctx->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0) {
        set_err(err, errsz, "dumb buffer %dx%d failed: %s",
                o->width, o->height, strerror(errno));
        return -1;
    }
    o->handle[idx] = creq.handle;
    o->pitch[idx] = creq.pitch;
    o->size[idx] = creq.size;

    if (drmModeAddFB(ctx->fd, o->width, o->height, 24, 32,
                     creq.pitch, creq.handle, &o->fb[idx]) != 0) {
        set_err(err, errsz, "drmModeAddFB failed: %s", strerror(errno));
        return -1;
    }

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    if (drmIoctl(ctx->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) != 0) {
        set_err(err, errsz, "DRM_IOCTL_MODE_MAP_DUMB failed: %s", strerror(errno));
        return -1;
    }
    o->map[idx] = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       ctx->fd, (off_t)mreq.offset);
    if (o->map[idx] == MAP_FAILED) {
        o->map[idx] = NULL;
        set_err(err, errsz, "mmap of dumb buffer failed: %s", strerror(errno));
        return -1;
    }
    memset(o->map[idx], 0, creq.size);
    return 0;
}

static int output_modeset(disp_output *o, char *err, size_t errsz)
{
    disp_ctx *ctx = o->ctx;
    uint32_t blob = 0;

    if (drmModeCreatePropertyBlob(ctx->fd, &o->mode, sizeof(o->mode), &blob) != 0) {
        set_err(err, errsz, "cannot create mode blob: %s", strerror(errno));
        return -1;
    }

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    int ok = req != NULL;

    ok &= drmModeAtomicAddProperty(req, o->conn_id, o->conn_crtc_prop, o->crtc_id) >= 0;
    ok &= drmModeAtomicAddProperty(req, o->crtc_id, o->crtc_mode_prop, blob) >= 0;
    ok &= drmModeAtomicAddProperty(req, o->crtc_id, o->crtc_active_prop, 1) >= 0;
    ok &= drmModeAtomicAddProperty(req, o->plane_id, o->plane_crtc_prop, o->crtc_id) >= 0;
    ok &= drmModeAtomicAddProperty(req, o->plane_id, o->plane_fb_prop, o->fb[0]) >= 0;
    ok &= drmModeAtomicAddProperty(req, o->plane_id, o->plane_src_x, 0) >= 0;
    ok &= drmModeAtomicAddProperty(req, o->plane_id, o->plane_src_y, 0) >= 0;
    ok &= drmModeAtomicAddProperty(req, o->plane_id, o->plane_src_w,
                                   (uint64_t)o->width << 16) >= 0;
    ok &= drmModeAtomicAddProperty(req, o->plane_id, o->plane_src_h,
                                   (uint64_t)o->height << 16) >= 0;
    ok &= drmModeAtomicAddProperty(req, o->plane_id, o->plane_dst_x, 0) >= 0;
    ok &= drmModeAtomicAddProperty(req, o->plane_id, o->plane_dst_y, 0) >= 0;
    ok &= drmModeAtomicAddProperty(req, o->plane_id, o->plane_dst_w, o->width) >= 0;
    ok &= drmModeAtomicAddProperty(req, o->plane_id, o->plane_dst_h, o->height) >= 0;

    int ret = -1;
    if (ok) {
        os_mutex_lock(&ctx->lock);
        ret = drmModeAtomicCommit(ctx->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
        int saved = errno;
        os_mutex_unlock(&ctx->lock);
        if (ret != 0)
            set_err(err, errsz, "atomic modeset on %s failed: %s",
                    o->name, strerror(saved));
    } else {
        set_err(err, errsz, "cannot build atomic request");
    }

    if (req)
        drmModeAtomicFree(req);
    drmModeDestroyPropertyBlob(ctx->fd, blob);
    return ret == 0 ? 0 : -1;
}

disp_output *disp_open(disp_ctx *ctx, const char *name, const disp_spec *spec,
                       char *err, size_t errsz)
{
    if (!ctx) {
        set_err(err, errsz, "no display context");
        return NULL;
    }
    if (!ctx->master) {
        /* Retry: a compositor or X server may have released DRM master after
         * we started, and the wall should recover without a service restart. */
        if (drmSetMaster(ctx->fd) == 0) {
            ctx->master = 1;
            disable_all(ctx);
        } else {
            set_err(err, errsz,
                    "not DRM master: stop X/Wayland/display manager first");
            return NULL;
        }
    }
    if (ctx->n_out >= MAX_OUT) {
        set_err(err, errsz, "too many outputs (max %d)", MAX_OUT);
        return NULL;
    }

    int width = spec ? spec->width : 0;
    int height = spec ? spec->height : 0;

    drmModeRes *res = drmModeGetResources(ctx->fd);
    if (!res) {
        set_err(err, errsz, "drmModeGetResources failed: %s", strerror(errno));
        return NULL;
    }

    drmModeConnector *conn = resolve_connector(ctx, res, name, err, errsz);
    if (!conn) {
        drmModeFreeResources(res);
        return NULL;
    }

    disp_output *o = calloc(1, sizeof(*o));
    if (!o) {
        set_err(err, errsz, "out of memory");
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return NULL;
    }
    o->ctx = ctx;
    o->crtc_index = 0xFFFFFFFFu;
    o->conn_id = conn->connector_id;
    connector_name(conn, o->name, sizeof(o->name));

    const drmModeModeInfo *mode = pick_mode(conn, width, height);
    if (!mode) {
        set_err(err, errsz, "connector %s reports no modes", o->name);
        goto fail;
    }
    if ((width > 0 && height > 0) &&
        (mode->hdisplay != width || mode->vdisplay != height))
        fprintf(stderr, "drm: %s: requested %dx%d not available, using %dx%d\n",
                o->name, width, height, mode->hdisplay, mode->vdisplay);
    o->mode = *mode;
    o->width = mode->hdisplay;
    o->height = mode->vdisplay;

    /* Pick an encoder/CRTC pair that is not used by another output. */
    int crtc_index = -1;
    drmModeEncoder *enc = NULL;
    if (conn->encoder_id)
        enc = drmModeGetEncoder(ctx->fd, conn->encoder_id);
    if (enc && enc->crtc_id) {
        for (int i = 0; i < res->count_crtcs; i++)
            if (res->crtcs[i] == enc->crtc_id && !(ctx->used_crtc_mask & (1u << i)))
                crtc_index = i;
    }
    if (crtc_index < 0 && enc) {
        for (int i = 0; i < res->count_crtcs; i++) {
            if (!(ctx->used_crtc_mask & (1u << i)) &&
                (enc->possible_crtcs & (1u << i))) {
                crtc_index = i;
                break;
            }
        }
    }
    if (crtc_index < 0) {
        for (int i = 0; i < res->count_crtcs; i++) {
            if (!(ctx->used_crtc_mask & (1u << i))) {
                crtc_index = i;
                break;
            }
        }
    }
    if (enc)
        drmModeFreeEncoder(enc);
    if (crtc_index < 0) {
        set_err(err, errsz, "no free CRTC for %s", o->name);
        goto fail;
    }

    o->crtc_index = (uint32_t)crtc_index;
    o->crtc_id = res->crtcs[crtc_index];

    if (find_plane(ctx, o->crtc_index, o->crtc_id, &o->plane_id, err, errsz) != 0)
        goto fail;

    o->conn_crtc_prop = object_prop_id(ctx->fd, o->conn_id,
                                       DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    o->crtc_mode_prop = object_prop_id(ctx->fd, o->crtc_id,
                                       DRM_MODE_OBJECT_CRTC, "MODE_ID");
    o->crtc_active_prop = object_prop_id(ctx->fd, o->crtc_id,
                                         DRM_MODE_OBJECT_CRTC, "ACTIVE");
    o->plane_fb_prop = object_prop_id(ctx->fd, o->plane_id,
                                      DRM_MODE_OBJECT_PLANE, "FB_ID");
    o->plane_crtc_prop = object_prop_id(ctx->fd, o->plane_id,
                                        DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    o->plane_src_x = object_prop_id(ctx->fd, o->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    o->plane_src_y = object_prop_id(ctx->fd, o->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    o->plane_src_w = object_prop_id(ctx->fd, o->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    o->plane_src_h = object_prop_id(ctx->fd, o->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    o->plane_dst_x = object_prop_id(ctx->fd, o->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    o->plane_dst_y = object_prop_id(ctx->fd, o->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    o->plane_dst_w = object_prop_id(ctx->fd, o->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    o->plane_dst_h = object_prop_id(ctx->fd, o->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");

    if (!o->conn_crtc_prop || !o->crtc_mode_prop || !o->crtc_active_prop ||
        !o->plane_fb_prop || !o->plane_crtc_prop || !o->plane_src_w ||
        !o->plane_src_h || !o->plane_src_x || !o->plane_src_y || !o->plane_dst_x ||
        !o->plane_dst_y || !o->plane_dst_w || !o->plane_dst_h) {
        set_err(err, errsz, "missing atomic properties on %s", o->name);
        goto fail;
    }

    if (make_buffer(o, 0, err, errsz) != 0 || make_buffer(o, 1, err, errsz) != 0)
        goto fail;

    if (output_modeset(o, err, errsz) != 0)
        goto fail;

    o->back = 1;  /* buffer 0 is now being scanned out */

    ctx->used_crtc_mask |= (1u << o->crtc_index);
    ctx->used_plane[ctx->n_used_plane++] = o->plane_id;
    o->index = ctx->n_out;
    ctx->out[ctx->n_out++] = o;

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return o;

fail:
    destroy_output(o);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return NULL;
}

uint8_t *disp_backbuffer(disp_output *o)
{
    if (!o->shadow) {
        o->shadow_pitch = (int)o->pitch[0];
        size_t sz = (size_t)o->shadow_pitch * (size_t)o->height;
        o->shadow = malloc(sz);
        if (o->shadow)
            memset(o->shadow, 0, sz);
    }
    return o->shadow ? o->shadow : o->map[o->back];
}

int disp_pitch(disp_output *o)
{
    return o->shadow_pitch > 0 ? o->shadow_pitch : (int)o->pitch[o->back];
}

int disp_width(disp_output *o)
{
    return o->width;
}

int disp_height(disp_output *o)
{
    return o->height;
}

int disp_refresh_hz(disp_output *o)
{
    return o->mode.vrefresh;
}

int disp_present(disp_output *o)
{
    disp_ctx *ctx = o->ctx;
    int idx = o->back;

    if (o->shadow && o->map[idx]) {
        size_t bytes = (size_t)o->pitch[idx] * (size_t)o->height;
        /* The dumb buffer is write-combined memory: a streaming (non-temporal)
         * copy is several times faster than a cache-polluting memcpy. */
#if defined(__x86_64__) || defined(__i386__)
        size_t i = 0;
        for (; i + 16 <= bytes; i += 16) {
            __m128i v = _mm_loadu_si128(
                (const __m128i *)(const void *)(o->shadow + i));
            _mm_stream_si128((__m128i *)(void *)(o->map[idx] + i), v);
        }
        _mm_sfence();
        if (i < bytes)
            memcpy(o->map[idx] + i, o->shadow + i, bytes - i);
#else
        memcpy(o->map[idx], o->shadow, bytes);
#endif
    }

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req)
        return -1;
    if (drmModeAtomicAddProperty(req, o->plane_id, o->plane_fb_prop,
                                 o->fb[idx]) < 0) {
        drmModeAtomicFree(req);
        return -1;
    }

    os_mutex_lock(&ctx->lock);
    int ret = drmModeAtomicCommit(ctx->fd, req, 0, NULL);
    int saved = errno;
    os_mutex_unlock(&ctx->lock);
    drmModeAtomicFree(req);

    if (ret != 0) {
        if (!o->commit_err_logged) {
            fprintf(stderr, "drm: %s: atomic flip failed: %s\n",
                    o->name, strerror(saved));
            o->commit_err_logged = 1;
        }
        return -1;
    }
    o->commit_err_logged = 0;
    o->back = idx ^ 1;
    o->frames++;
    return 0;
}

void disp_close(disp_output *o)
{
    destroy_output(o);
}

const char *disp_backend_name(void) { return "DRM/KMS"; }

int disp_pump(disp_ctx *ctx)
{
    (void)ctx;
    return 0;
}

#endif /* __linux__ */
