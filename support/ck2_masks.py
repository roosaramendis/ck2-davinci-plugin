"""Shared mask tools for CorridorKey Hint and Roto.

The same file ships with ck2-gui (ck2/ck2_masks.py) and ck2-davinci-plugin
(support/ck2_masks.py). It runs in the engine's venv (Hint: SAM 2.1) or in the
optional Roto environment (<engine>/roto-env: SAM 2.1 / SAM 3 + MatAnyone 2).

Prompt objects (pixel coordinates of the picked frame, top-left origin):
    {"points": [[x, y, 1|0], ...],          1 = keep, 0 = remove
     "box": [x0, y0, x1, y1] | null,
     "regions": [{"poly": [[x, y], ...],    a lasso
                  "op": 1|0,                1 = add, 0 = cut away
                  "exact": true|false}]}    exact: the edge is where you drew it;
                                            false: SAM finds the edge inside it
"""
from __future__ import annotations

import os
from pathlib import Path

import cv2
import numpy as np

# ============================================================ prompts / lasso


def parts(ob: dict, scale: float = 1.0):
    """(points [[x, y, l]], box | None, regions) scaled to the working size."""
    pts = [[float(x) * scale, float(y) * scale, int(l)] for x, y, l in (ob.get("points") or [])]
    box = [float(v) * scale for v in ob["box"]] if ob.get("box") else None
    regs = []
    for r in ob.get("regions") or []:
        poly = [[float(x) * scale, float(y) * scale] for x, y in r.get("poly") or []]
        if len(poly) >= 3:
            regs.append({"poly": poly, "op": int(r.get("op", 1)), "exact": bool(r.get("exact", True))})
    return pts, box, regs


def poly_mask(poly, w: int, h: int) -> np.ndarray:
    """Filled polygon -> uint8 0/1 (h, w)."""
    m = np.zeros((h, w), np.uint8)
    p = np.round(np.asarray(poly, np.float64) * 16).astype(np.int32)  # 4 bits of sub-pixel precision
    cv2.fillPoly(m, [p], 1, lineType=cv2.LINE_8, shift=4)
    return m


def interior_points(mask: np.ndarray, k: int = 3) -> list[list[float]]:
    """Up to k well-spread points deep inside a 0/1 mask (for SAM point prompts)."""
    if mask.sum() == 0:
        return []
    d = cv2.distanceTransform(np.pad(mask, 1).astype(np.uint8), cv2.DIST_L2, 5)[1:-1, 1:-1]
    pts = []
    for _ in range(k):
        y, x = np.unravel_index(int(np.argmax(d)), d.shape)
        if d[y, x] <= 0.5:
            break
        pts.append([float(x), float(y)])
        r = max(3, int(d[y, x] * 1.5))
        cv2.circle(d, (int(x), int(y)), r, 0, -1)  # next point elsewhere
    return pts


def guide_prompts(regions, w: int, h: int):
    """Lassos with exact=False -> extra SAM points (+ a box around the added areas)."""
    pts, xs, ys = [], [], []
    for r in regions:
        m = poly_mask(r["poly"], w, h)
        for x, y in interior_points(m, 3 if r["op"] else 2):
            pts.append([x, y, 1 if r["op"] else 0])
        if r["op"]:
            p = np.asarray(r["poly"])
            xs += [p[:, 0].min(), p[:, 0].max()]
            ys += [p[:, 1].min(), p[:, 1].max()]
    box = [max(0.0, min(xs)), max(0.0, min(ys)), min(w - 1.0, max(xs)), min(h - 1.0, max(ys))] if xs else None
    return pts, box


def compose(prob: np.ndarray | None, regions, w: int, h: int) -> np.ndarray:
    """Exact lassos on top of a SAM probability map: add / cut away, drawn order."""
    out = np.zeros((h, w), np.float32) if prob is None else prob.astype(np.float32).copy()
    for r in regions:
        m = poly_mask(r["poly"], w, h).astype(bool)
        out[m] = 1.0 if r["op"] else 0.0
    return out


def keeps_something(ob: dict) -> bool:
    pts, box, regs = parts(ob)
    return bool(box) or any(l == 1 for *_, l in pts) or any(r["op"] for r in regs)


# ============================================================ SAM 2.1 / SAM 3


def is_sam3(name: str) -> bool:
    return "sam3" in (name or "").lower()


