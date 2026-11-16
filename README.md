# Xash3D for GameCube <img align="right" width="128" height="128" src="https://github.com/FWGS/xash3d-fwgs/raw/master/game_launch/icon-xash-material.png" alt="Xash3D FWGS icon" />
[![GitHub Actions Status](https://github.com/FWGS/xash3d-fwgs/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/FWGS/xash3d-fwgs/actions/workflows/c-cpp.yml) [![FreeBSD Build Status](https://img.shields.io/cirrus/github/FWGS/xash3d-fwgs?label=freebsd%20build)](https://cirrus-ci.com/github/FWGS/xash3d-fwgs) \
[![Discord Server](https://img.shields.io/discord/355697768582610945?logo=Discord&label=International%20Discord%20chat)](http://xash.su/discord/) [![Russian speakers Telegram Chat](https://img.shields.io/badge/Russian_speakers_Telegram_chat-gray?logo=Telegram)](https://t.me/flyingwithgauss) \
[![Download Daily Build](https://img.shields.io/badge/downloads-testing-orange)](https://github.com/FWGS/xash3d-fwgs/releases/tag/continuous)

Xash3D ([pronounced](https://ipa-reader.com/?text=ks%C9%91%CA%82) `[ksɑʂ]`) FWGS is a game engine, aimed to provide compatibility with Half-Life Engine and extend it, as well as to give game developers well known workflow.

Xash3D FWGS is a heavily modified fork of an original [Xash3D Engine](https://www.moddb.com/engines/xash3d-engine) by Unkle Mike.

## Donate
[![Donate to FWGS button](https://img.shields.io/badge/Donate_to_FWGS-%3C3-magenta)](Documentation/donate.md) \
If you like Xash3D FWGS, consider supporting individual engine maintainers. By supporting us, you help to continue developing this game engine further. The sponsorship links are available in [documentation](Documentation/donate.md).

## Features
* Half-Life (HLSDK 2.5) and Day of Defeat game support on Nintendo GameCube (PowerPC, GX renderer).
* GameCube controller input, memory card saves and BBA networking (lwIP/libbba), including LAN/WAN server browsing.
* [GoldSrc protocol support](Documentation/goldsrc-protocol-support.md).
* Advanced virtual filesystem with ARAM-backed asset caching and streaming.
* Ogg Vorbis (`.ogg`) audio support.
* [A set of small improvements](Documentation/), without broken compatibility.

## Reporting issues
* Issues are accepted in both English and Russian.
* They are only accepted if you run legally acquired game data.

## Contributing code
* Check the CONTRIBUTING.md file.

## Build instructions
The build targets the GameCube; the section below describes it.

### GameCube

This fork includes a GameCube auto-porting pipeline built around the scripts in
[`scripts/`](scripts/). The preferred hardware loader path is **Swiss**, built
against Extrems' **libogc2** with **libdvm** for FAT/SD (Swiss-compatible
accessory stack). Classic stock `libogc` remains a fallback.

1. Install `devkitPro` with `devkitPPC`, then install the Swiss stack:
   `sudo (dkp-)pacman -S libogc2` and choose `libogc2-libdvm` when prompted for
   a `libogc2-libfat` provider. Ensure `python3` is available.
   Override with `XASH_GAMECUBE_OGC_STACK=libogc` only if you must use classic
   `$DEVKITPRO/libogc`.
2. Provide legally acquired Half-Life game data at `Half-Life/valve` if you want ISO builds,
   or copy assets to `sd:/xash3d/valve/` for direct `boot.dol` testing via Swiss.
3. Start the automation from the repo root with:
   `scripts/gamecube-autoport.sh`

Useful modes:

* `scripts/gamecube-autoport.sh goal-runner --chunk-passes 20 --recoverable-retries 8`
  runs the main evidence-gated port loop.
* `scripts/gamecube-autoport.sh reagent`
  runs one compile/probe classification pass and writes `.ai/reagent-last-probe.json`.
* `scripts/gamecube-autoport.sh supervisor`
  runs the phased engine-build, disc-build, and Dolphin boot flow.

Manual artifact builds are still available through `scripts/build-gamecube.sh`.

### Running tests

Tests are enabled with `--enable-tests` passed to `./waf configure` and can be run with `./waf --alltests`.

This builds both standalone unit tests and a separate engine test binary (`xash3d_tests`) that embeds engine-level tests. The engine test binary requires no game assets.
