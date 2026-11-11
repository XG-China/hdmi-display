#ifndef VNC_SOURCE_H
#define VNC_SOURCE_H

#include <stdint.h>
#include <stddef.h>

#include "video_dec.h"   /* reuse video_frame_info */

typedef struct vnc_source vnc_source;

/* arg: "host:port" or "vnc://host:port" (default port 5900) */
vnc_source *vnc_source_open(const char *arg, int out_w, int out_h,
                            char *err, size_t errsz);
void vnc_source_close(vnc_source *v);

uint64_t vnc_source_serial(vnc_source *v);
int      vnc_source_draw(vnc_source *v, uint8_t *dst, int dst_pitch,
                         int scr_w, int scr_h, video_frame_info *info);
void     vnc_source_pointer(vnc_source *v, int mask, int x, int y);
void     vnc_source_wheel(vnc_source *v, int delta, int x, int y);
void     vnc_source_key(vnc_source *v, uint32_t keysym, int down);
void     vnc_source_size(vnc_source *v, int *w, int *h);
void     vnc_source_status(vnc_source *v, char *codec, size_t codecsz,
                           double *pos_s, double *dur_s, double *fps,
                           int *online, char *err, size_t errsz);

#endif /* VNC_SOURCE_H */
