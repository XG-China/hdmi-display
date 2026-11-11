/*
 * video_dec.c - network/file video decoding with FFmpeg.
 *
 * A background thread demuxes and decodes into the newest AVFrame only (old
 * frames are dropped, which keeps latency low).  The display thread scales the
 * newest frame straight into the DRM/window canvas, so no extra full-frame
 * copy is needed.
 */
#define _GNU_SOURCE

#include "video_dec.h"
#include "os_thread.h"

#include <dlfcn.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>

#include "alsa_out.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct video_source {
    os_thread_t thread;
    os_mutex_t  lock;
    volatile int running;

    char url[1024];
    int  screen_w, screen_h;
    int  loop;
    int  still;
    int  hw;
    char fmt[32];
    char audio_dev[64];

    AVBufferRef *hw_device;
    enum AVPixelFormat hw_pix_fmt;
    AVFrame *sw_frame;      /* hardware frames transferred to system memory */

    AVFrame *latest;
    uint64_t serial;
    int64_t  last_frame_us;

    double   duration;
    double   stream_fps;
    AVRational time_base;
    char     codec[32];
    char     err[256];
    unsigned long dropped;

    /* only touched by the display thread; sws_getCachedContext() keeps this
     * in sync with the current source/destination geometry. */
    struct SwsContext *sws;
};

void video_global_init(void)
{
    avformat_network_init();

    /* libavdevice is optional: it only registers device demuxers (x11grab,
     * v4l2, ...) used by the screen/web sources. Load it when present so the
     * rest of the app does not hard-depend on its large dependency set. */
    void *h = dlopen("libavdevice.so.59", RTLD_LAZY | RTLD_LOCAL);
    if (!h)
        h = dlopen("libavdevice.so", RTLD_LAZY | RTLD_LOCAL);
    if (h) {
        void (*reg)(void) = (void (*)(void))dlsym(h, "avdevice_register_all");
        if (reg)
            reg();
        /* keep h open: the registered demuxers live in that module */
    }
}

void video_global_deinit(void)
{
    avformat_network_deinit();
}

static int is_network(const char *url)
{
    return strstr(url, "://") != NULL;
}

static void set_error(video_source *vs, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    os_mutex_lock(&vs->lock);
    snprintf(vs->err, sizeof(vs->err), "%s", buf);
    os_mutex_unlock(&vs->lock);
    fprintf(stderr, "video: %s: %s\n", vs->url, buf);
}

static int interrupt_cb(void *opaque)
{
    video_source *vs = opaque;
    return !vs->running;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts)
{
    video_source *vs = ctx->opaque;
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == vs->hw_pix_fmt)
            return *p;
    return pix_fmts[0];   /* hw not available for this codec: use software */
}

static void publish_frame(video_source *vs, AVFrame *f)
{
    os_mutex_lock(&vs->lock);
    av_frame_unref(vs->latest);
    av_frame_move_ref(vs->latest, f);
    vs->serial++;
    vs->last_frame_us = av_gettime_relative();
    os_mutex_unlock(&vs->lock);
}

/* Returns 0 = stopped, 1 = EOF, -1 = error. */
int video_probe_size(const char *url, int *w, int *h)
{
    if (!url || !*url)
        return -1;

    AVFormatContext *fmt = NULL;
    AVDictionary *opts = NULL;
    if (strncmp(url, "rtsp", 4) == 0) {
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "stimeout", "5000000", 0);
    }
    if (is_network(url))
        av_dict_set(&opts, "rw_timeout", "5000000", 0);
    av_dict_set(&opts, "probesize", "500000", 0);
    av_dict_set(&opts, "analyzeduration", "500000", 0);

    if (avformat_open_input(&fmt, url, NULL, &opts) != 0) {
        av_dict_free(&opts);
        return -1;
    }
    av_dict_free(&opts);

    int rc = -1;
    if (avformat_find_stream_info(fmt, NULL) >= 0) {
        for (unsigned i = 0; i < fmt->nb_streams; i++) {
            AVCodecParameters *par = fmt->streams[i]->codecpar;
            if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
                par->width > 0 && par->height > 0) {
                *w = par->width;
                *h = par->height;
                rc = 0;
                break;
            }
        }
    }
    avformat_close_input(&fmt);
    return rc;
}

typedef struct {
    alsa_out *ao;
    uint8_t *buf;
    size_t cap, r, w, used;
    os_mutex_t lock;
    os_thread_t thread;
    volatile int run;
    volatile int started;
} audio_sink;

