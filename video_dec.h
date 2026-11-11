#ifndef VIDEO_DEC_H
#define VIDEO_DEC_H

#include <stdint.h>
#include <stddef.h>

typedef struct video_source video_source;

typedef struct {
    char url[1024];
    int  screen_w;
    int  screen_h;
    int  loop;      /* restart local files when they reach EOF */
    int  still;     /* single image: decode one frame and stop */
    int  hw;        /* try VAAPI hardware decoding */
    char fmt[32];   /* optional FFmpeg input format, e.g. x11grab */
    char audio_dev[64]; /* ALSA device for media audio, "" = off */
} video_src_opts;

typedef struct {
    int x, y, w, h;         /* destination rectangle inside the canvas */
    int src_w, src_h;       /* decoded frame size */
    double pos_s, dur_s;    /* playback position / duration, 0 if unknown */
    double stream_fps;
    char codec[32];
    uint64_t serial;        /* increments for every new decoded frame */
    int online;             /* frames seen in the last 5 seconds */
    unsigned long dropped;  /* reserved */
} video_frame_info;

void video_global_init(void);
void video_global_deinit(void);

/* Probe width/height of the first video stream without decoding it.
 * Used to pick an output mode that does not downscale the source. */
int video_probe_size(const char *url, int *w, int *h);

video_source *video_source_open(const video_src_opts *opt, char *err, size_t errsz);
void video_source_close(video_source *vs);

uint64_t video_source_serial(video_source *vs);

/* Scale the newest decoded frame into dst (XRGB8888). Returns 1 if drawn. */
int video_source_draw(video_source *vs, uint8_t *dst, int dst_pitch,
                      int scr_w, int scr_h, video_frame_info *info);

void video_source_status(video_source *vs, char *codec, size_t codecsz,
                         double *pos_s, double *dur_s, double *fps,
                         int *online, char *err, size_t errsz);

#endif /* VIDEO_DEC_H */
