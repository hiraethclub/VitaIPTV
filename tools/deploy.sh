#!/usr/bin/env bash
#
# Deploy VitaIPTV to a Vita running the vitacompanion plugin (FTP on 1337,
# command server on 1338). Runs on the machine that can reach the Vita over
# the LAN (Linux, macOS, WSL, or Git Bash on Windows).
#
# The Vita's IP is NEVER committed. Provide it via the VITA_IP environment
# variable or a gitignored tools/deploy.conf containing:  VITA_IP=192.168.1.23
#
# Commands (vitacompanion protocol, verified against its README):
#   FTP:      curl -T <file> ftp://IP:1337/ux0:/...
#   control:  text commands over TCP 1338, e.g. "launch VIPTV0001"
#
# Usage:
#   tools/deploy.sh eboot     # fast loop: quit, upload eboot.bin, relaunch
#                             # (app must already be installed once from the VPK)
#   tools/deploy.sh vpk       # upload the .vpk to ux0:/ for manual install in VitaShell
#   tools/deploy.sh launch    # just (re)launch the app
#   tools/deploy.sh quit      # close the app
#   tools/deploy.sh log [port]# listen for UDP network logs (needs a debugnet build)
#
# Prerequisite for the fast loop: install the VPK once via VitaShell so the
# app directory ux0:/app/VIPTV0001 exists. Until vitacompanion is installed at
# all, the fallback is fully manual: build, copy the .vpk to the Vita, install.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TITLEID="VIPTV0001"
FTP_PORT=1337
CMD_PORT=1338
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-vita}"
VPK="$BUILD_DIR/vita/vitaiptv.vpk"
EBOOT="$BUILD_DIR/vita/eboot.bin"
LOG_PORT_DEFAULT=18194   # libdebugnet default; adjust to your build

# --- load VITA_IP -----------------------------------------------------------
if [[ -z "${VITA_IP:-}" && -f "$REPO_ROOT/tools/deploy.conf" ]]; then
    # shellcheck disable=SC1091
    source "$REPO_ROOT/tools/deploy.conf"
fi
if [[ -z "${VITA_IP:-}" ]]; then
    echo "error: VITA_IP is not set. Export it or add it to tools/deploy.conf" >&2
    exit 1
fi

# Send a command to the vitacompanion control port (TCP 1338) via bash /dev/tcp.
send_cmd() {
    local cmd="$1"
    echo "-> $cmd"
    exec 3<>"/dev/tcp/$VITA_IP/$CMD_PORT" || {
        echo "error: cannot reach $VITA_IP:$CMD_PORT (is vitacompanion running?)" >&2
        return 1
    }
    printf '%s\n' "$cmd" >&3
    # give the plugin a moment, then close
    sleep 0.3
    exec 3>&- || true
}

ftp_put() {
    local src="$1" dest="$2"
    echo "ftp: $src -> ux0:/$dest"
    curl -q --silent --show-error -T "$src" \
        "ftp://$VITA_IP:$FTP_PORT/ux0:/$dest"
}

cmd="${1:-eboot}"
case "$cmd" in
    eboot)
        [[ -f "$EBOOT" ]] || { echo "missing $EBOOT (build first)" >&2; exit 1; }
        send_cmd "quit $TITLEID" || true
        ftp_put "$EBOOT" "app/$TITLEID/eboot.bin"
        send_cmd "launch $TITLEID"
        ;;
    vpk)
        [[ -f "$VPK" ]] || { echo "missing $VPK (build first)" >&2; exit 1; }
        ftp_put "$VPK" "vitaiptv.vpk"
        echo "Uploaded to ux0:/vitaiptv.vpk. Install it from VitaShell."
        ;;
    launch) send_cmd "launch $TITLEID" ;;
    quit)   send_cmd "quit $TITLEID" ;;
    log)
        port="${2:-$LOG_PORT_DEFAULT}"
        echo "listening for UDP logs on 0.0.0.0:$port (Ctrl-C to stop)"
        echo "note: requires the app to be built with debugnet pointed at this PC"
        if command -v nc >/dev/null 2>&1; then
            nc -u -l -k -p "$port" 2>/dev/null || nc -u -l "$port"
        else
            echo "error: nc (netcat) not found; install it for log listening" >&2
            exit 1
        fi
        ;;
    *)
        echo "usage: tools/deploy.sh [eboot|vpk|launch|quit|log [port]]" >&2
        exit 1
        ;;
esac