static void *audio_sink_thread(void *arg)
{
    audio_sink *s = arg;
    uint8_t tmp[8192];
    while (s->run) {
        size_t n = 0;
        os_mutex_lock(&s->lock);
        if (s->started && s->used > 0) {
            n = s->used;
            if (n > sizeof(tmp))
                n = sizeof(tmp);
            size_t first = s->cap - s->r;
            if (first > n)
                first = n;
            memcpy(tmp, s->buf + s->r, first);
            if (first < n)
                memcpy(tmp + first, s->buf, n - first);
            s->r = (s->r + n) % s->cap;
            s->used -= n;
        }
        os_mutex_unlock(&s->lock);
        if (n)
            alsa_write(s->ao, tmp, (int)(n / 4));   /* S16 stereo */
        else
            usleep(2000);
    }
    return NULL;
}

static audio_sink *audio_sink_start(const char *dev, int rate, int ch)
{
    if (!dev || !*dev)
        return NULL;
    alsa_out *ao = alsa_open(dev, rate, ch);
    if (!ao)
        return NULL;

    audio_sink *s = calloc(1, sizeof(*s));
    if (!s) {
        alsa_close(ao);
        return NULL;
    }
    s->ao = ao;
    s->cap = (size_t)rate * (size_t)ch * 2 * 12;   /* 12 seconds */
    s->buf = malloc(s->cap);
    if (!s->buf) {
        alsa_close(ao);
        free(s);
        return NULL;
    }
    os_mutex_init(&s->lock);
    s->run = 1;
    if (os_thread_create(&s->thread, audio_sink_thread, s) != 0) {
        os_mutex_destroy(&s->lock);
        free(s->buf);
        alsa_close(ao);
        free(s);
        return NULL;
    }
    return s;
}

static void audio_sink_start_clock(audio_sink *s)
{
    if (!s)
        return;
    os_mutex_lock(&s->lock);
    s->started = 1;
    os_mutex_unlock(&s->lock);
}

static void audio_sink_push(audio_sink *s, const void *data, size_t bytes)
{
    if (!s || !bytes)
        return;
    os_mutex_lock(&s->lock);
    if (bytes > s->cap)
        bytes = s->cap;
    size_t free_b = s->cap - s->used;
    if (bytes > free_b) {
        /* producer is ahead (live segment burst): drop the oldest audio */
        size_t drop = bytes - free_b;
        s->r = (s->r + drop) % s->cap;
        s->used -= drop;
    }
    size_t first = s->cap - s->w;
    if (first > bytes)
        first = bytes;
    memcpy(s->buf + s->w, data, first);
    if (first < bytes)
        memcpy(s->buf, (const uint8_t *)data + first, bytes - first);
    s->w = (s->w + bytes) % s->cap;
    s->used += bytes;
    os_mutex_unlock(&s->lock);
}

static void audio_sink_stop(audio_sink *s)
{
    if (!s)
        return;
    s->run = 0;
    os_thread_join(s->thread);
    os_mutex_destroy(&s->lock);
    if (s->ao)
        alsa_close(s->ao);
    free(s->buf);
    free(s);
}

static void audio_push_frame(AVFrame *af, AVStream *ast, SwrContext **swr,
                             uint8_t **buf, int *cap, audio_sink *sink)
{
    if (!af || af->nb_samples <= 0 || !*swr || !sink)
        return;

    int out_max = swr_get_out_samples(*swr, af->nb_samples);
    if (out_max <= 0)
        return;
    int need = out_max * 2 * 2;   /* S16, stereo */
    if (*cap < need) {
        uint8_t *nb = realloc(*buf, (size_t)need);
        if (!nb)
            return;
        *buf = nb;
        *cap = need;
    }

    uint8_t *out = *buf;
    int n = swr_convert(*swr, &out, out_max,
                        (const uint8_t **)af->data, af->nb_samples);
    if (n > 0)
        audio_sink_push(sink, *buf, (size_t)n * 4);
    (void)ast;
}

