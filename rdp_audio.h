#ifndef RDP_AUDIO_H
#define RDP_AUDIO_H

#include <stddef.h>

/* Install a private FreeRDP rdpsnd backend ("app") that plays remote audio
 * through alsa_out.  Call after freerdp_client_context_new(). */
void rdp_audio_install_provider(void);

/* Build "/sound:sys:app,dev:cXDev" for an ALSA "hw:X,Y" PCM. Returns 0 on
 * success. The comma-free token avoids FreeRDP's /sound argument separator. */
int rdp_audio_make_arg(const char *pcm, char *out, size_t cap);

#endif
