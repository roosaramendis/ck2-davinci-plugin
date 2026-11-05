#!/usr/bin/env bash
# CorridorKey 2.0 – DaVinci Resolve plugin installer (Linux, any distro + macOS).
# Windows: install.ps1. Usually run for you by the ck2-gui app (Setup → DCC plugins).
#
#   bash install.sh                      install (ready-made build if present, else compile)
#   bash install.sh --engine DIR         use this CorridorKey2.0 engine folder
#   bash install.sh --build              compile even if a ready-made build is present
#   bash install.sh --ffmpeg PATH        ffmpeg for the engine (default: the one on PATH)
#   bash install.sh --gui                ask for the password with a dialog (no terminal)
#   bash install.sh --connect-only       only (re)write the config (engine moved)
#   bash install.sh --no-sam             skip downloading the SAM 2 model (Roto node)
#   bash install.sh --uninstall          remove the plugin from Resolve's OFX folder
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUNDLE="CorridorKey2.ofx.bundle"
SUDO="${SUDO-sudo}"   # SUDO= (empty) runs admin steps directly (tests, root)

if [[ "$(uname -s)" == "Darwin" ]]; then
    OS=mac
    PREBUILT_ZIP="$HERE/prebuilt/CorridorKey2-ofx-macos.zip"
    OFX_DIR="${OFX_DIR:-/Library/OFX/Plugins}"
    CONF_DIR="${CONF_DIR:-$HOME/Library/Application Support/ck2-davinci-plugin}"
    CACHE_DIR="$HOME/Library/Caches/corridorkey-ofx"
    TEMP_DIR="$HOME/Library/Caches/corridorkey-tmp"
    BACKEND=mps
    SAM_MODEL=facebook/sam2.1-hiera-base-plus
    MACRO_DIR="$HOME/Library/Application Support/Blackmagic Design/DaVinci Resolve/Fusion/Macros"
    export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
else
    OS=linux
    PREBUILT_ZIP="$HERE/prebuilt/CorridorKey2-ofx-linux.zip"
    OFX_DIR="${OFX_DIR:-/usr/OFX/Plugins}"
    CONF_DIR="${CONF_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/ck2-davinci-plugin}"
    CACHE_DIR="$HOME/.cache/corridorkey-ofx"   # not /tmp: RAM-backed on CachyOS
    TEMP_DIR="$HOME/.cache/corridorkey-tmp"
    BACKEND=cuda
    SAM_MODEL=facebook/sam2.1-hiera-large
    MACRO_DIR="$HOME/.local/share/DaVinciResolve/Fusion/Macros"
fi
CONF="$CONF_DIR/ofx.conf"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m ✓\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m ✗ %s\033[0m\n' "$*" >&2; exit 1; }

# Run one shell command line with admin rights, asking once.
as_admin() {
    local cmd="$1"
    if [[ -z "$SUDO" || $EUID -eq 0 ]]; then
        sh -c "$cmd"
    elif [[ $GUI == 1 && $OS == mac ]]; then
        local esc=${cmd//\\/\\\\}; esc=${esc//\"/\\\"}
        osascript -e "do shell script \"$esc\" with administrator privileges"
    elif [[ $GUI == 1 ]]; then
        command -v pkexec >/dev/null || die "No password dialog (pkexec) on this system. In a terminal run: sudo sh -c '$cmd'"
        pkexec sh -c "$cmd"
    else
        $SUDO sh -c "$cmd"
    fi
}
q() { printf '%q' "$1"; }   # quote a path for as_admin

ENGINE="" MODE=full BUILD_FROM_SOURCE=0 SAM=1 GUI=0 FFMPEG=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --engine) ENGINE="${2:-}"; shift 2 ;;
        --ffmpeg) FFMPEG="${2:-}"; shift 2 ;;
        --build) BUILD_FROM_SOURCE=1; shift ;;
        --prebuilt) BUILD_FROM_SOURCE=0; shift ;;   # (default now; kept for old instructions)
        --gui) GUI=1; shift ;;
        --no-sam) SAM=0; shift ;;
        --connect-only) MODE=connect; shift ;;
        --uninstall) MODE=uninstall; shift ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
done

# ---------------------------------------------------------------- uninstall
if [[ $MODE == uninstall ]]; then
    if [[ -d "$OFX_DIR/$BUNDLE" ]]; then
        say "Removing $OFX_DIR/$BUNDLE (asks for your password)"
        as_admin "rm -rf $(q "$OFX_DIR/$BUNDLE")"
        rm -f "$MACRO_DIR/CorridorKey Outputs.setting"
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
    [[ $GUI == 0 ]] || die "No CorridorKey 2.0 engine at $ENGINE (infer.py + venv/). Install the engine first."
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
    cp "$HERE/support/roto_daemon.py" "$CONF_DIR/roto_daemon.py"   # SAM 2 worker for the Roto node
    cp "$HERE/support/roto_picker.py" "$CONF_DIR/roto_picker.py"   # the Roto node's picker window
    ff="$FFMPEG"                         # the engine needs ffmpeg even for EXR output
    [[ -n "$ff" && -x "$ff" ]] || ff="$(command -v ffmpeg || true)"
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
        echo "roto_daemon=$CONF_DIR/roto_daemon.py"
        echo "roto_picker=$CONF_DIR/roto_picker.py"
        echo "sam_model=$SAM_MODEL"
    } > "$CONF"
    ok "Connected: wrote $CONF"
}