static int run_session(video_source *vs)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    int result = -1;
    int vidx = -1;

    int aidx = -1;
    AVStream *ast = NULL;
    AVCodecContext *adec = NULL;
    AVFrame *aframe = NULL;
    SwrContext *swr = NULL;
    uint8_t *abuf = NULL;
    int abuf_cap = 0;
    audio_sink *as = NULL;

    fmt = avformat_alloc_context();
    if (!fmt) {
        set_error(vs, "out of memory");
        return -1;
    }
    fmt->interrupt_callback.callback = interrupt_cb;
    fmt->interrupt_callback.opaque = vs;

    AVDictionary *opts = NULL;
    if (strncmp(vs->url, "rtsp", 4) == 0) {
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "fflags", "nobuffer", 0);
        av_dict_set(&opts, "flags", "low_delay", 0);
        av_dict_set(&opts, "max_delay", "500000", 0);
        av_dict_set(&opts, "stimeout", "5000000", 0);
    }
    if (is_network(vs->url))
        av_dict_set(&opts, "rw_timeout", "5000000", 0);
    if (strcmp(vs->fmt, "fbdev") == 0)
        av_dict_set(&opts, "framerate", "15", 0);   /* text console: 15 fps is plenty */
    av_dict_set(&opts, "probesize", "1000000", 0);
    av_dict_set(&opts, "analyzeduration", "1000000", 0);

    const AVInputFormat *ifmt = NULL;
    if (vs->fmt[0]) {
        ifmt = av_find_input_format(vs->fmt);
        if (!ifmt) {
            set_error(vs, "unknown input format '%s'", vs->fmt);
            avformat_close_input(&fmt);
            av_dict_free(&opts);
            return -1;
        }
    }

    int ret = avformat_open_input(&fmt, vs->url, ifmt, &opts);    av_dict_free(&opts);
    if (ret < 0) {
        char eb[128];
        av_strerror(ret, eb, sizeof(eb));
        set_error(vs, "open failed: %s", eb);
        fmt = NULL;   /* avformat_open_input already freed it */
        return -1;
    }

    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) {
        char eb[128];
        av_strerror(ret, eb, sizeof(eb));
        set_error(vs, "stream info failed: %s", eb);
        goto done;
    }

    vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vidx < 0) {
        set_error(vs, "no video stream");
        goto done;
    }
    AVStream *st = fmt->streams[vidx];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        set_error(vs, "no decoder for codec id %d", (int)st->codecpar->codec_id);
        goto done;
    }

    dec = avcodec_alloc_context3(codec);
    if (!dec) {
        set_error(vs, "out of memory");
        goto done;
    }
    if (avcodec_parameters_to_context(dec, st->codecpar) < 0) {
        set_error(vs, "cannot copy codec parameters");
        goto done;
    }
    dec->pkt_timebase = st->time_base;
    dec->thread_count = 0;
    dec->thread_type = FF_THREAD_SLICE;   /* no frame delay */
    dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    dec->flags2 |= AV_CODEC_FLAG2_FAST;

    if (vs->hw) {
        if (!vs->hw_device) {
            vs->hw_pix_fmt = AV_PIX_FMT_VAAPI;
            int hwret = av_hwdevice_ctx_create(&vs->hw_device,
                                               AV_HWDEVICE_TYPE_VAAPI,
                                               NULL, NULL, 0);
            if (hwret < 0) {
                char eb[128];
                av_strerror(hwret, eb, sizeof(eb));
                fprintf(stderr, "video: VAAPI init failed (%s), software decode\n", eb);
                vs->hw_device = NULL;
            }
        }
        if (vs->hw_device) {
            dec->hw_device_ctx = av_buffer_ref(vs->hw_device);
            dec->opaque = vs;
            dec->get_format = get_hw_format;
        }
    }

    if (avcodec_open2(dec, codec, NULL) < 0) {
        if (dec->hw_device_ctx) {
            /* hardware path rejected by this codec/profile: retry on the CPU */
            av_buffer_unref(&dec->hw_device_ctx);
            dec->get_format = NULL;
            dec->opaque = NULL;
            fprintf(stderr, "video: VAAPI open failed, retrying software decode\n");
            if (avcodec_open2(dec, codec, NULL) < 0) {
                set_error(vs, "cannot open decoder %s", codec->name);
                goto done;
            }
        } else {
            set_error(vs, "cannot open decoder %s", codec->name);
            goto done;
        }
    }

    /* Audio is decoded from the same AVFormatContext as the video, so both
     * streams share timestamps and stay in sync (no second reader). */
    aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (aidx >= 0 && vs->audio_dev[0]) {
        ast = fmt->streams[aidx];
        const AVCodec *ac = avcodec_find_decoder(ast->codecpar->codec_id);
        if (ac) {
            adec = avcodec_alloc_context3(ac);
            if (adec &&
                avcodec_parameters_to_context(adec, ast->codecpar) == 0 &&
                avcodec_open2(adec, ac, NULL) >= 0) {
                int in_rate = ast->codecpar->sample_rate > 0
                                  ? ast->codecpar->sample_rate : 48000;
                AVChannelLayout in_layout;
                if (ast->codecpar->ch_layout.nb_channels > 0)
                    av_channel_layout_copy(&in_layout,
                                           &ast->codecpar->ch_layout);
                else
                    av_channel_layout_default(&in_layout, 2);
                AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
                SwrContext *nswr = NULL;
                if (swr_alloc_set_opts2(&nswr, &out_layout, AV_SAMPLE_FMT_S16,
                                        48000, &in_layout,
                                        (enum AVSampleFormat)ast->codecpar->format,
                                        in_rate, 0, NULL) == 0)
                    swr = nswr;
                av_channel_layout_uninit(&in_layout);
                aframe = av_frame_alloc();
                if (!swr || swr_init(swr) < 0 || !aframe) {
                    if (swr) swr_free(&swr);
                    if (aframe) av_frame_free(&aframe);
                    avcodec_free_context(&adec);
                    adec = NULL;
                    ast = NULL;
                } else {
                    as = audio_sink_start(vs->audio_dev, 48000, 2);
                    if (!as) {
                        swr_free(&swr);
                        av_frame_free(&aframe);
                        avcodec_free_context(&adec);
                        adec = NULL;
                        ast = NULL;
                    }
                }
            } else {
                if (adec) avcodec_free_context(&adec);
                adec = NULL;
                ast = NULL;
            }
        }
    }

    double dur = 0.0;
    if (fmt->duration > 0 && fmt->duration != AV_NOPTS_VALUE)
        dur = (double)fmt->duration / (double)AV_TIME_BASE;
    else if (st->duration > 0 && st->duration != AV_NOPTS_VALUE)
        dur = (double)st->duration * av_q2d(st->time_base);

    AVRational fr = av_guess_frame_rate(fmt, st, NULL);
    double fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 0.0;

    os_mutex_lock(&vs->lock);
    vs->duration = dur;
    vs->stream_fps = fps;
    vs->time_base = st->time_base;
    snprintf(vs->codec, sizeof(vs->codec), "%s", codec->name);
    vs->err[0] = 0;
    os_mutex_unlock(&vs->lock);

    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !pkt) {
        set_error(vs, "out of memory");
        goto done;
    }

    /* Pace by PTS even for live streams: RTSP/HTTP/HLS often deliver frames in
     * bursts, and without pacing the renderer would skip most of a burst
     * (the picture stutters). Unknown-duration streams start their clock at
     * the first frame. */
    int pace = fps > 1.0;
    int64_t base_wall = 0;
    double base_pts = 0;
    int have_base = 0;
    int hw_logged = 0;

    result = 0;
    while (vs->running) {
        ret = av_read_frame(fmt, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                result = 1;
            } else if (!vs->running) {
                result = 0;
            } else {
                char eb[128];
                av_strerror(ret, eb, sizeof(eb));
                set_error(vs, "read failed: %s", eb);
                result = -1;
            }
            break;
        }
        if (pkt->stream_index == aidx && adec && as) {
            if (avcodec_send_packet(adec, pkt) >= 0) {
                while (avcodec_receive_frame(adec, aframe) == 0)
                    audio_push_frame(aframe, ast, &swr, &abuf, &abuf_cap, as);
            }
            av_packet_unref(pkt);
            continue;
        }
        if (pkt->stream_index != vidx) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(dec, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        while (avcodec_receive_frame(dec, frame) == 0) {
            if (!vs->running)
                break;

            if (!hw_logged && vs->hw_device && frame->format == vs->hw_pix_fmt) {
                fprintf(stderr, "video: VAAPI decode active (%s)\n", codec->name);
                os_mutex_lock(&vs->lock);
                snprintf(vs->codec, sizeof(vs->codec), "%s (vaapi)", codec->name);
                os_mutex_unlock(&vs->lock);
                hw_logged = 1;
            }
            if (pace && frame->pts != AV_NOPTS_VALUE) {
                double pts = (double)frame->pts * av_q2d(st->time_base);
                int64_t now = av_gettime_relative();
                if (!have_base) {
                    base_wall = now;
                    base_pts = pts;
                    have_base = 1;
                    audio_sink_start_clock(as);
                }
                double delta = pts - base_pts;
                int64_t target = base_wall + (int64_t)(delta * 1000000.0);
                if (delta < -0.5 || target - now > 1000000) {
                    /* loop / seek / timestamp jump: resync, never freeze */
                    base_wall = now;
                    base_pts = pts;
                    target = now;
                }
                if (target > now)
                    av_usleep((unsigned)(target - now));
            }
            AVFrame *out_frame = frame;
            if (vs->hw_device && frame->format == vs->hw_pix_fmt) {
                av_frame_unref(vs->sw_frame);
                if (av_hwframe_transfer_data(vs->sw_frame, frame, 0) < 0)
                    continue;
                out_frame = vs->sw_frame;
            }
            publish_frame(vs, out_frame);
            if (vs->still) {
                result = 0;
                goto done;
            }
        }
    }

    if (result == 1 && vs->running) {
        avcodec_send_packet(dec, NULL);   /* flush */
        while (avcodec_receive_frame(dec, frame) == 0)
            publish_frame(vs, frame);
    }