def load_sam(name: str, device: str, dtype, local_first: bool = True):
    """(model, processor) for a SAM 2.1 or SAM 3 video checkpoint (Hugging Face name)."""
    import transformers as T
    if is_sam3(name):
        if not hasattr(T, "Sam3TrackerVideoModel"):
            raise RuntimeError("SAM 3 needs the Roto models (transformers 5); install them in "
                               "ck2-gui: Setup -> Roto models, or pick a SAM 2.1 model")
        M, P = T.Sam3TrackerVideoModel, T.Sam3TrackerVideoProcessor
    else:
        M, P = T.Sam2VideoModel, T.Sam2VideoProcessor
    token = os.environ.get("HF_TOKEN") or None
    try:
        if not local_first:
            raise OSError
        model = M.from_pretrained(name, local_files_only=True)
        proc = P.from_pretrained(name, local_files_only=True)
    except OSError:
        print(f"[sam] downloading {name} (once)", flush=True)
        try:
            model = M.from_pretrained(name, token=token)
            proc = P.from_pretrained(name, token=token)
        except OSError as e:
            if is_sam3(name) and ("gated" in str(e).lower() or "403" in str(e) or "401" in str(e)):
                raise RuntimeError(
                    "SAM 3 is gated by Meta: request access on huggingface.co/facebook/sam3 with a "
                    "free Hugging Face account, then add your access token (ck2-gui: Setup -> Roto "
                    "models; plugin: hf_token=... in ofx.conf). Or pick a SAM 2.1 model.") from e
            raise
    return model.to(device, dtype=dtype).eval(), proc


def object_prob(proc, out, session, oid: int, w: int, h: int) -> np.ndarray:
    """One object's probability map from a model output."""
    import torch
    m = proc.post_process_masks([out.pred_masks.float().cpu()], original_sizes=[[h, w]], binarize=False)[0]
    idx = session.obj_id_to_idx(oid)
    if idx >= m.shape[0]:
        return np.zeros((h, w), np.float32)
    return torch.sigmoid(m[idx, 0]).numpy().astype(np.float32)


SAM_SHORT = 1024  # SAM resizes every frame to 1024 x 1024 inside: more pixels never reach it


def sam_copy(img, short: int = SAM_SHORT):
    """(RGB image for SAM, scale). Big frames are shrunk only to what SAM uses anyway (short
    side 1024), so 4K clips don't fill the memory; masks are still made at full size."""
    from PIL import Image
    if isinstance(img, np.ndarray):
        img = Image.fromarray(img)
    w, h = img.size
    s = min(1.0, short / min(w, h))
    if s < 1.0:
        img = img.resize((max(1, round(w * s)), max(1, round(h * s))), Image.Resampling.LANCZOS)
    return img, s


def exact_regions(ob: dict, scale: float = 1.0) -> list:
    return [r for r in parts(ob, scale)[2] if r["exact"]]


def frame_probs(proc, out, session, w: int, h: int, exact: dict | None = None):
    """(union, {object id: probability}) at w x h, any size: SAM's mask is scaled straight to
    it. exact = {object id: exact lassos in w x h coords} for this frame: their edges are drawn
    again at this size, so a lasso is pixel-exact at full resolution."""
    import torch
    m = proc.post_process_masks([out.pred_masks.float().cpu()], original_sizes=[[h, w]], binarize=False)[0]
    per = {}
    for oid in session.obj_ids:
        idx = session.obj_id_to_idx(oid)
        p = (torch.sigmoid(m[idx, 0]).numpy().astype(np.float32) if idx < m.shape[0]
             else np.zeros((h, w), np.float32))
        regs = (exact or {}).get(oid)
        per[oid] = compose(p, regs, w, h) if regs else p
    del m
    union = np.maximum.reduce(list(per.values())) if per else np.zeros((h, w), np.float32)
    return union, per


class MaskFolder:
    """frame -> 0/1 mask, read from <dir>/<frame>.png only when asked (long 4K clips)."""

    def __init__(self, folder: Path):
        self.files = {int(p.stem): p for p in Path(folder).glob("*.png")}

    def __iter__(self):
        return iter(self.files)

    def __len__(self):
        return len(self.files)

    def __getitem__(self, f):
        return (cv2.imread(str(self.files[f]), cv2.IMREAD_GRAYSCALE) >= 128).astype(np.uint8)


