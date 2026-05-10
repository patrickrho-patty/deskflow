# Deskflow

## Custom macOS and Linux Build Changes

This fork includes a custom Deskflow build focused on reliable macOS-to-Linux sharing with Logitech mice:

- Added macOS `deskflow-core` HID capture for mouse buttons 4 and 5 so Logitech back/forward buttons are forwarded to remote clients instead of being swallowed by Logi Options Plus.
- Added local macOS handling for those same side buttons, mapping them to Command-Left and Command-Right when the pointer is on the Mac server.
- Added a macOS server UI checkbox, shown when Logi Options Plus with an MX Master 3S profile is detected, to switch Logi Options back/forward assignments to raw mouse buttons 4 and 5. The app backs up `settings.db` and restores the original per-slot macros when the checkbox is turned off.
- Added the macOS Input Monitoring usage string needed for side-button capture.
- Added a reusable Flatpak build script at `deploy/linux/flatpak/build-linux-flatpak.sh` for producing Linux bundles from macOS, including x86_64 builds for Linux Mint/Ubuntu-derived systems.
- Updated the Flatpak manifest to bundle the needed `libei` and `libportal` libraries in the Flatpak runtime so the Linux client can run without missing shared-library errors.

Deskflow is a free and open source keyboard and mouse sharing app. Use the keyboard, mouse, or trackpad of one computer to control nearby computers, and work seamlessly between them.

[Homepage](https://deskflow.org) [Code](https://github.com/deskflow/deskflow)

## Getting Help Online

- View the [wiki](https://github.com/deskflow/deskflow/wiki) online resource.

### Chat With Us

- Main discussion on Matrix: [`#deskflow:matrix.org`](https://matrix.to/#/#deskflow:matrix.org) ([Matrix clients](https://matrix.org/ecosystem/clients/)).
- Discussion also happens on IRC: `#deskflow` or `#deskflow-dev` on [Libera Chat](https://libera.chat/).
- Start a [new discussion](https://github.com/deskflow/deskflow/discussions) on the upstream GitHub project.

## Reporting Security Issues

Check [Security](docs/Security.md) to find out how to report security issues.
