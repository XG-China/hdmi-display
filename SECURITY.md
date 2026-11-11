# Security

HDMI Display runs as **root** and takes **DRM/KMS master** to drive the
displays directly. Treat the NAS and its web panel as sensitive systems.

## Please do not expose the panel to the public internet

- The HTTP panel listens on `0.0.0.0:8080` by default.
- Use a firewall / VLAN / VPN if you need remote access.
- Set `--web-token` if the panel is reachable by other users.
- The fnOS gateway socket is intended for the local fnOS desktop.

## Credentials

- RDP usernames and passwords are stored in the local configuration file.
- Protect `@appconf/hdmi-display` and any backup of it.
- VNC currently supports only **no authentication**. Prefer a local Unix
  socket (`/var/run/vms/*.vnc.sock`) or a trusted isolated network.

## Web source

- The web source runs Chromium with `--no-sandbox` because the app runs as
  root. Only load trusted URLs.
- Chromium and Xvfb are installed from the distribution repository; keep them
  updated with `apt`.

## Reporting a vulnerability

Please use GitHub's private security advisory feature for this repository
(“Security” → “Report a vulnerability”). Do not open a public issue for an
unfixed vulnerability.

Include:

- affected version / fpk file name,
- NAS model and GPU,
- reproduction steps,
- relevant logs or a minimal proof of concept.

## Supported versions

Security fixes are applied to the latest release on the `main` branch.