def all_prob(proc, pred_masks, w: int, h: int) -> np.ndarray:
    """All objects together (max)."""
    import torch
    m = proc.post_process_masks([pred_masks.float().cpu()], original_sizes=[[h, w]], binarize=False)[0]
    p = torch.sigmoid(m[:, 0]).amax(dim=0) if m.shape[0] else torch.zeros(h, w)
    return p.numpy().astype(np.float32)


def add_object(proc, model, session, frame: int, oid: int, ob: dict, scale: float, w: int, h: int):
    """Adds one object (points, box, lassos) on `frame`; returns the model output or None
    if it keeps nothing. Exact lassos become a mask prompt: SAM tracks that exact shape."""
    pts, box, regs = parts(ob, scale)
    guide = [r for r in regs if not r["exact"]]
    exact = [r for r in regs if r["exact"]]
    gp, gbox = guide_prompts(guide, w, h)
    pts = pts + gp
    if box is None:
        box = gbox
    if not (box or any(l == 1 for *_, l in pts) or any(r["op"] for r in exact)):
        return None
    out = None
    if box or any(l == 1 for *_, l in pts):
        kw = {}
        if pts:
            kw["input_points"] = [[[[x, y] for x, y, _ in pts]]]
            kw["input_labels"] = [[[int(l) for *_, l in pts]]]
        if box:
            kw["input_boxes"] = [[box]]
        proc.add_inputs_to_inference_session(session, frame, oid, **kw)
        out = model(inference_session=session, frame_idx=frame)  # must follow each add
    if exact:
        prob = object_prob(proc, out, session, oid, w, h) if out is not None else None
        m = compose(prob, exact, w, h) >= 0.5
        if not m.any():
            return out
        proc.add_inputs_to_inference_session(session, frame, [oid], input_masks=[m])
        out = model(inference_session=session, frame_idx=frame)
    return out


# ============================================================ MatAnyone 2 (Roto edges)

MATANYONE_URL = "https://github.com/pq-yang/MatAnyone2/releases/download/v1.0.0/matanyone2.pth"
MATANYONE_LICENSE = (
    "MatAnyone 2 (c) S-Lab, NTU, under the NTU S-Lab License 1.0: free for NON-COMMERCIAL use "
    "only. Commercial use (paid or client work) needs permission from its authors "
    "(peiqingyang99@outlook.com). It is not part of CorridorKey and is installed only if you agree.")


def matanyone_weights(env_dir: Path) -> Path:
    return Path(env_dir) / "models" / "matanyone2.pth"


def matanyone_ready(env_dir: Path | None) -> bool:
    if env_dir is None or not matanyone_weights(env_dir).exists():
        return False
    try:
        import importlib.util
        return importlib.util.find_spec("matanyone2") is not None
    except (ImportError, ValueError):
        return False


