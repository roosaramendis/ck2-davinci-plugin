#!/usr/bin/env python3
"""SAM 2.1 worker for the CorridorKey Roto node (runs in the engine's venv).

Started in the background by the plugin; one per cache folder. It watches
<root>/<job>/ for request files, so Resolve never waits on it, and quits after a
few idle minutes to give the GPU memory back.

  <job>/preview/request.json  {"seq", "frame", "objects", "model"}
      -> <job>/preview/mask_<frame>.exr (+ .png for the picker window),
         <job>/preview/done.json {"seq", "error"?}
  <job>/request.json          {"seq", "kind": "track", "frame", "objects", "model", "count"}
      -> <job>/masks/mask_NNNNNN.exr, progress "window i/N" in engine.log, exitcode
  <job>/cancel                stop tracking

objects = [{"points": [[x, y, 1|0], ...], "box": [x0, y0, x1, y1] | null}, ...]: one entry
per thing to keep (each rectangle is its own object). Coordinates are pixels of
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
        else:
            try:  # already downloaded: no network (a flaky connection only slows things down)
                model = T.Sam2VideoModel.from_pretrained(name, local_files_only=True)
                self.proc = T.Sam2VideoProcessor.from_pretrained(name, local_files_only=True)
            except OSError:
                print(f"[roto] downloading {name} (once)", flush=True)
                model = T.Sam2VideoModel.from_pretrained(name)
                self.proc = T.Sam2VideoProcessor.from_pretrained(name)
        self.model = model.to(self.device, dtype=self.dtype).eval()
        self.name = name

    def session(self, images):
        return self.proc.init_video_session(video=images, inference_device=self.device,
                                            video_storage_device="cpu", dtype=self.dtype)

    def add_objects(self, s, frame: int, objects, scale: float):
        """One SAM object per entry; returns the last model output (or None)."""
        out = None
        for oid, ob in enumerate(objects, start=1):
            kw = {}
            pts = ob.get("points") or []
            if pts:
                kw["input_points"] = [[[[float(x) * scale, float(y) * scale] for x, y, _ in pts]]]
                kw["input_labels"] = [[[int(l) for _, _, l in pts]]]
            if ob.get("box"):
                kw["input_boxes"] = [[[float(v) * scale for v in ob["box"]]]]
            if not ob.get("box") and not any(int(l) == 1 for _, _, l in pts):
                continue  # only "remove" points: nothing to keep
            self.proc.add_inputs_to_inference_session(s, frame, oid, **kw)
            out = self.model(inference_session=s, frame_idx=frame)  # must follow each add
        if out is None:
            raise ValueError("nothing to keep: click on the character or drag a box around it")
        return out

    def prob(self, pred_masks, w: int, h: int) -> np.ndarray:
        m = self.proc.post_process_masks([pred_masks.float().cpu()], original_sizes=[[h, w]],
                                         binarize=False)[0]
        p = torch.sigmoid(m[:, 0]).amax(dim=0) if m.shape[0] else torch.zeros(h, w)
        return p.numpy().astype(np.float32)


def load_rgb8(exr: Path, linear: bool):
    """Captured EXR (RGB, display-referred unless linear) -> 8-bit RGB at working size."""
    img = cv2.imread(str(exr), cv2.IMREAD_UNCHANGED)
    if img is None:
        raise FileNotFoundError(exr)
    img = img[..., :3][..., ::-1].astype(np.float32)
    if linear:
        a = np.abs(img)
        img = np.where(a <= 0.0031308, a * 12.92, 1.055 * np.power(a, 1 / 2.4) - 0.055)
    h, w = img.shape[:2]
    scale = min(1.0, MAX_SIDE / max(w, h))
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
        rgb, scale = load_rgb8(pv / f"src_{frame:06d}.exr", job_linear(job))
        h, w = rgb.shape[:2]
        with torch.inference_mode():
            s = sam.session([Image.fromarray(rgb)])
            sam.add_objects(s, 0, objects_of(req), scale)
            out = sam.model(inference_session=s, frame_idx=0)  # all objects together
            prob = sam.prob(out.pred_masks, w, h)
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


def build_sam_frames(job: Path, first: int, count: int):
    """Captured EXRs (frames first..first+count-1) -> small JPEGs kept for
    re-tracking; EXRs deleted. An existing set is reused."""
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
        rgb, scale = load_rgb8(job / "src" / f"frame_{first + i:06d}.exr", linear)
        cv2.imwrite(str(sf / f"{i:06d}.jpg"), rgb[..., ::-1], [cv2.IMWRITE_JPEG_QUALITY, 93])
        if i % 20 == 0:
            log(job, f"[roto] preparing frames {i + 1}/{count}")
    info = {"first": first, "n": count, "scale": scale}
    atomic_write(sf / "info.json", json.dumps(info))
    for p in (job / "src").glob("*.exr"):
        p.unlink()
    return info


def do_track(sam: Sam, job: Path, req: dict):
    from PIL import Image
    code = "1"
    try:
        info = build_sam_frames(job, int(req.get("first", 0)), int(req["count"]))
        first, n = int(info["first"]), int(info["n"])
        frame = int(req["frame"]) - first  # index inside the tracked range
        if not 0 <= frame < n:
            raise ValueError(f"the picked frame {frame + first} is outside the captured frames "
                             f"{first}-{first + n - 1}; pick on a frame inside them")
        sam.load(req.get("model") or DEFAULT_MODEL)
        files = sorted((job / "sam").glob("*.jpg"))
        imgs = [Image.open(p).convert("RGB") for p in files]
        w, h = imgs[0].size
        out = job / "masks"
        out.mkdir(exist_ok=True)
        done = 0
        log(job, f"[roto] tracking frames {first}-{first + n - 1} from frame {frame + first} "
                 f"with {sam.name} on {sam.device}")
        with torch.inference_mode():
            s = sam.session(imgs)
            del imgs
            sam.add_objects(s, frame, objects_of(req), info["scale"])
            for rev in (False, True):
                for o in sam.model.propagate_in_video_iterator(s, start_frame_idx=frame, reverse=rev):
                    if (job / "cancel").exists() or not job.is_dir():
                        raise KeyboardInterrupt
                    p = out / f"mask_{first + o.frame_idx:06d}.exr"
                    cv2.imwrite(str(p) + ".tmp.exr", sam.prob(o.pred_masks, w, h), EXR_HALF)
                    replace_retry(Path(str(p) + ".tmp.exr"), p)
                    done += 1
                    if done % 4 == 0 or done >= n:
                        log(job, f"      window {min(done, n)}/{n}")
        code = "0"
    except KeyboardInterrupt:
        code = "cancelled"
    except Exception as e:
        traceback.print_exc()
        log(job, f"[roto] tracking failed: {type(e).__name__}: {e}")
    if job.is_dir():
        atomic_write(job / "track_done.json", json.dumps({"seq": req["seq"]}))
        atomic_write(job / "exitcode", code)


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
