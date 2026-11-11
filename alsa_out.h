#ifndef ALSA_OUT_H
#define ALSA_OUT_H

#include <stddef.h>

typedef struct alsa_out alsa_out;

/* Open an HDMI PCM (e.g. "hw:0,3") through dlopen("libasound.so.2") so the
 * binary does not hard-depend on ALSA at build time. Returns NULL on failure. */
alsa_out *alsa_open_latency(const char *device, int rate, int channels,
                            unsigned latency_us);
alsa_out *alsa_open(const char *device, int rate, int channels);

/* HDMI PCM belonging to the GPU that owns /dev/dri/cardN, matched by PCI
 * slot and by the monitor name in ELD when several outputs are connected.
 * Returns 0 on success. */
int alsa_detect_for_drm_conn(int drm_card, const char *conn, char *out,
                             size_t cap);

/* Write interleaved S16 frames. Returns frames written, or -1 on error. */
int alsa_write(alsa_out *a, const void *buf, int frames);

void alsa_close(alsa_out *a);

#endif /* ALSA_OUT_H */
