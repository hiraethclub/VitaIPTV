# VitaIPTV - development notes

Running log of findings about the build environment, the Vita APIs, decoder
limits, and things tried. Kept updated per the project rules. Anything that
must be verified on real hardware is called out explicitly.

---

## 1. Build environment (as discovered)

This project is being developed in an **ephemeral Linux cloud container**, not
on the local Windows/WSL machine described in the original brief. Consequences:

- There is **no route to the Vita** from here, so deploy and any on-device
  testing must happen on the owner's own machine. The deploy script is written
  but cannot be exercised here.
- There was no pre-existing VitaSDR project to match against in this container.

Host toolchain present: gcc 13.3, clang 18, cmake 3.28, ninja, make, curl,
python3. 4 cores, ~15 GiB RAM, ~30 GiB free disk.

### VitaSDK

Installed here via vdpm into `/usr/local/vitasdk`:

- `arm-vita-eabi-gcc` 15.2.0, SCE stubs, and `share/vita.toolchain.cmake` are
  present.
- Third-party libs installed: vita2d, freetype, harfbuzz, libpng, zlib, libzip,
  libexif, libjpeg-turbo, libftpvita, libdebugnet, taihen, kubridge, libk,
  libmad, libogg, libvorbis, flac, **openssl (libssl.a + libcrypto.a)**.

**Gotcha for anyone reinstalling:** the vdpm scripts look for the pacman
package client at `$VITASDK/bin/pacman`, but the current core package ships it
at `$VITASDK/libexec/vdpm/pacman`. Set `VDPM_PACMAN=$VITASDK/libexec/vdpm/pacman`
before running `./vdpm install`, and pipe `yes |` (or pass `--noconfirm`) since
it prompts interactively. Do **not** pre-create `$VITASDK` before
`bootstrap-vitasdk.sh`; it refuses to run if the directory already exists.

---

## 2. M3U format - findings from the real iptv-org files

Measured against downloaded copies of the files named in the brief (UK, Wales,
News, and the full `index.m3u`). Fetch them with `tools/fetch-playlists.sh`.

- `index.m3u` is **2.37 MiB, 10,966 channels**, UTF-8 with **CRLF** line
  endings. `gb-wls.m3u` is tiny (2 channels) and kept as a committed fixture.
- Only three directive types occur across the whole index: `#EXTM3U` (1),
  `#EXTINF` (one per channel), `#EXTVLCOPT` (776).
- Every channel has `tvg-id`, `tvg-logo`, and `group-title`. **`tvg-name` never
  appears** in these files, though the brief mentioned it - parsed anyway if
  present.
- `#EXTVLCOPT` only ever carries `http-user-agent` (559) or `http-referrer`
  (219), as **unquoted** values (everything after the `=`).
- `http-user-agent` / `http-referrer` also appear as **EXTINF attributes** on
  some lines, not just as EXTVLCOPT. The parser captures them from both places.
- One malformed stray attribute (`centeralt="`) exists in the index; unknown
  attributes are simply ignored.
- Number of `#EXTINF` lines equals the number of URL lines in every file, i.e.
  no dangling entries in practice.

### Parser design (core/)

- Incremental **push parser** (`vi_m3u_feed` accepts arbitrary byte chunks and
  buffers only the current line). This is what the future network fetch layer
  will drive; convenience wrappers parse a whole file or buffer for tests.
- Compact storage: one growing **string arena** plus a channel array whose
  fields are **32-bit offsets** into the arena (not pointers), so the arena can
  realloc freely. Offset 0 is the reserved empty string. Channel struct = 36
  bytes.
- Tolerates BOM, CRLF/LF, blank lines, unknown `#` directives, quoted and
  unquoted attribute values, and commas inside quoted values (the name split
  respects quotes).

### Memory + speed report (index.m3u, on the dev PC)

| metric | value |
|---|---|
| channels | 10,966 |
| parse time | ~12 ms (~194 MiB/s) |
| string arena | 1.93 MiB used |
| channel array | 576 KiB (36 B/channel) |
| **parser heap total** | **~2.56 MiB** (~245 B/channel) |