class Matter:
    """MatAnyone 2: a first-frame mask -> soft alpha mattes (hair, motion blur) through the
    clip. Frames are read one at a time, so long 4K clips don't fill the memory."""

    def __init__(self, weights: Path, device: str):
        import torch
        from hydra import compose, initialize_config_dir
        from hydra.core.global_hydra import GlobalHydra
        from omegaconf import open_dict
        import matanyone2
        from matanyone2.model.matanyone2 import MatAnyone2
        GlobalHydra.instance().clear()
        self.torch = torch
        self.device = torch.device(device)
        cfg_dir = str(Path(matanyone2.__file__).parent / "config")
        with initialize_config_dir(version_base="1.3.2", config_dir=cfg_dir, job_name="ck2_roto"):
            cfg = compose(config_name="eval_matanyone_config")
        with open_dict(cfg):
            cfg["weights"] = str(weights)
            cfg.model["pretrained_resnet"] = False  # its own weights replace these anyway: no download
        self.model = MatAnyone2(cfg, single_object=True).to(self.device).eval()
        self.model.load_weights(torch.load(str(weights), map_location=self.device))

    def run_full(self, frames: list, anchor: int, mask: np.ndarray, write, cancelled=lambda: False,
                 progress=lambda i: None, log=print):
        """run() at the frames' full resolution. If the GPU runs out of memory, it tries again
        smaller (2160, 1440, 1080 lines) and says so. Returns the lines used (None: full)."""
        h, w = cv2.imread(str(frames[anchor])).shape[:2]
        tries = [None] + [n for n in (2160, 1440, 1080) if n < min(w, h)]
        for k, max_short in enumerate(tries):
            try:
                self.run(frames, anchor, mask, write, cancelled, max_short=max_short, progress=progress)
                return max_short
            except Exception as e:
                oom = isinstance(e, self.torch.OutOfMemoryError) or "out of memory" in str(e).lower()
                if not oom or k == len(tries) - 1:
                    raise
                self.torch.cuda.empty_cache() if self.device.type == "cuda" else None
                log(f"[roto] MatAnyone 2: not enough GPU memory at {f'{min(w, h)}' if max_short is None else max_short} "
                    f"lines, trying {tries[k + 1]} (the matte is scaled back up to {w}x{h})")

    def run(self, frames: list, anchor: int, mask: np.ndarray, write, cancelled=lambda: False,
            max_short: int | None = None, warmup: int = 10, progress=lambda i: None):
        """frames: image paths (any size, RGB/BGR files); mask: 0/1 at frame size, on frame
        `anchor`. Goes forward from anchor to the end, then backward to the start.
        write(i, alpha float32 (h, w) at the frames' size). max_short: work at most this many
        lines (None: full resolution)."""
        torch = self.torch
        from matanyone2.inference.inference_core import InferenceCore
        from matanyone2.utils.device import safe_autocast
        import torch.nn.functional as F

        first = cv2.imread(str(frames[anchor]))
        h, w = first.shape[:2]
        s = min(1.0, max_short / min(w, h)) if max_short else 1.0
        nw, nh = max(16, round(w * s)), max(16, round(h * s))

        def load(i):
            img = cv2.imread(str(frames[i]))[..., ::-1]
            t = torch.from_numpy(np.ascontiguousarray(img)).permute(2, 0, 1).float()
            if (nw, nh) != (w, h):
                t = F.interpolate(t[None], size=(nh, nw), mode="area")[0]
            return (t / 255.0).to(self.device)

        m = (cv2.resize(mask.astype(np.uint8), (nw, nh), interpolation=cv2.INTER_NEAREST) > 0)
        m255 = m.astype(np.float32) * 255
        k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (10, 10))  # same clean-up as MatAnyone's own demo
        m255 = cv2.dilate((m255 > 0).astype(np.float32), k) * 255
        m255 = cv2.erode((m255 == 255).astype(np.float32), k) * 255
        mask_t = torch.from_numpy(m255).float().to(self.device)

        done = 0
        for pas, order in enumerate((list(range(anchor, len(frames))), list(range(anchor, -1, -1)))):
            proc = InferenceCore(self.model, cfg=self.model.cfg, device=self.device)
            with torch.inference_mode(), safe_autocast():
                img0 = load(order[0])
                proc.step(img0, mask_t, objects=[1])
                prob = proc.step(img0, first_frame_pred=True)
                for _ in range(warmup):
                    prob = proc.step(img0, first_frame_pred=True)
                for n, i in enumerate(order):
                    if cancelled():
                        raise KeyboardInterrupt
                    if n:
                        prob = proc.step(load(i))
                    elif pas:
                        continue  # the anchor frame was written on the way forward
                    a = proc.output_prob_to_mask(prob).float().clamp(0, 1).cpu().numpy()
                    if a.shape != (h, w):
                        a = cv2.resize(a, (w, h), interpolation=cv2.INTER_CUBIC)
                    write(i, np.clip(a, 0, 1).astype(np.float32))
                    done += 1
                    progress(done)
            del proc
            if self.device.type == "cuda":
                torch.cuda.empty_cache()


# ============================================================ editable shapes (Fusion)


def _largest_contour(m: np.ndarray):
    cs, _ = cv2.findContours(m.astype(np.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    if not cs:
        return None
    c = max(cs, key=cv2.contourArea)
    if cv2.contourArea(c) < 16:
        return None
    return c[:, 0, :].astype(np.float64)


def _resample(c: np.ndarray, n: int) -> np.ndarray:
    """Closed contour -> n points evenly spaced along it."""
    d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(np.vstack([c, c[:1]]), axis=0), axis=1))]
    t = np.linspace(0, d[-1], n, endpoint=False)
    cc = np.vstack([c, c[:1]])
    return np.stack([np.interp(t, d, cc[:, 0]), np.interp(t, d, cc[:, 1])], axis=1)


def _signed_area(p: np.ndarray) -> float:
    x, y = p[:, 0], p[:, 1]
    return 0.5 * float(np.dot(x, np.roll(y, -1)) - np.dot(np.roll(x, -1), y))