done:
    if (as)
        audio_sink_stop(as);
    if (swr)
        swr_free(&swr);
    free(abuf);
    if (aframe)
        av_frame_free(&aframe);
    if (adec)
        avcodec_free_context(&adec);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return result;
}

static void *video_thread(void *arg)
{
    video_source *vs = arg;
    while (vs->running) {
        int r = run_session(vs);
        if (!vs->running)
            break;
        if (r == 0)
            break;
        if (r == 1) {
            if (!vs->loop)
                break;
            av_usleep(200000);
            continue;
        }
        /* error: retry, live streams often come back */
        for (int i = 0; i < 20 && vs->running; i++)
            av_usleep(100000);
    }
    return NULL;
}

video_source *video_source_open(const video_src_opts *opt, char *err, size_t errsz)
{
    if (!opt || !opt->url[0]) {
        snprintf(err, errsz, "empty source");
        return NULL;
    }

    video_source *vs = calloc(1, sizeof(*vs));
    if (!vs) {
        snprintf(err, errsz, "out of memory");
        return NULL;
    }

    snprintf(vs->url, sizeof(vs->url), "%s", opt->url);
    vs->screen_w = opt->screen_w > 0 ? opt->screen_w : 1280;
    vs->screen_h = opt->screen_h > 0 ? opt->screen_h : 720;
    vs->loop = opt->loop || is_network(vs->url);
    vs->still = opt->still;
    vs->hw = opt->hw;
    snprintf(vs->fmt, sizeof(vs->fmt), "%s", opt->fmt);
    snprintf(vs->audio_dev, sizeof(vs->audio_dev), "%s", opt->audio_dev);
    vs->time_base.num = 1;
    vs->time_base.den = 25;

    os_mutex_init(&vs->lock);
    vs->latest = av_frame_alloc();
    if (!vs->latest) {
        os_mutex_destroy(&vs->lock);
        free(vs);
        snprintf(err, errsz, "out of memory");
        return NULL;
    }
    if (vs->hw) {
        vs->sw_frame = av_frame_alloc();
        if (!vs->sw_frame) {
            av_frame_free(&vs->latest);
            os_mutex_destroy(&vs->lock);
            free(vs);
            snprintf(err, errsz, "out of memory");
            return NULL;
        }
    }

    vs->running = 1;
    if (os_thread_create(&vs->thread, video_thread, vs) != 0) {
        av_frame_free(&vs->latest);
        av_frame_free(&vs->sw_frame);
        os_mutex_destroy(&vs->lock);
        free(vs);
        snprintf(err, errsz, "cannot create decoder thread");
        return NULL;
    }
    return vs;
}