**Caveat:** these are x86 dev-PC numbers. The Vita's CPU is far slower, so
parse time on device will be a large multiple of 12 ms - to be measured on
hardware. The ~2.56 MiB footprint is the meaningful figure for Vita memory and
is comfortable. Possible future win: `group-title` and `http-user-agent` values
are massively duplicated (a handful of distinct values across 11k channels);
interning/deduping them would cut arena memory notably. Left simple for now.

---

## 3. M2 playback research

All API facts below are from the **installed VitaSDK headers** unless stated
otherwise.

### Hardware H.264 decoder - `psp2/videodec.h`

- The only decoder type is `SCE_VIDEODEC_TYPE_HW_AVCDEC` (0x1001): **hardware
  H.264/AVC only.** No HEVC, MPEG-2, VP9 - consistent with the brief.
- Flow: `sceVideodecInitLibrary` -> `sceAvcdecQueryDecoderMemSize` ->
  `sceAvcdecCreateDecoder` (caller provides the frame buffer) ->
  `sceAvcdecDecode` (feed one access unit `SceAvcdecAu` = Annex-B ES bytes +
  timestamps, get `SceAvcdecArrayPicture` out) -> delete/term.
- `SceVideodecQueryInitInfoHwAvcdec` requires **max `horizontal`, `vertical`,
  `numOfRefFrames`** and `numOfStreams = 1` up front - you declare the ceiling
  resolution at init.
- Output pixel formats include **`RGBA8888`** and YUV420 raster. RGBA8888 out is
  convenient: it can be uploaded straight into a vita2d texture.
- **There is no profile/level argument.** The API will not politely pre-reject a
  too-high stream; init fails or `sceAvcdecDecode` errors
  (`SCE_AVCDEC_ERROR_UNSUPPORT_IMAGE_SIZE` etc.). **Therefore the brief's
  "detect codec/profile/level before starting" must be done by us** - parse the
  H.264 SPS (profile_idc / level_idc / dimensions) or read the demuxer's stream
  info, and compare against a configured ceiling.

### High-level player - `psp2/avplayer.h` (SceAvPlayer)

- One-call-ish path: `sceAvPlayerInit` -> `sceAvPlayerAddSource(handle, url)` ->
  `sceAvPlayerStart` -> poll `sceAvPlayerGetVideoData` / `GetAudioData` for
  decoded frames (pData + width/height/aspectRatio; audio as PCM). It does
  demux + decode + A/V sync internally.
- File I/O can be replaced (`open/close/readOffset/size`), but `readOffset`
  is **position + length based (random access)** and the control surface is
  built for **seekable VOD**: `duration`, `sceAvPlayerJumpToTime`, trick
  speeds. That is a poor fit for live, non-seekable HLS/TS.
- Stream info exposes width/height/aspectRatio but **not** codec profile/level,
  so our own detection is still needed.
- Developers report instability (noted in the brief); treated here as a
  fallback/comparison, not the primary path.

### Networking + TLS - `psp2/net/http.h`, `psp2/libssl.h`, openssl

- **Sony `SceHttp`/`SceHttps` supports HTTPS with certificate verification**:
  `SCE_HTTPS_FLAG_SERVER_VERIFY`, `_CN_CHECK`, `_KNOWN_CA_CHECK`, and validity
  window checks `_NOT_AFTER_CHECK` / `_NOT_BEFORE_CHECK`. Those validity checks
  depend on a **correct system clock** - exactly the risk the brief flagged. It
  uses the system CA store.
- **OpenSSL is also available** as static libs (`libssl.a`, `libcrypto.a`), so a
  second TLS path (our own CA bundle, our own clock handling) is possible if
  SceHttp proves limiting.
- This answers the brief's TLS question: two working backends exist. Leaning
  toward SceHttp for simplicity, with OpenSSL as the escape hatch. **Clock
  correctness on the device must be verified** (cert "not yet valid" failures
  are the classic symptom of a wrong RTC).

### Audio + power

- `psp2/audioout.h` (SceAudioOut) for PCM output; `psp2/power.h`
  (`scePowerSetIdleTimerCount` / control APIs) to inhibit sleep and screen dim
  during playback (M5, but noted).

### FFmpeg availability

