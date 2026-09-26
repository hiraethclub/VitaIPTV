#!/usr/bin/env bash
#
# Fetch the iptv-org test playlists used by the core unit tests and benchmark
# into tests/data/. The large files (uk/news/index) are gitignored; the tiny
# gb-wls.m3u is committed as a frozen fixture and is refreshed here too.
#
# These files are live and change over time. The tests derive expected channel
# counts from each file's own contents, so a refresh will not break them.
#
# Usage: tools/fetch-playlists.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_ROOT/tests/data"
BASE="https://iptv-org.github.io/iptv"

mkdir -p "$DEST"

fetch() {
    local name="$1" url="$2"
    echo "fetching $name"
    curl -fsSL -o "$DEST/$name" "$url"
}

fetch "gb-wls.m3u" "$BASE/subdivisions/gb-wls.m3u"
fetch "uk.m3u"     "$BASE/countries/uk.m3u"
fetch "news.m3u"   "$BASE/categories/news.m3u"
fetch "index.m3u"  "$BASE/index.m3u"

# A real MPEG-TS segment for the demuxer test (best-effort; skipped on failure).
# Resolves master -> 360p media playlist -> first segment. URLs rotate, so this
# is fetched fresh and gitignored.
FA="https://failarmy-international-gb.samsung.wurl.tv/playlist.m3u8"
mkdir -p "$DEST/hls"
if curl -fsSL -o "$DEST/hls/m.m3u8" "$FA" 2>/dev/null; then
    var=$(grep -vE '^#' "$DEST/hls/m.m3u8" | head -1)
    case "$var" in http*) vurl="$var";; *) vurl="${FA%/*}/$var";; esac
    if curl -fsSL -o "$DEST/hls/media.m3u8" "$vurl" 2>/dev/null; then
        seg=$(grep -vE '^#' "$DEST/hls/media.m3u8" | head -1)
        echo "fetching sample_segment.ts"
        curl -fsSL -o "$DEST/hls/sample_segment.ts" "$seg" 2>/dev/null \
            || echo "  (segment fetch failed; TS real-data test will skip)"
    fi
fi

echo "done. sizes:"
ls -la "$DEST"
