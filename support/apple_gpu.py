#!/usr/bin/env python3
"""Run the official CorridorKey 2.0 engine on an Apple GPU (PyTorch MPS)
without modifying the engine's files, so `git pull` updates keep working.

The engine takes its device from load_model(device=...) and otherwise follows
tensors' own .device. The only CUDA-specific calls are torch.cuda.*, .cuda()
and autocast("cuda"); this wrapper redirects those to MPS before the engine
is imported.

Usage (run with the engine's venv python):
    python apple_gpu.py <engine_dir> infer <infer.py args...>
    python apple_gpu.py <engine_dir> smoke [--load_model]
    python apple_gpu.py <engine_dir> probe

Env:
    CK2_GPU_DEVICE        mps (default) | cpu (testing on non-Macs)
    CK2_MPS_BUDGET_SCALE  share of macOS's recommended GPU memory the auto
                          window picker may plan for (default 0.6; MPS needs
                          more headroom than the CUDA-calibrated estimates)
"""
from __future__ import annotations

import os
import platform
import runpy
import sys
from pathlib import Path

# Ops MPS doesn't implement run on the CPU instead of crashing.
os.environ.setdefault("PYTORCH_ENABLE_MPS_FALLBACK", "1")

import torch  # noqa: E402

DEV = os.environ.get("CK2_GPU_DEVICE", "mps")
BUDGET_SCALE = float(os.environ.get("CK2_MPS_BUDGET_SCALE", "0.6"))
GiB = 2 ** 30


def _mps_ok() -> tuple[bool, str]:
    if DEV != "mps":
        return True, f"test device {DEV}"
    if not torch.backends.mps.is_available():
        why = ("this PyTorch build has no MPS support" if not torch.backends.mps.is_built()
               else "macOS doesn't expose the Apple GPU to PyTorch here")
        return False, f"Apple GPU (MPS) is not available: {why}."
    if not torch.backends.mps.is_macos_or_newer(14, 0):
        return False, ("macOS 14 (Sonoma) or newer is needed: CorridorKey runs in bfloat16, "
                       "which MPS only supports from macOS 14.")
    return True, "Apple GPU (MPS) available"


def _budget_bytes() -> int:
    if DEV == "mps":
        return int(torch.mps.recommended_max_memory() * BUDGET_SCALE)
    return 16 * GiB


def _used_bytes() -> int:
    return torch.mps.driver_allocated_memory() if DEV == "mps" else 0


def patch_torch():
    cuda = torch.cuda
    cuda.is_available = lambda: True
    # (free, total) like CUDA; runtime adds memory_reserved() back on top.
    cuda.mem_get_info = lambda *a, **k: (max(0, _budget_bytes() - _used_bytes()),
                                         _budget_bytes())
    cuda.memory_reserved = lambda *a, **k: _used_bytes()
    if DEV == "mps":
        cuda.empty_cache = torch.mps.empty_cache
        cuda.synchronize = lambda *a, **k: torch.mps.synchronize()
    else:
        cuda.empty_cache = lambda: None
        cuda.synchronize = lambda *a, **k: None

    torch.nn.Module.cuda = lambda self, device=None: self.to(DEV)
    torch.Tensor.cuda = (lambda self, device=None, non_blocking=False, **k:
                         self.to(DEV, non_blocking=non_blocking))

    base = torch.autocast

    class autocast(base):  # noqa: N801  (same name the engine calls)
        def __init__(self, device_type, *args, **kwargs):
            if device_type == "cuda":
                device_type = DEV
            super().__init__(device_type, *args, **kwargs)

    torch.autocast = autocast
    patch_adaptive_pools()
    patch_attention()


ATTN_LIMIT = int(float(os.environ.get("CK2_MPS_ATTN_CHUNK_MB", "1024")) * 2**20)


