# LARK Next

**LARK Next** is an enhanced continuation of [LARK (Libre Audiobook Reader for Kindle)](https://github.com/kbarni/LARKPlayer), a free audiobook player for jailbroken Kindle devices.

It builds on the original project with an improved interface, Audiobookshelf integration, and additional playback and usability improvements.

> **Compatibility:** The original project currently documents support for Kindle firmware below 5.19. Check compatibility with your device before installing.

## Features

- MP3 and AAC-encoded M4B audiobook playback
- Audiobookshelf server integration
- Audiobook library browsing and book details
- Playback session and progress integration
- Listening history, bookmarks and chapter support
- Improved interface and on-screen keyboard
- E-ink display refresh handling
- Audio and cover caching improvements
- Backlight management

## Installation

Download the latest available package from the [LARK Next Releases](https://github.com/coderkitten123/LARK-Next/releases) page.

Extract the package to the root of your jailbroken Kindle and launch it using the included scriptlet or KUAL entry.

*If no release package is available yet, the project is still under development.*

## Audiobookshelf

LARK Next includes Audiobookshelf integration. You can configure your server connection in the application.

Your Kindle must have network access and be able to reach your Audiobookshelf server.

## Building

LARK Next uses CMake and C++11.

The project requires a suitable Kindle cross-compilation toolchain and the required Kindle libraries. In particular, the FAAD2 libraries and headers must be available to the toolchain.

For desktop UI development and testing, see [`preview/README.md`](preview/README.md) for the Linux host preview harness.

## Credits

LARK Next is based on the original [LARKPlayer](https://github.com/kbarni/LARKPlayer) project by Barna Keresztes.

The project also uses:

- [KinAMP](https://github.com/kbarni/KinAMP) audio engine
- [FAAD2](https://github.com/knik0/faad2) AAC decoder
- [miniaudio](https://github.com/mackron/miniaudio)
- [cJSON](https://github.com/DaveGamble/cJSON)

See the individual component licence and copyright notices in the source tree.

## Licence

LARK Next is licensed under the GNU General Public License v3 or later. See [LICENSE](LICENSE).
