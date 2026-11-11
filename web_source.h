#ifndef WEB_SOURCE_H
#define WEB_SOURCE_H

#include <stddef.h>

#include "input_router.h"

/*
 * Headless web page source.
 *
 * Linux: starts Xvfb (virtual X server, no desktop environment) plus
 * Chromium in kiosk mode, then lets FFmpeg's x11grab capture that display.
 * The resulting frames go through the normal video pipeline to DRM/KMS.
 *
 * Requires: xvfb, chromium (or chromium-browser / google-chrome).
 * Chromium runs with --no-sandbox (kiosk appliance, trusted URLs only).
 */
typedef struct web_proc web_proc;

web_proc *web_proc_start(const char *url, int width, int height,
                         char *err, size_t errsz);
int  web_proc_display(const web_proc *w);   /* X display number, e.g. 99 */
/* Forward a bound keyboard/mouse event into the page (XTEST). */
void web_proc_input(web_proc *w, const input_event *ev);
void web_proc_stop(web_proc *w);

#endif /* WEB_SOURCE_H */
