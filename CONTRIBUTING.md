# Contributing

Thanks for helping test HDMI Display.

## Build

The reproducible build is in [`build-kit`](build-kit/README.md):

```bash
bash build-kit/build-in-docker.sh
```

or on Debian 12 / fnOS:

```bash
sudo bash build-kit/setup-debian.sh
bash packaging/build-fpk.sh
```

Please keep the build warning-free with `-Wall -Wextra`.

## Code map

See [`docs/CODE.md`](docs/CODE.md) for the full per-file code guide:
architecture, thread model, source adapters, DRM/KMS backend, web API,
configuration, packaging and extension points.

## Issues

For a useful bug report, include:

- HDMI Display version / fpk name
- NAS model, CPU/GPU, number and type of video outputs
- source type: video, image, RDP, VNC, web, PiP
- `journalctl -u hdmi-display --no-pager -n 100`
- whether `appliance-compositor` is running

Security issues: see [SECURITY.md](SECURITY.md), do not open a public issue.

## Commits

- Keep changes focused.
- Match the existing C11 style and Chinese/English UI wording.
- Add a `Signed-off-by:` line to commits (Developer Certificate of Origin).
- By contributing you agree that your contribution is licensed under
  GPL-3.0-or-later.