- **FFmpeg is not a stock VitaSDK package** (no `avformat.h`/`avcodec.h` after a
  full vdpm install; `vdpm search ffmpeg` finds nothing). Projects that use it
  **vendor a pinned build**. So using FFmpeg means building/vendoring it
  ourselves - a real cost to weigh against writing a small MPEG-TS demuxer.

### Reference projects (studied, licences checked)

| project | what it gives us | live input? | licence |
|---|---|---|---|
| **vita-hw-decoder** | FFmpeg demux + SceVideodec (`h264_vita`) + vita2d YUV render | **No** - README: "requires seekable containers... not streaming protocols" | **GPL-3.0-only** - decode/render parts reusable |
| **VitaMediaDeck** | HW-first H.264, NV12/P2 direct render, good buffering ideas (8 MiB sliding read-ahead ring; bounded PCM queue decoupling AAC decode from the AudioOut feeder) | **No** - "HLS, live TV... are not exposed yet"; uses HTTP Range | **GPL-3.0-only** |
| **NetStream** (GrapheneCt) | proof that HTTP + streaming works on Vita | Yes (HTTP server / livestreams) | **No licence file or statement found** -> all rights reserved by default; **study for approach only, do not copy** |

**Key takeaway:** every FFmpeg-based reference assumes a **seekable** container
(MOV/MP4/Matroska over HTTP Range). Live IPTV is the opposite: **raw MPEG-TS
over HTTP, or HLS (a rolling `.m3u8` of `.ts`/`.fmp4` segments), neither
seekable.** So from these projects we can reuse the **decode + render** stage
(SceVideodec -> vita2d) but **not** their demux/input stage. The live input and
demux is ours to build.

---

## 4. Proposed playback pipeline (M2) - awaiting approval

Two candidate approaches. Recommending **A**, with **B** kept as a quick early
sanity check.

**A. Custom pipeline (recommended for the real target: live HLS/TS)**

```
fetch (SceHttp, worker thread, HTTPS)
  -> HLS handler: parse master playlist, pick best H.264 variant <= ceiling,
     poll media playlist, download segments in order
  -> demux MPEG-TS / fMP4: split into H.264 access units + AAC frames
     (small hand-written TS demuxer, or a vendored minimal FFmpeg avformat)
  -> H.264 SPS check: profile/level/resolution vs configured ceiling;
     fail clearly if unsupported
  -> video: sceAvcdecDecode -> RGBA8888 -> vita2d texture -> scale to 960x544
  -> audio: AAC decode -> PCM -> SceAudioOut, on its own thread
  -> A/V sync off the stream PTS; bounded ring buffers between stages
```

All network and decode work on **worker threads**; the UI thread never blocks.
This matches the brief and the buffering patterns seen in VitaMediaDeck.

**B. SceAvPlayer smoke test (fast, throwaway)**

Hand a known-good HLS URL straight to `sceAvPlayerAddSource` and see what
happens on device. Cheap way to learn whether Sony's player copes with live HLS
at all, and a baseline for A/V behaviour. Not the foundation to build on given
the reported instability and VOD-oriented API.

**Open decision for you:** the demux stage in A - vendor a minimal FFmpeg
(more formats, build cost, GPL) vs. write a small MPEG-TS + basic HLS demuxer
in C (less code we don't control, but more to write and test). I lean toward a
hand-written TS/HLS demuxer first, since iptv-org streams are overwhelmingly
plain HLS/TS, and add FFmpeg only if real streams demand it.

---

## 5. Must-be-tested-on-hardware (cannot verify from here)

1. **Decoder ceiling.** Sources disagree (720p/L3.1 vs 1080p/L4.0). Must be
   probed on the actual PCH-1000 by trying known 720p and 1080p H.264 streams
   and reading the `sceAvcdec*` error codes.
2. **System clock / TLS.** Confirm HTTPS cert validation succeeds (correct RTC);
   watch for `SCE_HTTPS_ERROR_SSL_NOT_BEFORE/AFTER`.
3. **Live HLS via SceAvPlayer** (approach B) - does it play or fall over?
4. **Parse time for index.m3u on device** - expected to be much slower than the
   ~12 ms measured on PC.
