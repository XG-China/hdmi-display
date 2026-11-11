#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdint.h>
#include <stddef.h>

/*
 * The web UI is the only management interface and works on every platform.
 * Names (connector / monitor) are stable across hotplug, so the API is name
 * based rather than index based.
 */
typedef struct {
    int  (*state_json)(char *buf, size_t cap, void *ud);

    /* Assign a source to an output and enable it. */
    int  (*apply)(const char *out, const char *type, const char *arg,
                  int w, int h, void *ud);

    /* Stop an output (close its CRTC / window). */
    int  (*disable)(const char *out, void *ud);

    /*
     * Copy the latest RGB24 preview.
     *   0  = ok
     *  -1  = active but no frame yet
     *  -2  = unknown or inactive output
     */
    int  (*copy_preview)(const char *out, uint8_t *rgb, size_t cap,
                         int *w, int *h, uint64_t *serial, void *ud);

    /* Bind a physical input device to an output (empty out = unbind). */
    int  (*bind_input)(const char *id, const char *out, void *ud);

    void (*request_quit)(void *ud);
    const char *token;   /* optional; when set, ?token=... is required */
} web_ops;

typedef struct {
    int port;                /* 0 = do not listen on TCP */
    const char *bind_addr;   /* NULL / "0.0.0.0" = all interfaces */
    const char *socket_path; /* optional Unix socket for the fnOS gateway */
    const char *prefix;      /* optional gateway path, e.g. /app/hdmi-display */
    const char *web_dir;     /* static panel directory; NULL = embedded fallback */
} web_config;

/* At least one of port / socket_path must be configured. */
int  web_start(const web_config *cfg, const web_ops *ops, void *ud,
               char *err, size_t errsz);
void web_stop(void);

#endif /* WEB_SERVER_H */
