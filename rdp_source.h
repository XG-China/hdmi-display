#ifndef RDP_SOURCE_H
#define RDP_SOURCE_H

#include <stdint.h>
#include <stddef.h>
#include "video_dec.h"

typedef struct rdp_source rdp_source;

/* arg: "[domain\\]user:password@host[:port][?normal]" */
rdp_source *rdp_source_open(const char *arg, int out_w, int out_h,
                            const char *audio_dev,
                            char *err, size_t errsz);
void rdp_source_close(rdp_source *r);

int rdp_source_draw(rdp_source *r, uint8_t *dst, int dst_pitch,
                    int scr_w, int scr_h, video_frame_info *info);
void rdp_source_status(rdp_source *r, char *codec, size_t codecsz,
                       double *fps, int *online, char *err, size_t errsz);
uint64_t rdp_source_serial(rdp_source *r);

/* Linux evdev KEY_* code, forwarded as an RDP scancode. */
void rdp_source_keycode(rdp_source *r, uint16_t code, int down);
void rdp_source_pointer(rdp_source *r, int buttons, int x, int y);
void rdp_source_wheel(rdp_source *r, int delta);

#endif
