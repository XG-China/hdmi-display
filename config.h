/*
 * config.h - tiny persistent configuration (one output per line).
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#define CFG_MAX_OUTPUTS 16
#define CFG_MAX_INPUTS  16

typedef struct {
    char name[64];      /* connector / monitor */
    char type[16];      /* ui | bars | media | image | vnc | screen | pip */
    char arg[1024];     /* url / path / pip spec */
    int  width;
    int  height;
} cfg_output;

typedef struct {
    char device[256];   /* stable input device id */
    char output[64];    /* bound output name */
} cfg_input;

typedef struct {
    int web_port;
    char audio[64];     /* "auto" | "off" | "hw:C,D" for media audio */
    int count;
    cfg_output out[CFG_MAX_OUTPUTS];
    int input_count;
    cfg_input inputs[CFG_MAX_INPUTS];
    char path[512];
} app_config;

void config_init(app_config *c, const char *path);
int  config_load(app_config *c);          /* number of outputs, -1 if missing */
int  config_save(const app_config *c);    /* 0 on success */
void config_set(app_config *c, const char *name, const char *type,
                const char *arg, int width, int height);
void config_remove(app_config *c, const char *name);
void config_bind_input(app_config *c, const char *device, const char *output);
void config_unbind_input(app_config *c, const char *device);

#endif /* CONFIG_H */
