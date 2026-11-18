#!/usr/bin/env python3
"""Installs the optional CorridorKey Roto models into <engine>/roto-env.

The keying engine pins transformers < 5, but SAM 3 needs transformers 5, so the
Roto node / tab get their own Python environment. PyTorch is the same version as
the engine's (uv shares its cache, so it is not downloaded twice).

    python setup_roto_env.py --engine DIR [--uv UV] [--matanyone --accept-matanyone-license]
                             [--remove-matanyone] [--uninstall]

Same file in ck2-gui (ck2/setup_roto_env.py) and ck2-davinci-plugin (support/).
Prints one progress line per step; exit code 0 = ready.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

MATANYONE_COMMIT = "0079197acd6d16a741f71558809c06c586c579e0"
MATANYONE_ZIP = f"https://github.com/pq-yang/MatAnyone2/archive/{MATANYONE_COMMIT}.zip"
MATANYONE_WEIGHTS = "https://github.com/pq-yang/MatAnyone2/releases/download/v1.0.0/matanyone2.pth"
LICENSE_NOTE = (
    "MatAnyone 2 (c) S-Lab, NTU, NTU S-Lab License 1.0: free for NON-COMMERCIAL use only.\n"
    "Commercial use (paid or client work) needs permission from its authors\n"
    "(peiqingyang99@outlook.com). It is not part of CorridorKey.\n")
# What the Roto helper needs on top of torch (MatAnyone 2 itself is installed without its
# own dependency list: it pins an old huggingface_hub that would break transformers 5).
BASE = ["transformers>=5.0,<6", "huggingface_hub>=1.0", "safetensors", "accelerate",
        "opencv-python>=4.8,<5", "pillow", "numpy", "tqdm"]
MATANYONE_DEPS = ["hydra-core>=1.3.2", "omegaconf", "imageio", "einops"]


def bin_python(env: Path) -> Path:
    return env / ("Scripts/python.exe" if os.name == "nt" else "bin/python")


def run(cmd, **kw):
    print("$ " + " ".join(str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode != 0:
        sys.exit(f"[roto-env] failed ({r.returncode}): {cmd[0]} {cmd[1] if len(cmd) > 1 else ''}")


def torch_spec(engine_py: Path):
    """['torch==2.9.1', 'torchvision==0.24.1'], index URL or None, like the engine's."""
    out = subprocess.run([str(engine_py), "-c",
                          "import json, torch\n"
                          "try:\n import torchvision; tv = torchvision.__version__\n"
                          "except Exception:\n tv = None\n"
                          "print(json.dumps([torch.__version__, tv]))"],
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit("[roto-env] the engine's Python has no PyTorch: install the engine first")
    tv, tvv = json.loads(out.stdout.strip().splitlines()[-1])
    pkgs, index = [], None
    for name, ver in (("torch", tv), ("torchvision", tvv)):
        if ver:
            base, _, local = ver.partition("+")
            pkgs.append(f"{name}=={base}")
            if re.fullmatch(r"cu\d+|rocm[\d.]+|cpu", local or ""):
                index = f"https://download.pytorch.org/whl/{local}"
    if not any(p.startswith("torchvision") for p in pkgs):
        pkgs.append("torchvision")
    return pkgs, index


def pip_install(env: Path, uv: str | None, pkgs, index=None, no_deps=False):
    py = bin_python(env)
    if uv:
        cmd = [uv, "pip", "install", "--python", py]
    else:
        cmd = [py, "-m", "pip", "install", "--disable-pip-version-check"]
    if index:
        cmd += ["--index-url", index, "--extra-index-url", "https://pypi.org/simple"]
        if uv:
            cmd += ["--index-strategy", "unsafe-best-match"]
    if no_deps:
        cmd.append("--no-deps")
    run(cmd + list(pkgs))


def download(url: str, dest: Path):
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    print(f"[roto-env] downloading {url}", flush=True)
    ctx = None
    try:
        import ssl
        cafile = os.environ.get("SSL_CERT_FILE")
        if not cafile:
            try:
                import certifi
                cafile = certifi.where()
            except ImportError:
                pass
        ctx = ssl.create_default_context(cafile=cafile) if cafile else None
    except Exception:
        pass
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "ck2-roto-setup"}),
                                context=ctx, timeout=60) as r, open(tmp, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        got = 0
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)
            got += len(b)
            if total:
                print(f"\r[roto-env] {got >> 20}/{total >> 20} MB", end="", flush=True)
    print(flush=True)
    os.replace(tmp, dest)