void video_source_close(video_source *vs)
{
    if (!vs)
        return;
    vs->running = 0;
    os_thread_join(vs->thread);
    av_frame_free(&vs->latest);
    av_frame_free(&vs->sw_frame);
    av_buffer_unref(&vs->hw_device);
    if (vs->sws)
        sws_freeContext(vs->sws);
    os_mutex_destroy(&vs->lock);
    free(vs);
}

uint64_t video_source_serial(video_source *vs)
{
    os_mutex_lock(&vs->lock);
    uint64_t s = vs->serial;
    os_mutex_unlock(&vs->lock);
    return s;
}

int video_source_draw(video_source *vs, uint8_t *dst, int dst_pitch,
                      int scr_w, int scr_h, video_frame_info *info)
{
    memset(info, 0, sizeof(*info));
    if (!vs || !dst || scr_w < 2 || scr_h < 2)
        return 0;

    int drawn = 0;
    os_mutex_lock(&vs->lock);

    AVFrame *f = vs->latest;
    if (f && f->width > 0 && f->height > 0 && f->data[0]) {
        double dar = (double)f->width / (double)f->height;
        if (f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0)
            dar *= (double)f->sample_aspect_ratio.num /
                   (double)f->sample_aspect_ratio.den;

        int w = scr_w;
        int h = (int)lrint((double)scr_w / dar);
        if (h > scr_h) {
            h = scr_h;
            w = (int)lrint((double)scr_h * dar);
        }
        if (w < 2) w = 2;
        if (h < 2) h = 2;
        if (w > scr_w) w = scr_w;
        if (h > scr_h) h = scr_h;
        w &= ~1;
        h &= ~1;

        if (w >= 2 && h >= 2) {
            /* Near 1:1 use point sampling: it is several times faster and
             * visually identical to the source. Scaled output uses fast
             * bilinear. sws_getCachedContext() reuses the context and only
             * rebuilds it when the geometry or pixel format changes. */
            int flags = SWS_FAST_BILINEAR;
            if (abs(w - f->width) <= 4 && abs(h - f->height) <= 4)
                flags = SWS_POINT;
            vs->sws = sws_getCachedContext(
                vs->sws, f->width, f->height, (enum AVPixelFormat)f->format,
                w, h, AV_PIX_FMT_BGRA, flags, NULL, NULL, NULL);
            if (vs->sws) {
                uint8_t *d[4] = {
                    dst + (size_t)((scr_h - h) / 2) * dst_pitch +
                          (size_t)((scr_w - w) / 2) * 4,
                    NULL, NULL, NULL
                };
                int dls[4] = { dst_pitch, 0, 0, 0 };
                sws_scale(vs->sws, (const uint8_t *const *)f->data,
                          f->linesize, 0, f->height, d, dls);

                info->x = (scr_w - w) / 2;
                info->y = (scr_h - h) / 2;
                info->w = w;
                info->h = h;
                info->src_w = f->width;
                info->src_h = f->height;
                info->serial = vs->serial;
                info->stream_fps = vs->stream_fps;
                info->dur_s = vs->duration;
                if (f->pts != AV_NOPTS_VALUE && vs->time_base.den > 0)
                    info->pos_s = (double)f->pts * av_q2d(vs->time_base);
                if (info->dur_s <= 1.0)
                    info->pos_s = 0;   /* live source: no meaningful position */
                snprintf(info->codec, sizeof(info->codec), "%s", vs->codec);
                info->dropped = vs->dropped;
                drawn = 1;
            }
        }
    }

    info->online = vs->last_frame_us != 0 &&
                   av_gettime_relative() - vs->last_frame_us < 5000000;
    os_mutex_unlock(&vs->lock);
    return drawn;
}

