# Dynamic Video Cutter

Dynamic Video Cutter is a native OBS Studio filter plugin for media sources.
It jumps through a video at configurable intervals to create a live-cut effect
for loops, ambience footage, and long-form playback.

## Features

- Native OBS filter for media sources
- Configurable cut interval and jump distance
- Optional random variation per jump
- Optional blend filter activation on each cut
- Up to 5 playback regions with region-aware looping
- Background pause when the source is not visible
- Locale files for English and German

## Build

```bash
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## Release Package

The packaged Windows release is available in the sibling folder
`dynamic-video-cutter-1.0.0/` in this workspace.
