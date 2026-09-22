# VitaIPTV

A homebrew IPTV player for the PS Vita (PCH-1000, HENkaku/ensō), written in C
with VitaSDK. It loads M3U / M3U8 playlists (URL or local file) and plays the
channels in them. Only legitimate, publicly available streams are the intended
use.

This is early work in progress. See `docs/NOTES.md` for design decisions,
Vita API findings, decoder limits, and the milestone status.

## Layout

- `core/` - portable C99, no Vita headers. Playlist model + M3U parser (and,
  later, settings, favourites, search). Builds and is unit-tested natively.
- `vita/` - everything SCE-specific (UI, input, networking, decoder, audio).
- `tests/` - native test + benchmark targets (`vitaiptv_test`, `bench_m3u`).
- `tools/` - helper scripts (fetch test playlists, deploy).
- `docs/` - `NOTES.md`.

## Building and testing the core (dev PC, no VitaSDK)

```sh
tools/fetch-playlists.sh          # download real iptv-org test files
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
./build/tests/bench_m3u tests/data/index.m3u   # memory + speed report
```

## Licence

GPL-3.0 (see `LICENSE`). Reusing GPL-3.0 Vita homebrew for the decoder path
keeps the whole project GPL-3.0, which is intended.