void video_source_status(video_source *vs, char *codec, size_t codecsz,
                         double *pos_s, double *dur_s, double *fps,
                         int *online, char *err, size_t errsz)
{
    if (!vs) {
        if (codec && codecsz) codec[0] = 0;
        if (pos_s) *pos_s = 0;
        if (dur_s) *dur_s = 0;
        if (fps) *fps = 0;
        if (online) *online = 0;
        if (err && errsz) err[0] = 0;
        return;
    }
    os_mutex_lock(&vs->lock);
    if (codec && codecsz)
        snprintf(codec, codecsz, "%s", vs->codec);
    if (dur_s) *dur_s = vs->duration;
    if (fps) *fps = vs->stream_fps;
    if (online)
        *online = vs->last_frame_us != 0 &&
                  av_gettime_relative() - vs->last_frame_us < 5000000;
    if (pos_s) {
        double p = 0;
        if (vs->latest && vs->latest->pts != AV_NOPTS_VALUE && vs->time_base.den > 0)
            p = (double)vs->latest->pts * av_q2d(vs->time_base);
        if (vs->duration <= 1.0)
            p = 0;
        *pos_s = p;
    }
    if (err && errsz)
        snprintf(err, errsz, "%s", vs->err);
    os_mutex_unlock(&vs->lock);
}
