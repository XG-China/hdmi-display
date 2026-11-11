/*
 * alsa_out.c - minimal ALSA playback through dlopen.
 *
 * Uses the one-call snd_pcm_set_params() API, so no alsa headers are needed
 * at build time and a missing libasound only disables audio.
 */
#define _GNU_SOURCE
#include "alsa_out.h"

#include <dlfcn.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct _snd_pcm snd_pcm_t;

#define SND_PCM_STREAM_PLAYBACK 0
#define SND_PCM_FORMAT_S16_LE 2
#define SND_PCM_ACCESS_RW_INTERLEAVED 3

struct alsa_out {
    void *lib;
    snd_pcm_t *pcm;
    int (*p_open)(snd_pcm_t **, const char *, int, int);
    int (*p_set_params)(snd_pcm_t *, int, int, unsigned, unsigned,
                        int, unsigned);
    long (*p_writei)(snd_pcm_t *, const void *, unsigned long);
    int (*p_recover)(snd_pcm_t *, int, int);
    int (*p_drain)(snd_pcm_t *);
    int (*p_close)(snd_pcm_t *);
    const char *(*p_strerror)(int);
    int channels;
};

static int parse_device(const char *dev, int *card, int *pcm)
{
    const char *p = dev;
    if (strncmp(p, "plughw:", 7) == 0)
        p += 7;
    else if (strncmp(p, "hw:", 3) == 0)
        p += 3;
    return sscanf(p, "%d,%d", card, pcm) == 2 ? 0 : -1;
}

/* "0000:10:00.0" -> "0000:10:00" */
static int pci_slot(const char *path, char *slot, size_t cap)
{
    char buf[512];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    char *p = strstr(buf, "0000:");
    if (!p)
        return -1;
    snprintf(slot, cap, "%.10s", p);   /* domain:bus:device, no function */
    return 0;
}

static int alsa_card_for_drm(int drm_card)
{
    char dev[64], slot[16];
    snprintf(dev, sizeof(dev), "/sys/class/drm/card%d/device", drm_card);
    if (pci_slot(dev, slot, sizeof(slot)) != 0)
        return -1;

    for (int ac = 0; ac < 16; ac++) {
        char snd[64], aslot[16];
        snprintf(snd, sizeof(snd), "/sys/class/sound/card%d/device", ac);
        if (pci_slot(snd, aslot, sizeof(aslot)) != 0)
            continue;
        if (strcmp(slot, aslot) == 0)
            return ac;
    }
    return -1;
}

static int hdmi_pcm_list(int card, int *devs, int max)
{
    char dir[64];
    snprintf(dir, sizeof(dir), "/proc/asound/card%d", card);
    DIR *d = opendir(dir);
    if (!d)
        return 0;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        int p;
        char kind;
        if (sscanf(e->d_name, "pcm%d%c", &p, &kind) != 2 || kind != 'p')
            continue;
        char info[512];
        snprintf(info, sizeof(info), "%s/%s/info", dir, e->d_name);
        FILE *f = fopen(info, "r");
        if (!f)
            continue;
        char line[256];
        int hdmi = 0;
        while (fgets(line, sizeof(line), f))
            if (strstr(line, "HDMI")) {
                hdmi = 1;
                break;
            }
        fclose(f);
        if (hdmi) {
            int i = n++;
            while (i > 0 && devs[i - 1] > p) {
                devs[i] = devs[i - 1];
                i--;
            }
            devs[i] = p;
        }
    }
    closedir(d);
    return n;
}

static int eld_info(const char *path, int *present, char *name, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[256];
    *present = 0;
    name[0] = 0;
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "monitor_present %d", &v) == 1)
            *present = v;
        else if (strncmp(line, "monitor_name", 12) == 0) {
            char *p = line + 12;
            while (*p == ' ' || *p == '\t')
                p++;
            size_t n = strlen(p);
            while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r' ||
                             p[n - 1] == ' ' || p[n - 1] == '\t'))
                p[--n] = 0;
            snprintf(name, cap, "%s", p);
        }
    }
    fclose(f);
    return 0;
}

static int drm_monitor_name(int drm_card, const char *conn, char *out, size_t cap)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/drm/card%d-%s/edid",
             drm_card, conn);
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    unsigned char edid[128];
    size_t got = fread(edid, 1, sizeof(edid), f);
    fclose(f);
    if (got < sizeof(edid))
        return -1;

    for (int i = 0; i < 4; i++) {
        const unsigned char *d = edid + 0x36 + i * 18;
        if (d[0] || d[1] || d[2] || d[3] != 0xfc)
            continue;
        char name[16];
        int n = 0;
        for (int k = 5; k < 18 && d[k]; k++)
            name[n++] = (char)d[k];
        while (n > 0 && (name[n - 1] == ' ' || name[n - 1] == '\n' ||
                         name[n - 1] == '\r' || name[n - 1] == '\t'))
            n--;
        name[n] = 0;
        if (n > 0) {
            snprintf(out, cap, "%s", name);
            return 0;
        }
    }
    return -1;
}

