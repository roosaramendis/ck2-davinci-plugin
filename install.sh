#!/usr/bin/env bash
# CorridorKey 2.0 – DaVinci Resolve plugin installer (Linux + macOS).
#
#   bash install.sh                      build, install, connect to the engine
#   bash install.sh --engine DIR         use this CorridorKey2.0 engine folder
#   bash install.sh --prebuilt           Linux: skip compiling, use prebuilt/ zip
#   bash install.sh --connect-only       only (re)write the config (engine moved)
#   bash install.sh --uninstall          remove the plugin from Resolve's OFX folder
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUNDLE="CorridorKey2.ofx.bundle"
SUDO="${SUDO-sudo}"

if [[ "$(uname -s)" == "Darwin" ]]; then
    OS=mac
    OFX_DIR="${OFX_DIR:-/Library/OFX/Plugins}"
    CONF_DIR="${CONF_DIR:-$HOME/Library/Application Support/ck2-davinci-plugin}"
    CACHE_DIR="$HOME/Library/Caches/corridorkey-ofx"
    TEMP_DIR="$HOME/Library/Caches/corridorkey-tmp"
    BACKEND=mps
    export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
else
    OS=linux
    OFX_DIR="${OFX_DIR:-/usr/OFX/Plugins}"
    CONF_DIR="${CONF_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/ck2-davinci-plugin}"
    CACHE_DIR="$HOME/.cache/corridorkey-ofx"   # not /tmp: RAM-backed on CachyOS
    TEMP_DIR="$HOME/.cache/corridorkey-tmp"
    BACKEND=cuda
fi
CONF="$CONF_DIR/ofx.conf"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m ✓\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m ✗ %s\033[0m\n' "$*" >&2; exit 1; }

ENGINE="" MODE=full PREBUILT=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --engine) ENGINE="${2:?--engine needs a folder}"; shift 2 ;;
        --prebuilt) PREBUILT=1; shift ;;
        --connect-only) MODE=connect; shift ;;
        --uninstall) MODE=uninstall; shift ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
done

# ---------------------------------------------------------------- uninstall
if [[ $MODE == uninstall ]]; then
    if [[ -d "$OFX_DIR/$BUNDLE" ]]; then
        say "Removing $OFX_DIR/$BUNDLE (asks for your password)"
        $SUDO rm -rf "$OFX_DIR/$BUNDLE"
        ok "Plugin removed. Cached keys are still in $CACHE_DIR (delete by hand if you want)."
    else
        ok "Plugin isn't installed."
    fi
    exit 0
fi

# ---------------------------------------------------------------- find the engine
valid_engine() { [[ -f "$1/infer.py" && ( -x "$1/venv/bin/python" || -x "$1/.venv/bin/python" ) ]]; }

if [[ -z "$ENGINE" && -f "$CONF" ]]; then          # already connected before
    ENGINE="$(sed -n 's/^engine=//p' "$CONF" | head -1)"
fi
if [[ -z "$ENGINE" || ! -d "$ENGINE" ]]; then       # the ck2-gui app's engine, if it has one
    if [[ $OS == mac ]]; then
        ENGINE="$(defaults read com.ck2-gui.ck2-gui engine_dir_mps 2>/dev/null || true)"
    else
        G="${XDG_CONFIG_HOME:-$HOME/.config}/ck2-gui/ck2-gui.conf"
        if [[ -f "$G" ]]; then
            ENGINE="$(sed -n 's/^engine_dir_cuda=//p; s/^engine_dir=//p' "$G" | head -1)"
        fi
    fi
fi
if [[ -z "$ENGINE" || ! -d "$ENGINE" ]]; then ENGINE="$HOME/apps/CorridorKey2.0"; fi
ENGINE="${ENGINE/#\~/$HOME}"
if ! valid_engine "$ENGINE"; then
    echo "No CorridorKey 2.0 engine found at: $ENGINE"
    read -r -p "Path to your CorridorKey2.0 folder (the one with infer.py and venv/): " ENGINE
    ENGINE="${ENGINE/#\~/$HOME}"
    valid_engine "$ENGINE" || die "$ENGINE has no infer.py + venv/bin/python. Install the engine first."
