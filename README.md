# SpeedCrunch for macOS (Apple Silicon & Intel)

[SpeedCrunch](https://speedcrunch.org) has been my daily calculator of choice for many years. The
last official Mac release (0.12, 2016) is Intel-only and blurry on Retina screens, so I built
current versions for modern Macs, with the help of Claude Code. Not affiliated with the upstream
project.

This build fixes the font rendering on Retina displays (using the
[fix](https://github.com/yifany-github/speedcrunch_highResolusion_mac) already present in the
[original](https://bitbucket.org/heldercorreia/speedcrunch/commits/928c22c0ba21974c0f87201f24a4e5f6de05fb77)
source) and adds an optional Classic Appearance toggle for a more compact interface.

## Download

**[Get the latest release](https://github.com/roberthoegerl/speedcrunch-macos/releases/latest)**
(on GitHub, downloads are under "Releases", on the right of the page).

- Apple Silicon (M1/M2/M3/M4): `...-macOS-arm64.dmg`
- Intel: `...-macOS-x86_64.dmg`

## First launch

As I don't have an Apple Developer account, this release is not notarized by Apple, so to open right-click the app, choose Open, then Open again. This extra step is necessary once only.

## What's different from 0.12

- Native Apple Silicon (arm64), no Rosetta needed.
- Retina text-rendering fix included (it was in the source but not in the 0.12 download).
- Built from the current 1.0 codebase, plus an optional Classic Appearance toggle (off by default).

## Build

Version 1.0 (`1.0-604-g3aa5f060`), Qt 6.11.2, requires macOS 13 or newer.

Based on upstream `master` commit
[`b598d91d`](https://bitbucket.org/heldercorreia/speedcrunch/commits/b598d91db29333eb3536ff0fe86c2abcfe5c6ad8).

Compiled and published September 2026.

## License

GPL v2 ([LICENSE](LICENSE)). Full source is in this repo. SpeedCrunch is by Helder Correia and
contributors ([source](https://bitbucket.org/heldercorreia/speedcrunch)); this is an independent
build by [@roberthoegerl](https://github.com/roberthoegerl).