int alsa_detect_for_drm_conn(int drm_card, const char *conn, char *out,
                             size_t cap)
{
    int ac = alsa_card_for_drm(drm_card);
    if (ac < 0)
        return -1;

    int devs[8];
    int nd = hdmi_pcm_list(ac, devs, 8);
    if (nd <= 0)
        return -1;

    char want[32] = "";
    if (conn)
        drm_monitor_name(drm_card, conn, want, sizeof(want));

    int first_present = -1;
    for (int idx = 0; idx < 8; idx++) {
        char p[80];
        snprintf(p, sizeof(p), "/proc/asound/card%d/eld#0.%d", ac, idx);
        int present;
        char name[32];
        if (eld_info(p, &present, name, sizeof(name)) != 0)
            break;   /* no more ELD entries */
        if (!present)
            continue;
        if (first_present < 0)
            first_present = idx;
        if (want[0] && name[0] && strcmp(want, name) == 0 && idx < nd) {
            snprintf(out, cap, "hw:%d,%d", ac, devs[idx]);
            return 0;
        }
    }
    /* identical monitor names or unparsable EDID: use the first live pin */
    if (first_present >= 0 && first_present < nd) {
        snprintf(out, cap, "hw:%d,%d", ac, devs[first_present]);
        return 0;
    }
    return -1;
}

static void alsa_enable_iec958(int card)
{
    /* Each HDMI/DP converter has its own "IEC958,<n>" switch.  The old code
     * only enabled indexes 0/1; on this GPU the connected HDMI output is
     * converter 2, so its switch stayed off and the PCM ran silently. */
    char cmd[1024];
    int off = 0;
    for (int i = 0; i < 8; i++) {
        int n = snprintf(cmd + off, sizeof(cmd) - (size_t)off,
                         "amixer -c %d sset 'IEC958',%d on >/dev/null 2>&1; ",
                         card, i);
        if (n > 0)
            off += n;
        if (off >= (int)sizeof(cmd) - 80)
            break;
    }
    if (off > 0) {
        int rc = system(cmd);
        (void)rc;
    }
}

static alsa_out *alsa_open_impl(const char *device, int rate, int channels,
                                unsigned latency_us)
{    if (!device || !*device || strcmp(device, "off") == 0)
        return NULL;

    int card = 0, pcm = 0;
    if (parse_device(device, &card, &pcm) != 0)
        return NULL;

    void *lib = dlopen("libasound.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!lib)
        lib = dlopen("libasound.so", RTLD_LAZY | RTLD_LOCAL);
    if (!lib)
        return NULL;

    alsa_out *a = calloc(1, sizeof(*a));
    if (!a) {
        dlclose(lib);
        return NULL;
    }
    a->lib = lib;
    a->channels = channels;
    *(void **)(&a->p_open) = dlsym(lib, "snd_pcm_open");
    *(void **)(&a->p_set_params) = dlsym(lib, "snd_pcm_set_params");
    *(void **)(&a->p_writei) = dlsym(lib, "snd_pcm_writei");
    *(void **)(&a->p_recover) = dlsym(lib, "snd_pcm_recover");
    *(void **)(&a->p_drain) = dlsym(lib, "snd_pcm_drain");
    *(void **)(&a->p_close) = dlsym(lib, "snd_pcm_close");
    *(void **)(&a->p_strerror) = dlsym(lib, "snd_strerror");
    if (!a->p_open || !a->p_set_params || !a->p_writei ||
        !a->p_recover || !a->p_drain || !a->p_close) {
        dlclose(lib);
        free(a);
        return NULL;
    }

    /* The HDA HDMI codec keeps its IEC958 output switch off by default;
     * with it off the PCM runs but the TV receives silence. */
    alsa_enable_iec958(card);

    char devname[64];
    snprintf(devname, sizeof(devname), "hw:%d,%d", card, pcm);
    int rc = a->p_open(&a->pcm, devname, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        if (a->p_strerror)
            fprintf(stderr, "audio: %s: %s\n", devname, a->p_strerror(rc));
        dlclose(lib);
        free(a);
        return NULL;
    }
    rc = a->p_set_params(a->pcm, SND_PCM_FORMAT_S16_LE,
                         SND_PCM_ACCESS_RW_INTERLEAVED, (unsigned)channels,
                         (unsigned)rate, 1, latency_us);
    if (rc < 0) {
        if (a->p_strerror)
            fprintf(stderr, "audio: %s: %s\n", devname, a->p_strerror(rc));
        a->p_close(a->pcm);
        dlclose(lib);
        free(a);
        return NULL;
    }
    return a;
}

alsa_out *alsa_open_latency(const char *device, int rate, int channels,
                            unsigned latency_us)
{
    return alsa_open_impl(device, rate, channels, latency_us);
}

alsa_out *alsa_open(const char *device, int rate, int channels)
{
    return alsa_open_impl(device, rate, channels, 200000);
}

int alsa_write(alsa_out *a, const void *buf, int frames)
{
    if (!a || !a->pcm || frames <= 0)
        return 0;
    const char *p = buf;
    int left = frames;
    int retries = 0;
    while (left > 0) {
        long n = a->p_writei(a->pcm, p, (unsigned long)left);
        if (n < 0) {
            /* A dead/unplugged HDMI PCM must not spin the audio thread. */
            if (++retries > 8)
                return -1;
            int r = a->p_recover(a->pcm, (int)n, 1);
            if (r < 0)
                return -1;
            continue;
        }
        if (n == 0) {
            if (++retries > 8)
                return -1;
            continue;
        }
        retries = 0;
        left -= (int)n;
        p += (size_t)n * (size_t)a->channels * 2;
    }
    return frames;
}

void alsa_close(alsa_out *a)
{
    if (!a)
        return;
    if (a->pcm) {
        a->p_drain(a->pcm);
        a->p_close(a->pcm);
    }
    if (a->lib)
        dlclose(a->lib);
    free(a);
}
