/*
 * rdp_audio.c - FreeRDP rdpsnd device backend that plays remote audio on the
 * wall output's HDMI PCM through the app's ALSA helper.
 *
 * FreeRDP's /sound argument parser splits on commas, so the ALSA PCM name
 * "hw:3,8" cannot be passed as dev:hw:3,8.  We register a private rdpsnd
 * subsystem ("app") and pass a comma-free "dev:c3d8" token instead; this
 * backend reconstructs the real PCM name.
 */
#define _GNU_SOURCE
#include "rdp_audio.h"

#include <freerdp/addin.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/rdpsnd.h>
#include <freerdp/channels/rdpsnd.h>
#include <freerdp/codec/audio.h>
#include <freerdp/settings_types.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alsa_out.h"

typedef struct {
    rdpsndDevicePlugin dev;
    alsa_out *alsa;
    char pcm[64];
    int rate;
    int channels;
} app_snd;

static BOOL app_fmt_supported(rdpsndDevicePlugin *dev, const AUDIO_FORMAT *fmt)
{
    if (!fmt)
        return FALSE;
    return fmt->wFormatTag == WAVE_FORMAT_PCM && fmt->cbSize == 0 &&
           fmt->wBitsPerSample == 16 &&
           (fmt->nChannels == 1 || fmt->nChannels == 2) &&
           fmt->nSamplesPerSec >= 8000 && fmt->nSamplesPerSec <= 48000;
}

static BOOL app_open(rdpsndDevicePlugin *dev, const AUDIO_FORMAT *fmt,
                     UINT32 latency)
{
    app_snd *s = (app_snd *)dev;
    if (s->alsa) {
        alsa_close(s->alsa);
        s->alsa = NULL;
    }
    if (fmt) {
        s->rate = (int)fmt->nSamplesPerSec;
        s->channels = (int)fmt->nChannels;
    }
    if (s->rate <= 0)
        s->rate = 48000;
    if (s->channels <= 0 || s->channels > 2)
        s->channels = 2;
    unsigned us = latency ? latency * 1000u : 50000u;
    if (us < 30000u)
        us = 30000u;
    if (us > 200000u)
        us = 200000u;
    /* Keep the remote audio close to the decoded video; 50 ms is the default
     * and can be tuned with the /sound latency argument. */
    s->alsa = alsa_open_latency(s->pcm, s->rate, s->channels, us);
    if (!s->alsa)
        fprintf(stderr, "rdp audio: cannot open %s at %d Hz / %d ch\n",
                s->pcm, s->rate, s->channels);
    return s->alsa != NULL;
}

static UINT32 app_get_volume(rdpsndDevicePlugin *dev)
{
    return 0x80008000u;   /* 50% left/right */
}

static BOOL app_set_volume(rdpsndDevicePlugin *dev, UINT32 value)
{
    return TRUE;
}

static UINT app_play(rdpsndDevicePlugin *dev, const BYTE *data, size_t size)
{
    app_snd *s = (app_snd *)dev;
    if (!s->alsa || !data)
        return CHANNEL_RC_INITIALIZATION_ERROR;
    size_t frame_bytes = (size_t)s->channels * 2;
    int frames = (int)(size / frame_bytes);
    if (frames <= 0)
        return CHANNEL_RC_OK;
    if (alsa_write(s->alsa, data, frames) < 0) {
        fprintf(stderr, "rdp audio: ALSA write failed\n");
        return CHANNEL_RC_NO_MEMORY;
    }
    return CHANNEL_RC_OK;
}

static void app_close(rdpsndDevicePlugin *dev)
{
    app_snd *s = (app_snd *)dev;
    if (s && s->alsa) {
        alsa_close(s->alsa);
        s->alsa = NULL;
    }
}

static void app_free(rdpsndDevicePlugin *dev)
{
    app_snd *s = (app_snd *)dev;
    if (!s)
        return;
    app_close(dev);
    free(s);
}

static BOOL app_default_format(rdpsndDevicePlugin *dev,
                               const AUDIO_FORMAT *desired,
                               AUDIO_FORMAT *def)
{
    if (!def)
        return FALSE;
    memset(def, 0, sizeof(*def));
    if (desired) {
        *def = *desired;
        return TRUE;
    }
    def->wFormatTag = WAVE_FORMAT_PCM;
    def->nChannels = 2;
    def->nSamplesPerSec = 48000;
    def->nAvgBytesPerSec = 48000 * 4;
    def->nBlockAlign = 4;
    def->wBitsPerSample = 16;
    return TRUE;
}

static UINT VCAPITYPE app_rdpsnd_entry(PFREERDP_RDPSND_DEVICE_ENTRY_POINTS p)
{
    if (!p || !p->pRegisterRdpsndDevice)
        return CHANNEL_RC_BAD_INIT_HANDLE;

    app_snd *s = calloc(1, sizeof(*s));
    if (!s)
        return CHANNEL_RC_NO_MEMORY;

    int card = 0, dev = 0;
    int have_dev = 0;
    if (p->args) {
        for (int i = 0; i < p->args->argc; i++) {
            const char *a = p->args->argv[i];
            if (!a)
                continue;
            if (strncmp(a, "dev:", 4) == 0) {
                const char *v = a + 4;
                if (sscanf(v, "c%dd%d", &card, &dev) == 2)
                    have_dev = 1;
                else if (sscanf(v, "%d,%d", &card, &dev) == 2)
                    have_dev = 1;
            }
        }
    }
    if (!have_dev)
        snprintf(s->pcm, sizeof(s->pcm), "hw:0,0");
    else
        snprintf(s->pcm, sizeof(s->pcm), "hw:%d,%d", card, dev);

    s->dev.FormatSupported = app_fmt_supported;
    s->dev.Open = app_open;
    s->dev.GetVolume = app_get_volume;
    s->dev.SetVolume = app_set_volume;
    s->dev.Play = app_play;
    s->dev.Close = app_close;
    s->dev.Free = app_free;
    s->dev.DefaultFormat = app_default_format;

    p->pRegisterRdpsndDevice(p->rdpsnd, (rdpsndDevicePlugin *)s);
    fprintf(stderr, "rdp audio: backend %s\n", s->pcm);
    return CHANNEL_RC_OK;
}

static PVIRTUALCHANNELENTRY app_addin_provider(LPCSTR name, LPCSTR subsystem,
                                                LPCSTR type, DWORD flags)
{
    if (name && subsystem && strcmp(name, "rdpsnd") == 0 &&
        strcmp(subsystem, "app") == 0) {
        union {
            PFREERDP_RDPSND_DEVICE_ENTRY fn;
            PVIRTUALCHANNELENTRY entry;
        } u;
        u.fn = app_rdpsnd_entry;
        return u.entry;
    }
    return freerdp_channels_load_static_addin_entry(name, subsystem, type, flags);
}

void rdp_audio_install_provider(void)
{
    freerdp_register_addin_provider(app_addin_provider, 0);
}

int rdp_audio_make_arg(const char *pcm, char *out, size_t cap)
{
    int card = 0, dev = 0;
    if (!pcm || !*pcm || !out || cap == 0)
        return -1;
    if (sscanf(pcm, "hw:%d,%d", &card, &dev) != 2)
        return -1;
    snprintf(out, cap, "/sound:sys:app,dev:c%dd%d,latency:50", card, dev);
    return 0;
}
