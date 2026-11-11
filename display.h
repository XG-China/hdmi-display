/*
 * display.h - display backend abstraction (Linux only).
 *
 *   drm_display.c   DRM/KMS atomic modesetting, direct CRTC/plane scanout
 *
 * Outputs are enumerated at runtime and opened/closed on demand, so the web UI
 * can assign any connector to any source while the program is running.
 */
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define DISP_MAX_OUTPUTS 16
#define DISP_MAX_MODES   32
#define DISP_MAX_CARDS   4

typedef struct disp_ctx disp_ctx;
typedef struct disp_output disp_output;

typedef struct {
    int width;
    int height;
    int refresh;
} disp_mode;

typedef struct {
    char name[64];
    int  connected;
    int  width, height, refresh;   /* preferred / current mode */
    int  mode_count;
    disp_mode modes[DISP_MAX_MODES];
} disp_info;

typedef struct {
    int width;    /* requested width, 0 = native */
    int height;   /* requested height, 0 = native */
} disp_spec;

/* device may be NULL ("/dev/dri/card0" is used). */
int  disp_init(disp_ctx **ctx, const char *device, char *err, size_t errsz);
void disp_shutdown(disp_ctx *ctx);

/* Enumerate every connector, including disconnected ones. */
int  disp_enumerate(disp_ctx *ctx, disp_info *out, int max);
void disp_list(disp_ctx *ctx, FILE *fp);

/*
 * name: "HDMI-A-1", "card0-HDMI-A-1", or the connected index "0","1"..
 *       NULL picks the first connector that is not in use yet.
 */
disp_output *disp_open(disp_ctx *ctx, const char *name, const disp_spec *spec,
                       char *err, size_t errsz);

uint8_t *disp_backbuffer(disp_output *o);
int      disp_pitch(disp_output *o);
int      disp_width(disp_output *o);
int      disp_height(disp_output *o);
int      disp_refresh_hz(disp_output *o);

/* Blocking present. Safe to call from the display worker thread. */
int  disp_present(disp_output *o);

/* Linux has no windowing event loop; kept for the common main loop. */
int  disp_pump(disp_ctx *ctx);

void disp_close(disp_output *o);

const char *disp_backend_name(void);

/* ---- small POSIX helpers used by the UI ---- */
double disp_now(void);
void   disp_sleep_ms(int ms);

typedef struct {
    double uptime_s;
    double load1;
    unsigned long long mem_total_mb;
    unsigned long long mem_free_mb;
    char host[64];
} disp_sysinfo;
void disp_get_sysinfo(disp_sysinfo *si);

/* Returns 1 if this is the only instance, 0 if another one is running. */
int app_singleton(const char *name);

#endif /* DISPLAY_H */