if [[ $MODE == connect ]]; then
    connect
    exit 0
fi

# ---------------------------------------------------------------- build
BUILD="$HERE/build"
if [[ $BUILD_FROM_SOURCE == 0 && -f "$PREBUILT_ZIP" ]]; then
    say "Using the ready-made plugin ($(basename "$PREBUILT_ZIP"))"
    rm -rf "$BUILD/$BUNDLE"; mkdir -p "$BUILD"
    if command -v unzip >/dev/null; then
        unzip -q -o "$PREBUILT_ZIP" -d "$BUILD"
    else
        (cd "$BUILD" && "$PY" -m zipfile -e "$PREBUILT_ZIP" .)
    fi
    chmod +x "$BUILD/$BUNDLE"/Contents/*/CorridorKey2.ofx 2>/dev/null || true
else
    missing=()
    command -v cmake >/dev/null || missing+=(cmake)
    command -v git   >/dev/null || missing+=(git)
    command -v c++   >/dev/null || command -v g++ >/dev/null || command -v clang++ >/dev/null \
        || missing+=(compiler)
    if [[ ${#missing[@]} -gt 0 ]]; then
        if [[ $OS == mac ]]; then
            [[ " ${missing[*]} " != *" compiler "* ]] || die "Run:  xcode-select --install   (click Install), then try again."
            die "Install the build tools first:  brew install cmake git"
        fi
        hint="your package manager"
        if command -v apt-get >/dev/null; then hint="sudo apt install cmake git build-essential"
        elif command -v dnf >/dev/null; then hint="sudo dnf install cmake git gcc-c++ make"
        elif command -v pacman >/dev/null; then hint="sudo pacman -S --needed cmake git base-devel"
        elif command -v zypper >/dev/null; then hint="sudo zypper install cmake git gcc-c++ make"
        elif command -v emerge >/dev/null; then hint="sudo emerge --ask dev-build/cmake dev-vcs/git"
        fi
        die "No ready-made plugin for this system and missing build tools (${missing[*]}). Install them:  $hint"
    fi
    say "Building (the first build downloads the OpenFX SDK, ~1 min)"
    gen=(); command -v ninja >/dev/null && gen=(-G Ninja)
    cmake -S "$HERE" -B "$BUILD" "${gen[@]}" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD" --config Release --parallel
fi
[[ -d "$BUILD/$BUNDLE/Contents" ]] || die "Build failed – use 'Report a bug' in ck2-gui (or paste the output above)."
ok "Plugin ready: $BUILD/$BUNDLE"

# ---------------------------------------------------------------- install
say "Copying into $OFX_DIR (asks for your password)"
as_admin "mkdir -p $(q "$OFX_DIR/$BUNDLE") && cp -R $(q "$BUILD/$BUNDLE")/. $(q "$OFX_DIR/$BUNDLE")/"
[[ -d "$OFX_DIR/$BUNDLE/Contents" ]] || die "Copying into $OFX_DIR didn't work (password cancelled?)."
if [[ $OS == mac ]]; then xattr -dr com.apple.quarantine "$OFX_DIR/$BUNDLE" 2>/dev/null || true; fi
ok "Installed $OFX_DIR/$BUNDLE"

connect

# ---------------------------------------------------------------- Fusion macro (multi-output)
mkdir -p "$MACRO_DIR"
cp "$HERE/fusion/CorridorKey Outputs.setting" "$MACRO_DIR/"
ok "Fusion macro: $MACRO_DIR/CorridorKey Outputs.setting"

# ---------------------------------------------------------------- picker window needs PySide6
if ! "$PY" -c "import PySide6" 2>/dev/null; then
    say "Adding PySide6 to the engine's Python (for the Roto picker window)"
    if command -v uv >/dev/null; then
        uv pip install --python "$PY" "PySide6>=6.7" || echo "   warning: PySide6 install failed – the Roto picker won't open"
    else
        "$PY" -m pip install "PySide6>=6.7" || echo "   warning: PySide6 install failed – the Roto picker won't open"
    fi
fi

# ---------------------------------------------------------------- SAM 2 model for the Roto node
if [[ $SAM == 1 ]]; then
    if "$PY" -c "import transformers; transformers.Sam2VideoModel" 2>/dev/null; then
        say "Downloading $SAM_MODEL for the Roto node (once, ~0.9 GB; skip with --no-sam)"
        if "$PY" -c "from huggingface_hub import snapshot_download as d; d('$SAM_MODEL')" 2>"$CONF_DIR/sam_download.log"; then
            ok "SAM 2 model ready"
        else
            echo "   warning: couldn't download $SAM_MODEL now; the Roto node will try on first use (details: $CONF_DIR/sam_download.log)"
        fi
    else
        echo "   warning: this engine's transformers has no SAM 2 – the Roto node needs:"
        echo "            $PY -m pip install -U 'transformers>=4.56'"
    fi
fi

cat <<EOF

Done. Next:
  1. Restart DaVinci Resolve.
  2. Effects → OpenFX → Keying: CorridorKey 2.0, CorridorKey Pass, CorridorKey Roto.
     Fusion page: Add Tool → Macros → CorridorKey Outputs (Cutout/Alpha/FG/Checker outputs).
  3. Press "Key clip" in the Inspector, then play the clip once.
EOF
