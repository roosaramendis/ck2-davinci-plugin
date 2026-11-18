#!/usr/bin/env python3
"""SAM worker for the CorridorKey Hint and Roto nodes.

Hint node: runs in the engine's venv (SAM 2.1) and writes hint masks.
Roto node: runs in <engine>/roto-env (SAM 2.1 / SAM 3 + MatAnyone 2) and writes
soft alpha mattes plus editable shapes (<job>/shapes.setting for Fusion).

Started in the background by the plugin; one per cache folder. It watches
<root>/<job>/ for request files, so Resolve never waits on it, and quits after a
few idle minutes to give the GPU memory back.

  <job>/preview/request.json  {"seq", "frame", "objects", "model"}
      -> <job>/preview/mask_<frame>.exr (+ .png for the picker window),
         <job>/preview/done.json {"seq", "error"?}
  <job>/request.json          {"seq", "kind": "track", "frame", "objects", "model", "count",
                               "matte": "matanyone"|"sam" (Roto only), "t0", "max_side"}
      -> <job>/masks/mask_NNNNNN.exr, progress "window i/N" in engine.log, exitcode
         (Roto: soft alpha mattes, and <job>/shapes.setting)
  <job>/cancel                stop tracking

objects = [{"points": [[x, y, 1|0], ...], "box": [x0, y0, x1, y1] | null,
            "regions": [lassos, see ck2_masks.py]}, ...]: one entry per thing to keep
(each rectangle is its own object). Coordinates are pixels of
the full frame, top-left origin. Masks are SAM
probabilities (0..1) at the reduced working size (long side <= 1280).
"""
from __future__ import annotations

import json
import os
import sys
import time
import traceback
from pathlib import Path

os.environ.setdefault("PYTORCH_ENABLE_MPS_FALLBACK", "1")
os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")

import cv2  # noqa: E402
import numpy as np  # noqa: E402
import torch  # noqa: E402

import ck2_masks as M  # noqa: E402  (next to this file)

ROOT = Path(sys.argv[1])
DEFAULT_MODEL = sys.argv[2] if len(sys.argv) > 2 else "facebook/sam2.1-hiera-large"
IDLE_EXIT = float(os.environ.get("CK2_ROTO_IDLE", "300"))
MAX_SIDE = 1280
EXR_HALF = [cv2.IMWRITE_EXR_TYPE, cv2.IMWRITE_EXR_TYPE_HALF]


def log(job: Path, msg: str):
    with open(job / "engine.log", "a") as f:
        f.write(msg + "\n")


def atomic_write(path: Path, text: str):
    tmp = path.with_name(path.name + f".{os.getpid()}.tmp")
    tmp.write_text(text)
    replace_retry(tmp, path)


def replace_retry(src: Path, dst: Path):
    for i in range(50):  # Windows: the plugin may be reading dst right now
        try:
            os.replace(src, dst)
            return
        except PermissionError:
            if i == 49:
                raise
            time.sleep(0.05)


def objects_of(req: dict):
    if req.get("objects") is not None:
        return req["objects"]
    return [{"points": req.get("points") or [], "box": None}]  # older requests