def track_shape(masks: dict, n: int | None = None) -> dict:
    """{frame: 0/1 mask} -> {frame: (n, 2) points}: same point count and the same start /
    direction on every frame, so a roto shape can be keyframed (and edited) in Fusion."""
    frames = sorted(masks)
    shapes, prev = {}, None
    for f in frames:
        c = _largest_contour(masks[f])
        if c is None:
            continue
        if n is None:  # from the first usable frame: about one point per 30 px of outline
            per = float(np.sum(np.linalg.norm(np.diff(np.vstack([c, c[:1]]), axis=0), axis=1)))
            n = int(min(160, max(24, per / 30)))
        p = _resample(c, n)
        if _signed_area(p) < 0:
            p = p[::-1]
        if prev is not None:  # rotate so point 0 is where it was on the previous frame
            best = min(range(n), key=lambda k: float(np.sum((np.roll(p, -k, axis=0) - prev) ** 2)))
            p = np.roll(p, -best, axis=0)
        else:
            p = np.roll(p, -int(np.argmin(p[:, 1] * 1e6 + p[:, 0])), axis=0)  # start at the top
        shapes[f] = p
        prev = p
    return shapes


def fusion_setting(objects: dict, width: int, height: int, scale: float = 1.0, name: str = "CK2Roto") -> str:
    """objects {label: {frame_time: (n, 2) points in working pixels}} -> a Fusion .setting with
    one keyframed Polygon mask per object. Points are divided by `scale` to get full-frame pixels.
    Fusion polyline coordinates: origin at the image centre, 1.0 = image width (both axes)."""
    W, H = float(width), float(height)

    def num(v):
        return f"{v:.6f}".rstrip("0").rstrip(".") if v else "0"

    lines = ["{", "\tTools = ordered() {"]
    for oi, (label, shapes) in enumerate(objects.items(), start=1):
        if not shapes:
            continue
        tool = f"{name}{oi}"
        lines += [f"\t\t{tool} = PolylineMask {{",
                  '\t\t\tDrawMode = "ModifyOnly",',
                  '\t\t\tDrawMode2 = "InsertAndModify",',
                  "\t\t\tCtrlWZoom = false,",
                  '\t\t\tNameSet = true,',
                  "\t\t\tInputs = {",
                  f"\t\t\t\tMaskWidth = Input {{ Value = {int(W)}, }},",
                  f"\t\t\t\tMaskHeight = Input {{ Value = {int(H)}, }},",
                  "\t\t\t\tPixelAspect = Input { Value = { 1, 1 }, },",
                  '\t\t\t\tClippingMode = Input { Value = FuID { "None" }, },',
                  f'\t\t\t\tPolyline = Input {{ SourceOp = "{tool}Polyline", Source = "Value", }},',
                  "\t\t\t\tPolyline2 = Input { Value = Polyline { }, Disabled = true, },",
                  "\t\t\t},",
                  f"\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 0, {oi * 50} }} }},",
                  "\t\t},",
                  f"\t\t{tool}Polyline = BezierSpline {{",
                  "\t\t\tSplineColor = { Red = 173, Green = 255, Blue = 47 },",
                  "\t\t\tNameSet = true,",
                  "\t\t\tKeyFrames = {"]
        for t in sorted(shapes):
            p = np.asarray(shapes[t], np.float64) / scale
            x = p[:, 0] / W - 0.5
            y = (H / 2 - p[:, 1]) / W
            nx, ny = np.roll(x, -1), np.roll(y, -1)
            px, py = np.roll(x, 1), np.roll(y, 1)
            tx, ty = (nx - px) / 6, (ny - py) / 6  # smooth (Catmull-Rom) handles
            pts = ", ".join(
                f"{{ X = {num(x[i])}, Y = {num(y[i])}, LX = {num(-tx[i])}, LY = {num(-ty[i])}, "
                f"RX = {num(tx[i])}, RY = {num(ty[i])} }}" for i in range(len(x)))
            lines.append(f"\t\t\t\t[{int(t)}] = {{ 0, Flags = {{ Linear = true, LockedY = true }}, "
                         f"Value = Polyline {{ Closed = true, Points = {{ {pts} }} }} }},")
        lines += ["\t\t\t}", "\t\t},"]
    lines += ["\t},", "\tActiveTool = \"" + f"{name}1" + "\"", "}"]
    return "\n".join(lines) + "\n"
