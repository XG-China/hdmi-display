# Linux build (DRM/KMS).
#
#   make                               use the system FFmpeg
#   make FFMPEG_PREFIX=/opt/ffmpeg-min use the minimal FFmpeg build
#   make clean
#
# Build dependencies (Debian 12 / fnOS):
#   sudo apt install build-essential pkg-config libdrm-dev \
#        freerdp3-dev winpr3-dev libssl-dev libx11-dev libxtst-dev
#
# The minimal FFmpeg (only the codecs/demuxers/protocols this app uses) is
# built by packaging/build-ffmpeg-min.sh into /opt/ffmpeg-min.

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -pthread -MMD -MP

PKGS := libdrm libavformat libavcodec libavutil libswscale libswresample \
        freerdp-client3 freerdp3 winpr3 libcrypto x11 xtst

FF_CFLAGS :=
FF_LIBS   :=
ifneq ($(FFMPEG_PREFIX),)
  FF_CFLAGS := -I$(FFMPEG_PREFIX)/include
  FF_LIBS   := -L$(FFMPEG_PREFIX)/lib
endif

CFLAGS  += $(FF_CFLAGS) $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(FF_LIBS) $(shell pkg-config --libs $(PKGS)) -pthread -lm -ldl

DISPLAY_SRC := drm_display.c
SRCS := main.c draw.c font8x16.c font_cjk.c video_dec.c vnc_source.c \
        rdp_source.c rdp_audio.c web_source.c input_router.c web_server.c \
        os_thread.c platform.c config.c alsa_out.c $(DISPLAY_SRC)
OBJS := $(SRCS:.c=.o)
BIN  := hdmi_display

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN) *.d

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)/usr/local/bin/$(BIN)

-include $(OBJS:.o=.d)

.PHONY: all clean install
