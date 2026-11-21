# LARK Next

LARK Next is a free audiobook player for jailbroken Kindle devices. It continues the [LARKPlayer project](https://github.com/kbarni/LARKPlayer) with a refreshed interface, Audiobookshelf support, and playback improvements.


**WARNING:** *AI tools assisted with parts of the code review, bug fixes, and documentation for LARK Next. The project was reviewed and tested by the maintainer, including on a Kindle device.*

## Features

- Play MP3 and AAC audio in M4B files
- Browse Audiobookshelf libraries and book details
- Sync playback sessions and listening progress with Audiobookshelf
- Download Audiobookshelf books for offline listening
- Use local audiobook files, history, chapters, and bookmarks
- Browse recent local books while connected to Audiobookshelf
- Control playback with Bluetooth play/pause and 10-second seek buttons
- Configure display refresh, backlight, and sleep behavior

## Compatibility

The original LARK project documents support for Kindle firmware earlier than 5.19. This repository's supplied cross-compilation toolchain builds a 32-bit ARM hard-float executable for compatible Kindle firmware. The executable is dynamically linked and requires the Kindle runtime libraries, including GTK, GStreamer, FAAD2, and LIPC.

Check the release notes and confirm your device model and firmware are supported before installing. The build in this repository is not a universal Kindle binary.

## Install

Download a release from [GitHub Releases](https://github.com/coderkitten123/LARK-Next/releases)
Extract the package to `/mnt/us/` , the contents of `documents` and `extensions` to their respective places on the Kindle and launch LARK from KUAL (or KUAL-Next) or the included launcher booklet. The launcher expects the executable to be named `larkplayer` for hard-float firmware.

If there is no release package yet, the repository contains source code rather than a ready-to-install bundle. A complete package must include the executable, launcher/KUAL files, and any runtime libraries that are not already present on the target Kindle.

## Audiobookshelf setup

Connect the Kindle to Wi-Fi and ensure it can reach your Audiobookshelf server. In LARK's Options screen, enable Audiobookshelf and enter the server address plus either:

- a username and password, or
- an Audiobookshelf API key.

Use an `http://` address. The current client does not support HTTPS. Tap **Test connection** to check the settings before browsing the library.

## Build

The supplied `armhf-toolchain.cmake` targets Kindle hard-float. It expects the `kindlehf` cross toolchain and target libraries for GTK2, GStreamer 0.10, FAAD2, LIPC, SQLite, and libxml2. See the comments in the toolchain file for its expected location and SDK setup.

Activate the `kindlehf` environment using `refs/x-compile.sh` from your koxtoolchain checkout (the script is not part of this repository). Then, from the repository root, configure and build with:

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=armhf-toolchain.cmake
cmake --build build -j2
```

The executable is written to `build/larkplayer`. This build uses the ARM hard-float ABI; it is not suitable for soft-float Kindle models.

For Linux desktop UI preview, see [`preview/README.md`](preview/README.md).

## Project files

- `assets/start_lark.sh` — Kindle launcher script
- `assets/kual/` — KUAL menu and extension metadata
- `armhf-toolchain.cmake` — hard-float Kindle cross-compilation configuration
- `preview/` — desktop preview harness and fake Audiobookshelf server

## Credits and licences

LARK Next is based on [LARKPlayer](https://github.com/kbarni/LARKPlayer) by Barna Keresztes. It also uses KinAMP, FAAD2, miniaudio, and cJSON. See the relevant source directories for their copyright and licence notices.

LARK Next is licensed under GNU GPL version 3 or, at your option, any later version. See [LICENSE](LICENSE).