def read_json(path: Path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return None


def pick_device():
    want = os.environ.get("CK2_ROTO_DEVICE")
    if want:
        return want
    if torch.cuda.is_available():
        return "cuda"
    if torch.backends.mps.is_available():
        return "mps"
    return "cpu"


class Sam:
    def __init__(self):
        self.device = pick_device()
        self.dtype = torch.bfloat16 if self.device == "cuda" else torch.float32
        self.name = None
        self.model = self.proc = None

    def load(self, name: str):
        if self.name == name:
            return
        import transformers as T
        self.model = self.proc = None
        if self.device == "cuda":
            torch.cuda.empty_cache()
        if os.environ.get("CK2_ROTO_RANDOM_WEIGHTS"):  # offline testing only
            model = T.Sam2VideoModel(T.Sam2VideoConfig())
            self.proc = T.Sam2VideoProcessor(image_processor=T.Sam2ImageProcessorFast(),
                                             video_processor=T.Sam2VideoVideoProcessor())
            self.model = model.to(self.device, dtype=self.dtype).eval()
        else:
            self.model, self.proc = M.load_sam(name, self.device, self.dtype)
        self.name = name

    def session(self, images):
        return self.proc.init_video_session(video=images, inference_device=self.device,
                                            video_storage_device="cpu", dtype=self.dtype)

    def add_objects(self, s, frame: int, objects, scale: float, w: int, h: int):
        """One SAM object per entry (points, box, lassos); returns the last model output."""
        out = None
        for oid, ob in enumerate(objects, start=1):
            o = M.add_object(self.proc, self.model, s, frame, oid, ob, scale, w, h)
            out = o if o is not None else out
        if out is None:
            raise ValueError("nothing to keep: click on what you want, or drag a box or a lasso around it")
        return out

    def prob(self, pred_masks, w: int, h: int) -> np.ndarray:
        m = self.proc.post_process_masks([pred_masks.float().cpu()], original_sizes=[[h, w]],
                                         binarize=False)[0]
        p = torch.sigmoid(m[:, 0]).amax(dim=0) if m.shape[0] else torch.zeros(h, w)
        return p.numpy().astype(np.float32)


def load_rgb8(exr: Path, linear: bool, max_side: int = MAX_SIDE):
    """Captured EXR (RGB, display-referred unless linear) -> 8-bit RGB at working size."""
    img = cv2.imread(str(exr), cv2.IMREAD_UNCHANGED)
    if img is None:
        raise FileNotFoundError(exr)
    img = img[..., :3][..., ::-1].astype(np.float32)
    if linear:
        a = np.abs(img)
        img = np.where(a <= 0.0031308, a * 12.92, 1.055 * np.power(a, 1 / 2.4) - 0.055)
    h, w = img.shape[:2]
    scale = min(1.0, max_side / max(w, h)) if max_side > 0 else 1.0  # 0: full resolution (Roto)
    if scale < 1.0:
        img = cv2.resize(img, (round(w * scale), round(h * scale)), interpolation=cv2.INTER_AREA)
    return (np.clip(img, 0, 1) * 255).round().astype(np.uint8), scale


def job_linear(job: Path) -> bool:
    try:
        return "linear=1" in (job / "job.txt").read_text()
    except OSError:
        return False


def do_preview(sam: Sam, job: Path, req: dict):
    from PIL import Image
    pv = job / "preview"
    frame = int(req["frame"])
    try:
        sam.load(req.get("model") or DEFAULT_MODEL)
        rgb, scale = load_rgb8(pv / f"src_{frame:06d}.exr", job_linear(job), max_side_of(req))
        h, w = rgb.shape[:2]
        img, sam_scale = M.sam_copy(rgb)
        objs = objects_of(req)
        exact = {oid: M.exact_regions(ob, scale) for oid, ob in enumerate(objs, start=1)}
        with torch.inference_mode():
            s = sam.session([img])
            sam.add_objects(s, 0, objs, scale * sam_scale, img.size[0], img.size[1])
            out = sam.model(inference_session=s, frame_idx=0)  # all objects together
            prob, _ = M.frame_probs(sam.proc, out, s, w, h, {k: v for k, v in exact.items() if v})
        cv2.imwrite(str(pv / f"mask_{frame:06d}.exr.tmp.exr"), prob, EXR_HALF)
        replace_retry(pv / f"mask_{frame:06d}.exr.tmp.exr", pv / f"mask_{frame:06d}.exr")
        cv2.imwrite(str(pv / "mask_view.tmp.png"), (np.clip(prob, 0, 1) * 255).round().astype(np.uint8))
        replace_retry(pv / "mask_view.tmp.png", pv / "mask_view.png")
        atomic_write(pv / "done.json", json.dumps({"seq": req["seq"], "frame": frame}))
    except Exception as e:  # report, keep serving
        traceback.print_exc()
        log(job, f"[roto] preview failed: {type(e).__name__}: {e}")
        atomic_write(pv / "done.json", json.dumps({"seq": req["seq"], "frame": frame,
                                                   "error": f"{type(e).__name__}: {e}"}))


def max_side_of(req: dict) -> int:
    v = req.get("max_side")
    return MAX_SIDE if v is None else int(v)  # 0: full resolution (the Roto node)


def build_sam_frames(job: Path, first: int, count: int, max_side: int = MAX_SIDE):
    """Captured EXRs (frames first..first+count-1) -> JPEGs kept for re-tracking (Hint: at most
    max_side wide; Roto: full resolution); EXRs deleted. An existing set is reused."""
    sf = job / "sam"
    if (sf / "info.json").exists():
        info = read_json(sf / "info.json")
        info.setdefault("first", 0)
        return info
    sf.mkdir(exist_ok=True)
    linear = job_linear(job)
    scale = 1.0
    for i in range(count):
        if (job / "cancel").exists() or not job.is_dir():
            raise KeyboardInterrupt
        rgb, scale = load_rgb8(job / "src" / f"frame_{first + i:06d}.exr", linear, max_side)
        cv2.imwrite(str(sf / f"{i:06d}.jpg"), rgb[..., ::-1],
                    [cv2.IMWRITE_JPEG_QUALITY, 97 if max_side <= 0 else 93])
        if i % 20 == 0:
            log(job, f"[roto] preparing frames {i + 1}/{count}")
    info = {"first": first, "n": count, "scale": scale, "max_side": max_side}
    atomic_write(sf / "info.json", json.dumps(info))
    for p in (job / "src").glob("*.exr"):
        p.unlink()
    return info


def notify(job: Path, ok: bool):
    """Desktop notification when tracking ends, if the node asked for one (<job>/notify)."""
    import shutil
    import subprocess
    try:
        node = (job / "notify").read_text().splitlines()[0].strip() or "CorridorKey Hint"
    except (OSError, IndexError):
        return
    title = f"{node}: tracking " + ("done" if ok else "failed")
    body = ("Now press Key clip on the CorridorKey node." if ok and "Hint" in node
            else "The roto matte is ready." if ok
            else f"See the Status line on the {node.replace('CorridorKey ', '')} node for details.")
    try:
        env = {k: v for k, v in os.environ.items()
               if not k.upper().startswith(("LD_", "QT_", "PYTHON", "DYLD_"))}
        quiet = {"stdin": subprocess.DEVNULL, "stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL, "env": env}
        if sys.platform == "darwin":
            def q(t):
                return t.replace("\\", "\\\\").replace('"', '\\"')
            subprocess.Popen(["osascript", "-e", 'display notification "%s" with title "%s"' % (q(body), q(title))],
                             start_new_session=True, **quiet)
        elif os.name == "nt":
            env["CK2_TITLE"], env["CK2_BODY"] = title, body
            ps = ("Add-Type -AssemblyName System.Windows.Forms; Add-Type -AssemblyName System.Drawing; "
                  "$n = New-Object System.Windows.Forms.NotifyIcon; "
                  "$n.Icon = [System.Drawing.SystemIcons]::Information; $n.Visible = $true; "
                  "$n.ShowBalloonTip(15000, $env:CK2_TITLE, $env:CK2_BODY, 'Info'); "
                  "Start-Sleep -Seconds 15; $n.Dispose()")
            subprocess.Popen(["powershell", "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden",
                              "-Command", ps], creationflags=0x08000000, **quiet)
        elif shutil.which("notify-send", path=env.get("PATH")):
            subprocess.Popen(["notify-send", "-a", "CorridorKey", "-i", "dialog-information" if ok else "dialog-error",
                              title, body], start_new_session=True, **quiet)
        elif shutil.which("gdbus", path=env.get("PATH")):
            subprocess.Popen(["gdbus", "call", "--session", "--dest", "org.freedesktop.Notifications",
                              "--object-path", "/org/freedesktop/Notifications",
                              "--method", "org.freedesktop.Notifications.Notify", "CorridorKey", "0",
                              "dialog-information", json.dumps(title), json.dumps(body), "[]", "{}", "15000"],
                             start_new_session=True, **quiet)
    except Exception as e:
        log(job, f"[roto] notification failed: {e}")


def do_track(sam: Sam, job: Path, req: dict):
    from PIL import Image
    code = "1"
    matte = req.get("matte")  # Roto node: "matanyone" | "sam"; Hint node: None
    try:
        max_side = max_side_of(req)
        info = build_sam_frames(job, int(req.get("first", 0)), int(req["count"]), max_side)
        first, n = int(info["first"]), int(info["n"])
        frame = int(req["frame"]) - first  # index inside the tracked range
        if not 0 <= frame < n:
            raise ValueError(f"the picked frame {frame + first} is outside the captured frames "
                             f"{first}-{first + n - 1}; pick on a frame inside them")
        env_dir = Path(sys.prefix)  # <engine>/roto-env when run by the Roto node
        use_ma = matte == "matanyone" and M.matanyone_ready(env_dir)
        if matte == "matanyone" and not use_ma:
            log(job, "[roto] MatAnyone 2 isn't installed: edges come from SAM (install it with the "
                     "Roto models in ck2-gui Setup, or install.sh --roto --matanyone)")
        steps = n * (2 if use_ma else 1)  # progress: SAM pass (+ MatAnyone pass)
        sam.load(req.get("model") or DEFAULT_MODEL)
        files = sorted((job / "sam").glob("*.jpg"))
        imgs, sam_scale = [], 1.0
        for p in files:  # one at a time: only SAM's own size stays in memory
            im, sam_scale = M.sam_copy(Image.open(p).convert("RGB"))
            imgs.append(im)
        with Image.open(files[0]) as im0:
            w, h = im0.size  # the masks are made at this size (Roto: full resolution)
        sw, sh = imgs[0].size
        out = job / "masks"
        out.mkdir(exist_ok=True)
        objs = objects_of(req)
        exact = {oid: M.exact_regions(ob, info["scale"]) for oid, ob in enumerate(objs, start=1)}
        exact = {k: v for k, v in exact.items() if v}
        per_dir = job / "objects" if matte else None  # Roto: each object's mask, for the shapes
        if per_dir is not None:
            import shutil
            shutil.rmtree(per_dir, ignore_errors=True)
        done = 0
        log(job, f"[roto] tracking frames {first}-{first + n - 1} from frame {frame + first} "
                 f"with {sam.name} on {sam.device}, masks at {w}x{h}" + (" + MatAnyone 2" if use_ma else ""))
        with torch.inference_mode():
            s = sam.session(imgs)
            del imgs
            sam.add_objects(s, frame, objs, info["scale"] * sam_scale, sw, sh)
            for rev in (False, True):
                for o in sam.model.propagate_in_video_iterator(s, start_frame_idx=frame, reverse=rev):
                    if (job / "cancel").exists() or not job.is_dir():
                        raise KeyboardInterrupt
                    prob, per = M.frame_probs(sam.proc, o, s, w, h, exact if o.frame_idx == frame else None)
                    p = out / f"mask_{first + o.frame_idx:06d}.exr"
                    cv2.imwrite(str(p) + ".tmp.exr", prob, EXR_HALF)
                    replace_retry(Path(str(p) + ".tmp.exr"), p)
                    if per_dir is not None:
                        for oid, po in per.items():
                            d = per_dir / str(oid)
                            d.mkdir(parents=True, exist_ok=True)
                            cv2.imwrite(str(d / f"{o.frame_idx:06d}.png"), (po >= 0.5).astype(np.uint8) * 255)
                    del prob, per
                    done += 1
                    if done % 4 == 0 or done >= n:
                        log(job, f"      window {min(done, n)}/{steps}")
            anchor = cv2.imread(str(out / f"mask_{first + frame:06d}.exr"), cv2.IMREAD_UNCHANGED)
        if use_ma:
            del s
            if sam.device == "cuda":
                torch.cuda.empty_cache()
            log(job, "[roto] soft edges with MatAnyone 2 (hair, motion blur)")
            mt = M.Matter(M.matanyone_weights(env_dir), sam.device)

            def write(i, a):
                p = out / f"mask_{first + i:06d}.exr"
                cv2.imwrite(str(p) + ".tmp.exr", a, EXR_HALF)
                replace_retry(Path(str(p) + ".tmp.exr"), p)

            def progress(k):
                if k % 4 == 0 or k >= n:
                    log(job, f"      window {n + min(k, n)}/{steps}")

            used = mt.run_full(files, frame, (anchor >= 0.5).astype(np.uint8), write,
                               cancelled=lambda: (job / "cancel").exists() or not job.is_dir(),
                               progress=progress, log=lambda t: log(job, t))
            log(job, f"[roto] MatAnyone 2 at {'full resolution' if used is None else f'{used} lines'}")
            del mt
        if per_dir is not None and per_dir.is_dir():
            t0 = int(round(float(req.get("t0", 0))))
            shapes = {f"Object {d.name}": {t0 + first + f: pts for f, pts in M.track_shape(M.MaskFolder(d)).items()}
                      for d in sorted((x for x in per_dir.iterdir() if x.is_dir()), key=lambda x: int(x.name))}
            full_w, full_h = round(w / info["scale"]), round(h / info["scale"])
            atomic_write(job / "shapes.setting", M.fusion_setting(shapes, full_w, full_h, info["scale"]))
            log(job, f"[roto] shapes: {sum(len(v) for v in shapes.values())} keyframes in shapes.setting")
        code = "0"
    except KeyboardInterrupt:
        code = "cancelled"
    except Exception as e:
        traceback.print_exc()
        log(job, f"[roto] tracking failed: {type(e).__name__}: {e}")
    if job.is_dir():
        atomic_write(job / "track_done.json", json.dumps({"seq": req["seq"]}))
        atomic_write(job / "exitcode", code)
        if code != "cancelled":
            notify(job, code == "0")


def single_instance():
    """Only one helper per cache folder (each one would load SAM 2 onto the GPU)."""
    f = open(ROOT / "daemon.lock", "a+")
    try:
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return None
    return f  # keep open = keep the lock


def heartbeat():
    """Runs in the background so loading/downloading the model never looks like a dead helper."""
    while True:
        try:
            atomic_write(ROOT / "daemon.alive", f"{os.getpid()} {time.time():.0f}\n")
        except OSError:
            pass
        time.sleep(2)


def main():
    import threading
    ROOT.mkdir(parents=True, exist_ok=True)
    lock = single_instance()
    if lock is None:
        return  # another helper is already running
    sys.stdout = sys.stderr = open(ROOT / "daemon.log", "a", buffering=1)
    threading.Thread(target=heartbeat, daemon=True).start()
    sam = Sam()
    last_work = time.time()
    print(f"[roto] daemon {os.getpid()} on {sam.device}, watching {ROOT}", flush=True)
    while True:
        busy = False
        for job in sorted(p for p in ROOT.iterdir() if p.is_dir()):
            req = read_json(job / "preview" / "request.json")
            done = read_json(job / "preview" / "done.json") or {}
            if req and req.get("seq", 0) > done.get("seq", 0):
                do_preview(sam, job, req)
                busy = True
            req = read_json(job / "request.json")
            done = read_json(job / "track_done.json") or {}
            if req and req.get("seq", 0) > done.get("seq", 0) and not (job / "exitcode").exists():
                do_track(sam, job, req)
                busy = True
        if busy:
            last_work = time.time()
        elif time.time() - last_work > IDLE_EXIT:
            break
        time.sleep(0.25)
    try:
        (ROOT / "daemon.alive").unlink()
    except OSError:
        pass


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
