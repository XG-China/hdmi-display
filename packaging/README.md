# Fygo OS package (hdmi-display.fpk)

This directory turns the Linux `hdmi_display` binary into a Fygo OS application
that can be installed from App Center.

## Build

On a Linux build machine (or on the NAS itself):

```bash
sudo apt install build-essential pkg-config libdrm-dev \
    freerdp3-dev winpr3-dev libssl-dev libx11-dev libxtst-dev \
    nasm libva-dev libxext-dev libxfixes-dev libxv-dev \
    libxcb1-dev libxcb-shm0-dev libxcb-shape0-dev libxcb-xfixes0-dev \
    zlib1g-dev

# Minimal FFmpeg (only the codecs/demuxers/protocols x11grab uses)
./packaging/build-ffmpeg-min.sh        # -> /opt/ffmpeg-min

# fygopack
curl -L -o /tmp/fygopack \
  https://static2.fygonas.com/fygopack/fygopack-1.2.3-linux-amd64
chmod +x /tmp/fygopack && sudo mv /tmp/fygopack /usr/local/bin/fygopack

./packaging/build-fpk.sh
# -> build/hdmi-display.fpk
```

`build-fpk.sh` uses `/opt/ffmpeg-min` (override with `FFMPEG_PREFIX=...`).
The default `BUNDLE_LIBS=1` copies the minimal FFmpeg, FreeRDP, OpenSSL,
ALSA, libdrm, X11/XCB and their complete dependency closure into `app/lib`;
the generated unit adds `LD_LIBRARY_PATH`. The target NAS does not need
FFmpeg/FreeRDP development or runtime packages.

## Install

App Center → Manual installation → `hdmi-display.fpk`.

The install wizard asks for:

- **Web panel port** (default 8080) — the app's built-in web UI.
- **Stop appliance-compositor while running** — see below.
- **Web-page dependencies** — optionally run `apt-get install xvfb chromium`
  (about 300 MB). The fnOS App Center does not provide these packages. The
  same option is available later in the app's configuration wizard; if it is
  skipped, web sources show the exact apt command in the panel.

The app appears in the Fygo OS desktop as **HDMI Display** (administrators
only). Its window is an iframe of the web panel.

Lifecycle (matches fnOS button semantics):

- A fresh **install starts the app**: App Center shows **Open**.
- **Stop** runs `cmd/main stop` (`systemctl stop` + `disable`): the app stops,
  does not run in the background and does not autostart; App Center shows
  **Enable**.
- **Enable/start** runs `cmd/main start` (`systemctl enable` + `restart`):
  App Center shows **Open** again.
- **Upgrade/config changes preserve the previous state**: a running app is
  restarted, a stopped app stays stopped. Only the user's Stop switches to
  Enable.
- **Wizard settings persist**: the port and appliance-compositor policy are
  stored in `@appconf/hdmi-display/install.env`, so an App Center upgrade that
  does not pass the install environment keeps them. The config wizard's
  "keep the current setting" option changes the port without resetting the
  compositor policy.
- Uninstall always removes the unit, drop-ins, socket, runtime directory and
  temporary Chromium profiles. The uninstall wizard defaults to also removing
  the configuration, app data and app directory (`wizard_remove_data=remove`);
  choose "Keep" to preserve them.

## Important: DRM master vs. the TV compositor

`hdmi_display` drives the HDMI/DP connectors through **DRM/KMS directly**
(connector → CRTC → plane). Only one process can be DRM master.

- On a NAS without a local compositor (no Fygo Cinema / TV app), the app takes
  the displays and works as a wall.
- If **appliance-compositor** is running (Fygo Cinema and other local TV apps
  are its windows), it already owns DRM master. `hdmi_display` cannot start
  until it is stopped.

The install/config wizard's **“Stop appliance-compositor while running”**
switch makes the systemd unit:

```
Conflicts=appliance-compositor.service
ExecStartPre=-systemctl stop appliance-compositor.service
ExecStopPost=-systemctl start appliance-compositor.service
```