def install_matanyone_code(env: Path, src: str):
    """Copies the matanyone2 package into the environment. (Its own packaging doesn't build
    with current hatchling, and its dependency pins would break transformers 5.)"""
    import tempfile
    import zipfile
    site = subprocess.run([str(bin_python(env)), "-c", "import sysconfig; print(sysconfig.get_paths()['purelib'])"],
                          capture_output=True, text=True).stdout.strip()
    dest = Path(site) / "matanyone2"
    shutil.rmtree(dest, ignore_errors=True)
    if os.path.isdir(src):
        shutil.copytree(Path(src) / "matanyone2", dest)
    else:
        with tempfile.TemporaryDirectory() as td:
            z = Path(td) / "matanyone2.zip"
            download(src, z)
            with zipfile.ZipFile(z) as zf:
                for n in zf.namelist():
                    parts = n.split("/", 1)
                    if len(parts) == 2 and parts[1].startswith("matanyone2/") and not n.endswith("/"):
                        out = Path(site) / parts[1]
                        out.parent.mkdir(parents=True, exist_ok=True)
                        out.write_bytes(zf.read(n))
    if not (dest / "__init__.py").exists():
        sys.exit("[roto-env] MatAnyone 2 code missing after download")
    print(f"[roto-env] MatAnyone 2 code in {dest}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", required=True, type=Path)
    ap.add_argument("--uv", default=None)
    ap.add_argument("--matanyone", action="store_true")
    ap.add_argument("--accept-matanyone-license", action="store_true")
    ap.add_argument("--remove-matanyone", action="store_true")
    ap.add_argument("--uninstall", action="store_true")
    ap.add_argument("--matanyone-src", default=os.environ.get("CK2_MATANYONE_SRC", MATANYONE_ZIP),
                    help=argparse.SUPPRESS)  # testing: a local checkout
    a = ap.parse_args()

    engine = a.engine.expanduser().resolve()
    env = engine / "roto-env"
    if a.uninstall:
        shutil.rmtree(env, ignore_errors=True)
        print("[roto-env] removed", flush=True)
        return
    if a.remove_matanyone:
        shutil.rmtree(env / "models", ignore_errors=True)
        if bin_python(env).exists():
            site = subprocess.run([str(bin_python(env)), "-c", "import sysconfig; print(sysconfig.get_paths()['purelib'])"],
                                  capture_output=True, text=True).stdout.strip()
            shutil.rmtree(Path(site) / "matanyone2", ignore_errors=True)
        print("[roto-env] MatAnyone 2 removed", flush=True)
        return
    if a.matanyone and not a.accept_matanyone_license:
        print(LICENSE_NOTE, flush=True)
        sys.exit("[roto-env] MatAnyone 2 is only installed with --accept-matanyone-license")

    engine_py = bin_python(engine / "venv")
    if not engine_py.exists():
        sys.exit(f"[roto-env] no engine venv at {engine / 'venv'}: install the engine first")
    uv = a.uv or shutil.which("uv")

    print("[roto-env] step 1/4: environment", flush=True)
    if not bin_python(env).exists():
        if uv:
            run([uv, "venv", "--allow-existing", "--python", engine_py, env])
        else:
            run([engine_py, "-m", "venv", env])
    if not uv:
        run([bin_python(env), "-m", "pip", "install", "-q", "--upgrade", "pip"])

    print("[roto-env] step 2/4: PyTorch (same as the engine)", flush=True)
    pkgs, index = torch_spec(engine_py)
    if os.environ.get("CK2_TORCH_INDEX") is not None:  # testing / mirrors ("" = PyPI only)
        index = os.environ["CK2_TORCH_INDEX"] or None
    pip_install(env, uv, pkgs, index)

    print("[roto-env] step 3/4: transformers 5 (SAM 2.1 + SAM 3)", flush=True)
    pip_install(env, uv, BASE)

    if a.matanyone:
        print("[roto-env] step 4/4: MatAnyone 2 (hair and soft edges)", flush=True)
        print(LICENSE_NOTE, flush=True)
        pip_install(env, uv, MATANYONE_DEPS)
        install_matanyone_code(env, a.matanyone_src)
        w = env / "models" / "matanyone2.pth"
        if not w.exists() or w.stat().st_size < 100 << 20:
            download(MATANYONE_WEIGHTS, w)
        (env / "models" / "MATANYONE2_LICENSE.txt").write_text(LICENSE_NOTE)
    else:
        print("[roto-env] step 4/4: MatAnyone 2 skipped (edges come from SAM only)", flush=True)

    check = subprocess.run([str(bin_python(env)), "-c",
                            "import os, tempfile, numpy as np\n"
                            "os.environ['OPENCV_IO_ENABLE_OPENEXR'] = '1'\n"
                            "import transformers, torch, cv2\n"
                            "p = os.path.join(tempfile.mkdtemp(), 't.exr')\n"
                            "assert cv2.imwrite(p, np.zeros((4, 4), np.float32)), 'OpenCV cannot write EXR'\n"
                            "print('transformers', transformers.__version__, 'torch', torch.__version__,"
                            " 'opencv', cv2.__version__, 'sam3', hasattr(transformers, 'Sam3TrackerVideoModel'))"],
                           capture_output=True, text=True, env=dict(os.environ, OPENCV_IO_ENABLE_OPENEXR="1"))
    print(check.stdout.strip() or check.stderr.strip()[-400:], flush=True)
    if check.returncode != 0:
        sys.exit("[roto-env] the environment doesn't import; see above")
    (env / "ck2-roto-env.json").write_text(json.dumps(
        {"matanyone": bool(a.matanyone) or (env / "models" / "matanyone2.pth").exists()}))
    print(f"[roto-env] ready: {env}", flush=True)


if __name__ == "__main__":
    main()