def _chunked_sdpa(original):
    """CUDA's flash/memory-efficient attention never stores the full
    (queries × keys) score matrix; MPS does. For full-frame attention over an
    80-frame window at 1080p that matrix is ~89 GB. Split the call along the
    batch and query dimensions so each piece's score matrix stays under
    ATTN_LIMIT. Every query row is computed independently, so the result is
    identical, just produced in pieces."""
    def sdpa(query, key, value, attn_mask=None, dropout_p=0.0, is_causal=False,
             scale=None, **kwargs):
        if (query.device.type != DEV or is_causal or dropout_p or query.dim() < 3):
            return original(query, key, value, attn_mask=attn_mask, dropout_p=dropout_p,
                            is_causal=is_causal, scale=scale, **kwargs)
        L, S = query.shape[-2], key.shape[-2]
        heads = 1
        for n in query.shape[1:-2]:
            heads *= n
        B = query.shape[0]
        row_bytes = heads * S * 4  # float32 scores per query row, per batch item
        if B * L * row_bytes <= ATTN_LIMIT:
            return original(query, key, value, attn_mask=attn_mask, dropout_p=dropout_p,
                            is_causal=is_causal, scale=scale, **kwargs)
        if not sdpa.warned:
            print(f"[apple-gpu] attention needs {B * L * row_bytes / 2**30:.1f} GB in one go; "
                  f"splitting it into ≤{ATTN_LIMIT / 2**20:.0f} MB pieces", flush=True)
            sdpa.warned = True
        nb = max(1, min(B, ATTN_LIMIT // max(1, L * row_bytes)))
        lq = L if nb > 1 or L * row_bytes <= ATTN_LIMIT else max(1, ATTN_LIMIT // row_bytes)
        out = query.new_empty((*query.shape[:-1], value.shape[-1]))

        def bslice(t, b0, b1):
            return t[b0:b1] if t is not None and t.dim() == query.dim() and t.shape[0] == B else t

        for b0 in range(0, B, nb):
            b1 = min(B, b0 + nb)
            qb, kb, vb = query[b0:b1], bslice(key, b0, b1), bslice(value, b0, b1)
            mb = bslice(attn_mask, b0, b1)
            for l0 in range(0, L, lq):
                l1 = min(L, l0 + lq)
                m = mb[..., l0:l1, :] if (mb is not None and mb.dim() >= 2
                                          and mb.shape[-2] == L) else mb
                out[b0:b1, ..., l0:l1, :] = original(
                    qb[..., l0:l1, :], kb, vb, attn_mask=m, dropout_p=0.0,
                    is_causal=False, scale=scale, **kwargs)
        return out
    sdpa.warned = False
    sdpa.__wrapped__ = original
    return sdpa


def patch_attention():
    import torch.nn.functional as F
    if not hasattr(F.scaled_dot_product_attention, "__wrapped__"):
        F.scaled_dot_product_attention = _chunked_sdpa(F.scaled_dot_product_attention)


def _cpu_fallback(original, name):
    """MPS adaptive pooling raises a plain RuntimeError (so
    PYTORCH_ENABLE_MPS_FALLBACK doesn't catch it) when the input size isn't
    divisible by the output size, e.g. F.interpolate(mode="area") to 7/32 of
    the frame for MoGe. Run just that op on the CPU and move the result back."""
    def wrapped(input, *args, **kwargs):
        if input.device.type == DEV:
            try:
                return original(input, *args, **kwargs)
            except (RuntimeError, NotImplementedError) as e:
                msg = str(e).lower()
                if "divisible" not in msg and "not implemented" not in msg:
                    raise
                if not wrapped.warned:
                    print(f"[apple-gpu] {name}: size not supported on the GPU, "
                          "doing this resize on the CPU", flush=True)
                    wrapped.warned = True
                out = original(input.cpu(), *args, **kwargs)
                if isinstance(out, tuple):  # max pools may return (values, indices)
                    return tuple(o.to(input.device) for o in out)
                return out.to(input.device)
        return original(input, *args, **kwargs)
    wrapped.warned = False
    wrapped.__wrapped__ = original
    return wrapped


def patch_adaptive_pools():
    import torch.nn.functional as F
    # F.interpolate(mode="area") looks these names up in the functional
    # module at call time, so replacing them there covers interpolate too.
    for name in ("adaptive_avg_pool1d", "adaptive_avg_pool2d", "adaptive_avg_pool3d",
                 "adaptive_max_pool1d", "adaptive_max_pool2d", "adaptive_max_pool3d"):
        original = getattr(F, name, None)
        if original is not None and not hasattr(original, "__wrapped__"):
            setattr(F, name, _cpu_fallback(original, name))


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    engine = Path(sys.argv[1]).expanduser().resolve()
    mode, rest = sys.argv[2], sys.argv[3:]

    ok, msg = _mps_ok()
    chip = platform.processor() or platform.machine()
    print(f"[apple-gpu] torch {torch.__version__} · {msg} · {platform.system()} "
          f"{platform.mac_ver()[0] or platform.release()} · {chip}", flush=True)
    if not ok:
        print(f"[error] {msg}", flush=True)
        return 1
    if DEV == "mps":
        print(f"[apple-gpu] macOS recommends up to {torch.mps.recommended_max_memory() / GiB:.1f} GB "
              f"for the GPU; auto window planning uses {_budget_bytes() / GiB:.1f} GB", flush=True)

    if mode == "probe":
        x = torch.randn(1024, 1024, device=DEV)
        with torch.autocast(DEV, dtype=torch.bfloat16):
            y = (x @ x).float()
        print(f"[apple-gpu] bf16 matmul ok: {bool(torch.isfinite(y).all())}", flush=True)
        return 0

    patch_torch()
    sys.path.insert(0, str(engine))
    os.chdir(engine)
    import corridorkey.runtime as rt  # noqa: E402

    original = rt.load_model
    rt.load_model = lambda paths, device="cuda": original(paths, device=DEV)

    script = {"infer": engine / "infer.py",
              "smoke": engine / "scripts" / "smoke_test_runtime.py"}.get(mode)
    if script is None:
        print(f"[error] unknown mode {mode!r}")
        return 2
    sys.argv = [str(script), *rest]
    runpy.run_path(str(script), run_name="__main__")
    return 0


if __name__ == "__main__":
    sys.exit(main())
