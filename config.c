/*
 * config.c - line based config, easy to edit by hand:
 *
 *   web_port=8080
 *   output HDMI-A-1 media 1920x1080 rtsp://192.168.1.10/live
 *   output HDMI-A-2 ui 0x0
 *   output HDMI-A-3 image 0x0 /data/wallpaper.jpg
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void config_init(app_config *c, const char *path)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->path, sizeof(c->path), "%s", path && *path ? path : "hdmi_display.conf");
    snprintf(c->audio, sizeof(c->audio), "auto");
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    return s;
}

int config_load(app_config *c)
{
    FILE *f = fopen(c->path, "r");
    if (!f)
        return -1;

    c->count = 0;
    c->input_count = 0;
    char line[1400];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == '#')
            continue;

        if (strncmp(s, "web_port=", 9) == 0) {
            c->web_port = atoi(s + 9);
            continue;
        }
        if (strncmp(s, "audio=", 6) == 0) {
            snprintf(c->audio, sizeof(c->audio), "%s", s + 6);
            continue;
        }
        if (strncmp(s, "input ", 6) == 0) {
            char dev[256] = "", out[64] = "";
            if (sscanf(s + 6, "%255s %63s", dev, out) == 2 &&
                c->input_count < CFG_MAX_INPUTS) {
                snprintf(c->inputs[c->input_count].device,
                         sizeof(c->inputs[0].device), "%s", dev);
                snprintf(c->inputs[c->input_count].output,
                         sizeof(c->inputs[0].output), "%s", out);
                c->input_count++;
            }
            continue;
        }
        if (strncmp(s, "output ", 7) != 0)
            continue;
        if (c->count >= CFG_MAX_OUTPUTS)
            break;

        char name[64] = "", type[16] = "", res[32] = "", arg[1024] = "";
        int n = sscanf(s + 7, "%63s %15s %31s %1023[^\n]", name, type, res, arg);
        if (n < 3)
            continue;

        cfg_output *o = &c->out[c->count];
        memset(o, 0, sizeof(*o));
        snprintf(o->name, sizeof(o->name), "%s", name);
        snprintf(o->type, sizeof(o->type), "%s", type);
        sscanf(res, "%dx%d", &o->width, &o->height);
        snprintf(o->arg, sizeof(o->arg), "%s", trim(arg));
        c->count++;
    }
    fclose(f);
    return c->count;
}

int config_save(const app_config *c)
{
    /* Write a sibling temp file and rename it into place, so a crash or power
     * loss never leaves a truncated config behind. */
    char tmp[sizeof(c->path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", c->path);

    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;

    fprintf(f, "# HDMI Display configuration\n");
    fprintf(f, "web_port=%d\n", c->web_port);
    fprintf(f, "audio=%s\n", c->audio[0] ? c->audio : "auto");
    for (int i = 0; i < c->count; i++) {
        const cfg_output *o = &c->out[i];
        if (o->arg[0])
            fprintf(f, "output %s %s %dx%d %s\n", o->name, o->type,
                    o->width, o->height, o->arg);
        else
            fprintf(f, "output %s %s %dx%d\n", o->name, o->type,
                    o->width, o->height);
    }
    for (int i = 0; i < c->input_count; i++)
        fprintf(f, "input %s %s\n", c->inputs[i].device, c->inputs[i].output);

    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        remove(tmp);
        return -1;
    }
    if (fclose(f) != 0) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, c->path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

void config_set(app_config *c, const char *name, const char *type,
                const char *arg, int width, int height)
{
    cfg_output *o = NULL;
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->out[i].name, name) == 0) {
            o = &c->out[i];
            break;
        }
    }
    if (!o) {
        if (c->count >= CFG_MAX_OUTPUTS)
            return;
        o = &c->out[c->count++];
        memset(o, 0, sizeof(*o));
        snprintf(o->name, sizeof(o->name), "%s", name);
    }
    snprintf(o->type, sizeof(o->type), "%s", type ? type : "bars");
    snprintf(o->arg, sizeof(o->arg), "%s", arg ? arg : "");
    o->width = width;
    o->height = height;
}

void config_remove(app_config *c, const char *name)
{
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->out[i].name, name) == 0) {
            c->out[i] = c->out[c->count - 1];
            c->count--;
            return;
        }
    }
}

void config_bind_input(app_config *c, const char *device, const char *output)
{
    for (int i = 0; i < c->input_count; i++) {
        if (strcmp(c->inputs[i].device, device) == 0) {
            snprintf(c->inputs[i].output, sizeof(c->inputs[0].output), "%s",
                     output ? output : "");
            return;
        }
    }
    if (c->input_count < CFG_MAX_INPUTS) {
        cfg_input *in = &c->inputs[c->input_count++];
        snprintf(in->device, sizeof(in->device), "%s", device);
        snprintf(in->output, sizeof(in->output), "%s", output ? output : "");
    }
}

void config_unbind_input(app_config *c, const char *device)
{
    for (int i = 0; i < c->input_count; i++) {
        if (strcmp(c->inputs[i].device, device) == 0) {
            c->inputs[i] = c->inputs[c->input_count - 1];
            c->input_count--;
            return;
        }
    }
}