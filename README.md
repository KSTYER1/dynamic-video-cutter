# Dynamic Video Cutter

Dynamic Video Cutter is a third-party OBS Studio filter plugin for media
sources.

It jumps through a video at configurable intervals to create a live-cut effect
for loops, ambience footage, IRL backgrounds, and long-form playback.

## Features

- Native OBS filter for media sources.
- Configurable cut interval and jump distance.
- Optional random variation per jump.
- Optional blend filter activation on each cut.
- Up to 5 playback regions with region-aware looping.
- Background pause when the source is not visible.
- Helper buttons for capturing the current playback position as a region start
  or end value.
- Locale files for English and German.

## Requirements

- OBS Studio 30.x, 31.x, or 32.x
- Windows x64 for the packaged release
- The packaged release is intended for media sources and VLC-based video
  workflows inside OBS

## Installation

### Windows

Download the release archive and extract or copy its contents into your OBS
Studio installation directory.

The final layout should include:

```text
obs-plugins/64bit/dynamic-video-cutter.dll
data/obs-plugins/dynamic-video-cutter/locale/en-US.ini
data/obs-plugins/dynamic-video-cutter/locale/de-DE.ini
```

Restart OBS after installation. The filter appears in the filter menu for
media sources.

## Basic Usage

1. Select a media source or VLC video source in OBS.
2. Open the source filters.
3. Add `Dynamic Video Cutter`.
4. Set the cut interval, jump distance, and optional random variation.
5. Optionally enable a blend filter for less abrupt visual cuts.
6. Optionally define playback regions to constrain where the jumps happen.

## Building from Source

Requires CMake 3.28 or newer, OBS development headers, and a supported
compiler toolchain.

```bash
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

The packaged Windows release is published separately as
`dynamic-video-cutter-1.0.1.zip`.

## Version History

### 1.0.1

- Fixed saved blend settings being hidden when reopening filter properties.
- Fixed enabled playback regions being hidden when reopening filter properties.
- Prevented the cutter from selecting and disabling itself as blend filter.
- Preserved the previous enabled state of the selected blend filter.
- Added protection against manual video seeking while the cutter is running.
- Ignored or clamped playback regions that do not fit the current video.

### 1.0.0

- Initial release.
- Ported the original Lua script into a native C plugin.
- Expanded playback regions from 3 to 5 loops.
- Added random jump behavior.
- Added automatic blend-filter selection support.
- Added current-position capture buttons.
- Added a more structured property UI.

## License

Dynamic Video Cutter is licensed under GPL-2.0-or-later.

## Disclaimer

Dynamic Video Cutter is an unofficial third-party plugin and is not affiliated
with or endorsed by the OBS Project.

AI-assisted tools were used during development and release preparation. The
maintainer is responsible for reviewing, testing, and publishing the released
plugin.
