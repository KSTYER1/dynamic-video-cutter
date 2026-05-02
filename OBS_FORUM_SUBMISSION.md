# OBS Forum Submission Draft: Dynamic Video Cutter

## Resource Title

Dynamic Video Cutter

## Version

1.0.1

## Category

OBS Studio Plugins

## Tags

media source, video, filter, loop, playback

## Short Tagline

Loop selected regions of media sources with automatic jump and blend behavior.

## Supported Bit Versions

64-bit

## Supported Platforms

Windows

## Minimum OBS Studio Version

30.0.0

## Source Code URL

https://github.com/KSTYER1/dynamic-video-cutter

## Download URL

https://github.com/KSTYER1/dynamic-video-cutter/releases/tag/v1.0.1

## Overview

Dynamic Video Cutter is an unofficial third-party OBS Studio filter plugin for
media sources. It lets you define playback regions and automatically jumps
within those regions while a media source is playing.

## Features

- Up to five configurable playback regions.
- Looping or random jump behavior.
- Optional blend filter activation during jumps.
- Current-position capture buttons.
- Protection for manual seeking and source duration changes.
- English and German UI text.

## Installation

Download the Windows x64 release archive and extract or copy its contents into
your OBS Studio installation directory.

The final layout should include:

```text
obs-plugins/64bit/dynamic-video-cutter.dll
data/obs-plugins/dynamic-video-cutter/locale/en-US.ini
data/obs-plugins/dynamic-video-cutter/locale/de-DE.ini
```

Restart OBS after installation. The filter appears in the filter menu for media
sources.

## License

GPL-2.0-or-later.

## Disclaimer

Dynamic Video Cutter is an unofficial third-party plugin and is not affiliated
with or endorsed by the OBS Project.

AI-assisted tools were used during development and release preparation. The
maintainer is responsible for reviewing, testing, and publishing the released
plugin.

## Pre-Submit Checklist

- [x] Public GitHub repository exists.
- [x] README is visible on GitHub.
- [x] GPL license is visible on GitHub.
- [x] Source Code URL field points to the repository.
- [x] Release ZIP is attached to GitHub Releases or uploaded to the forum.
- [ ] At least one screenshot/GIF is added to the resource description.
- [x] Description is in English.
- [x] No OBS logo is used as resource icon or marketing artwork.