fi
ENGINE="$(cd "$ENGINE" && pwd)"
PY="$ENGINE/venv/bin/python"; [[ -x "$PY" ]] || PY="$ENGINE/.venv/bin/python"
ok "Engine: $ENGINE"

connect() {
    mkdir -p "$CONF_DIR" "$CACHE_DIR" "$TEMP_DIR"
    local wrapper="" ff
    if [[ $BACKEND == mps ]]; then
        # Keep our own copy so the plugin keeps working if this folder is moved.
        cp "$HERE/support/apple_gpu.py" "$CONF_DIR/apple_gpu.py"
        wrapper="$CONF_DIR/apple_gpu.py"
    fi
    ff="$(command -v ffmpeg || true)"   # the engine needs ffmpeg even for EXR output
    if [[ -z "$ff" ]]; then
        echo "   warning: ffmpeg not found – install it (the engine refuses to start without it)"
    fi
    {
        echo "# Written by ck2-davinci-plugin/install.sh – which engine the Resolve plugin runs."
        echo "engine=$ENGINE"
        echo "python=$PY"
        echo "backend=$BACKEND"
        if [[ -n "$wrapper" ]]; then echo "wrapper=$wrapper"; fi
        if [[ -n "$ff" ]]; then echo "ffmpeg=$ff"; fi
        echo "cache_dir=$CACHE_DIR"
        echo "temp_dir=$TEMP_DIR"
    } > "$CONF"
    ok "Connected: wrote $CONF"
}

if [[ $MODE == connect ]]; then
    connect
    exit 0
fi

# ---------------------------------------------------------------- build
BUILD="$HERE/build"
if [[ $PREBUILT == 1 ]]; then
    [[ $OS == linux ]] || die "--prebuilt is Linux only; on a Mac it has to be compiled."
    say "Unpacking prebuilt plugin"
    rm -rf "$BUILD/$BUNDLE"; mkdir -p "$BUILD"
    (cd "$BUILD" && python3 -m zipfile -e "$HERE/prebuilt/CorridorKey2-ofx-linux.zip" .)
    chmod +x "$BUILD/$BUNDLE/Contents/Linux-x86-64/CorridorKey2.ofx"
else
    missing=()
    command -v cmake >/dev/null || missing+=(cmake)
    command -v git   >/dev/null || missing+=(git)
    command -v c++   >/dev/null || command -v g++ >/dev/null || command -v clang++ >/dev/null \
        || missing+=(compiler)
    if [[ ${#missing[@]} -gt 0 ]]; then
        say "Missing build tools: ${missing[*]}"
        if [[ $OS == mac ]]; then
            [[ " ${missing[*]} " != *" compiler "* ]] || die "Run:  xcode-select --install   (click Install), then run this again."
            command -v brew >/dev/null || die "Homebrew is needed: https://brew.sh"
            brew install cmake git
        elif command -v pacman >/dev/null; then
            $SUDO pacman -S --needed --noconfirm cmake git base-devel
        else
            die "Install cmake, git and a C++ compiler with your package manager, or use --prebuilt."
        fi
    fi
    say "Building (the first build downloads the OpenFX SDK, ~1 min)"
    gen=(); command -v ninja >/dev/null && gen=(-G Ninja)
    cmake -S "$HERE" -B "$BUILD" "${gen[@]}" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD" --config Release --parallel
fi
[[ -d "$BUILD/$BUNDLE/Contents" ]] || die "Build failed – paste the output above into the chat."
ok "Built $BUILD/$BUNDLE"

# ---------------------------------------------------------------- install
say "Copying into $OFX_DIR (asks for your password)"
$SUDO mkdir -p "$OFX_DIR/$BUNDLE"
$SUDO cp -R "$BUILD/$BUNDLE/." "$OFX_DIR/$BUNDLE/"
ok "Installed $OFX_DIR/$BUNDLE"

connect

cat <<EOF

Done. Next:
  1. Restart DaVinci Resolve.
  2. Effects → OpenFX → Keying → CorridorKey 2.0 — drag it onto a clip.
  3. In the Inspector press "Key clip" and wait for the progress bar.
EOF