So while HDMI Display runs, Fygo Cinema and other TV windows are stopped; when
HDMI Display stops, the compositor is started again. This is a deliberate
trade-off: the two cannot share the GPU's display pipelines without adding a
Wayland backend to `hdmi_display`.

If you need both at once, run `hdmi_display` on a second machine, or add a
Wayland/GBM backend and become a client of `appliance-compositor` (not included
in this version).

## Verification status

Tested on a real fnOS 1.2.3 NAS (Debian 12 bookworm, trim kernel, AMD GPU):

- `appcenter-cli install-fpk build/hdmi-display.fpk` installs, shows **running**
  in `appcenter-cli list`, web panel answers on the chosen port;
- `appcenter-cli start/stop` work; the systemd unit points at
  `/usr/local/apps/@appcenter/hdmi-display/bin/hdmi_display` and
  `/usr/local/apps/@appconf/hdmi-display/hdmi_display.conf`;
- DRM output works on the connected `HDMI-A-1` (test pattern, 1360x768@60);
- `/api/state`, `/api/apply` (hot switch ui/bars), `/preview/<connector>.mjpg`
  and config persistence all work;
- uninstall from `appcenter-cli` is rejected by fnOS ("please uninstall from
  Web UI") — uninstall from App Center instead.

### Lifecycle gotcha

`cmd/install_init` runs **before** App Center extracts `app.tgz` into
`$TRIM_APPDEST`, so it must not check for the binary. System prerequisites
(`/dev/dri`) are checked there; the binary and `ldd` check happen in
`cmd/install_callback`, after extraction.

## Permissions

`config/privilege` uses `run-as: root`. The app needs:

- DRM master: `/dev/dri/card0`, `drmSetMaster`;
- input devices: `/dev/input/*` for the keyboard/mouse → VNC binding;
- `Xvfb` / `Chromium` for web-page sources (Chromium runs with
  `--no-sandbox` as root).

This is an appliance-style app; run it on a trusted LAN. The web panel itself
has no login (it is the local admin UI). Use a firewall/VPN or `--web-token`
in the unit if the NAS is exposed.

## Runtime dependencies

- Fygo OS 1.2+ / Debian 12, x86_64.
- A GPU with DRM/KMS (Intel/AMD recommended).
- The fpk bundles its minimal FFmpeg, FreeRDP, OpenSSL, ALSA, libdrm and
  X11/XCB closure; the host needs glibc and the kernel DRM drivers.
- **Web sources**: `xvfb` and `chromium` must be installed on the NAS.
- Optional: VNC servers for VM consoles (None auth only).

## Files

| path | what |
|---|---|
| `app/bin/hdmi_display` | the C binary (copied by the build script) |
| `app/bin/amixer` | bundled ALSA control helper for HDMI IEC958 |
| `app/bin/enable-rdp.ps1` | Windows RDP 60 fps helper, served by the panel |
| `app/lib` | minimal FFmpeg/FreeRDP/OpenSSL/ALSA/DRM/X11 closure |
| `app/web` | static admin panel (index/style/app) |
| `app/ui/config` | App Center entry (iframe, admin only) |
| `cmd/common` | unit generation and start/stop helpers |
| `cmd/main` | App Center start/stop/status |
| `cmd/install_*`, `upgrade_*`, `config_*`, `uninstall_*` | lifecycle |
| `wizard/*` | install/config/uninstall forms |
| `config/privilege` | `run-as: root` |
| `config/resource` | `hdmi-display/media` shared directory |
| `i18n/en-US` | App Center name/description |

## Configuration inside the NAS

- Config file: `$TRIM_PKGETC/hdmi_display.conf`
  (usually `/var/apps/hdmi-display/etc/hdmi_display.conf`).
- Every connected output starts on the test pattern. Open the web panel and
  choose a source for each screen.
- The web panel saves output sources, input bindings and the web port back to
  the config file.
- The `hdmi_display.conf` is preserved across upgrades.

Shared media directory: `/var/apps/hdmi-display/share/media`
(`hdmi-display/media` in the file manager). Point image/video sources there.

## Uninstall

The uninstall wizard can also remove the configuration and saved sources.
The systemd unit is always removed; the compositor is restored when the
service stops.
