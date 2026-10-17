"""Checks the C++ DSP core against the Python reference (reference/brawl_fx.py).

usage: python3 tests/compare.py path/to/dsp_cli
"""
import os, subprocess, sys, tempfile
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "reference"))
from brawl_fx import process

CLI = sys.argv[1]
SR = 48000
TMP = tempfile.mkdtemp()


def signals(seconds=6, ch=2, seed=0):
    """A: a pad that changes chord every 1.5 s plus a slow wobble. B: kick + noisy hats."""
    rng = np.random.default_rng(seed)
    t = np.arange(int(SR * seconds)) / SR
    f = np.where((t // 1.5) % 2 == 0, 220.0, 293.66)
    wob = 1 + 0.3 * np.sin(2 * np.pi * 0.7 * t)
    a_l = 0.25 * np.sin(2 * np.pi * f * t) * wob + 0.12 * np.sin(2 * np.pi * 1.5 * f * t)
    a_r = 0.25 * np.sin(2 * np.pi * f * t + 0.5) * wob + 0.12 * np.sin(2 * np.pi * 2 * f * t)
    kick = 0.6 * np.exp(-((t % 0.5) * 30)) * np.sin(2 * np.pi * 55 * t)
    hats = 0.12 * rng.standard_normal(t.size) * np.exp(-((t + 0.25) % 0.25) * 60)
    b_l, b_r = kick + hats, kick + 0.7 * hats
    A = np.stack([a_l, a_r][:ch]).astype(np.float32)
    B = np.stack([b_l, b_r][:ch]).astype(np.float32)
    return A, B


DEF = dict(budget_kbs=24, bias=0, sharpness=0.5, hold_ms=0, savings_ms=0, loop_ms=250, chunk=64,
           smooth=0.5, memory=1.0, bits=16, randomness=0, b_trim_db=0, mix=1, out_gain_db=0)


def run_cpp(a, b, host_block=512, **kw):
    p = dict(DEF); p.update(kw)
    seed = p.pop("seed", 1); sweep = p.pop("sweep", 0)
    fa, fb, fo = (os.path.join(TMP, n) for n in ("a.f32", "b.f32", "out.f32"))
    a.astype(np.float32).tofile(fa)
    if b is not None:
        b.astype(np.float32).tofile(fb)
    args = [CLI, fa, fb if b is not None else "-", fo, str(a.shape[0]), str(SR), str(host_block)]
    args += [f"{k}={v}" for k, v in p.items()] + [f"seed={seed}", f"sweep={sweep}"]
    r = subprocess.run(args, capture_output=True, text=True, check=True)
    y = np.fromfile(fo, dtype=np.float32).reshape(a.shape[0], -1)
    info = dict(kv.split("=") for kv in r.stderr.split())
    lat = int(info["latency"])
    y = np.concatenate([y[:, lat:], np.zeros((a.shape[0], lat), np.float32)], axis=1)  # undo latency
    return y, info


def run_py(a, b, **kw):
    p = dict(DEF); p.update(kw)
    return process(a, b if b is not None else np.zeros_like(a), SR, p, seed=1).astype(np.float32)


rms = lambda x: float(np.sqrt(np.mean(np.square(x))))
fails = 0


def check(name, ok, detail=""):
    global fails
    fails += 0 if ok else 1
    print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")


A, B = signals()
CUT = 1000   # ignore the very end, which the latency shift pads with zeros

# 1) a silent / missing sidechain leaves A untouched at full budget
quiet = (A * 0.5).astype(np.float32)   # stays below the soft limiter's knee
for kw in [dict(), dict(chunk=1, smooth=0), dict(chunk=1024), dict(chunk=300, smooth=1, memory=0.0)]:
    for side in (None, np.zeros_like(A)):
        y, _ = run_cpp(quiet, side, budget_kbs=100000, **kw)
        err = np.abs(y - quiet)[:, :-CUT].max()
        check(f"transparent with {'no' if side is None else 'silent'} sidechain {kw}", err < 1e-3, f"max err {err:.2e}")

# 2) deterministic case: full budget, greedy, no random tie-breaks -> matches Python closely
for kw in [dict(bits=8), dict(bits=12, chunk=1, smooth=0), dict(bits=16, bias=0.6, sharpness=1, hold_ms=40)]:
    y_c, _ = run_cpp(A, B, budget_kbs=100000, **kw)
    y_p = run_py(A, B, budget_kbs=100000, **kw)
    dd = np.abs(y_c - y_p)[:, :-CUT]
    share = float(np.mean(dd > 1e-3))
    check(f"matches Python at full budget {kw}", share < 0.01, f"max diff {dd.max():.2e}, {share*100:.3f}% of samples differ")

# 3) degraded settings: same character as Python (error vs each input within 15%)
configs = [dict(), dict(budget_kbs=10), dict(budget_kbs=40, sharpness=1), dict(sharpness=0, bias=-0.5),
           dict(sharpness=1, hold_ms=120), dict(savings_ms=500, budget_kbs=60), dict(randomness=1),
           dict(chunk=1, smooth=0, budget_kbs=30), dict(chunk=1000, loop_ms=400), dict(memory=0.3, loop_ms=300),
           dict(bits=8, b_trim_db=6), dict(loop_ms=900, chunk=16, budget_kbs=50)]
for kw in configs:
    y_c, _ = run_cpp(A, B, **kw); y_p = run_py(A, B, **kw)
    ok, det = True, []
    for name, ref in (("A", A), ("B", B)):
        ec, ep = rms((y_c - ref)[:, :-CUT]), rms((y_p - ref)[:, :-CUT])
        ok &= abs(ec - ep) / max(ep, 1e-9) < 0.15
        det.append(f"vs {name}: C++ {ec:.4f} / Py {ep:.4f}")
    check(f"same character as Python {kw}", ok, ", ".join(det))

# 4) result does not depend on the host buffer size
base, _ = run_cpp(A, B, host_block=64, budget_kbs=15, chunk=500, randomness=0.6, memory=0.5, sharpness=0.8, hold_ms=50)
for hb in (1000, 0):
    y, _ = run_cpp(A, B, host_block=hb, budget_kbs=15, chunk=500, randomness=0.6, memory=0.5, sharpness=0.8, hold_ms=50)
    check(f"independent of host buffer size ({hb or 'random'} vs 64)", np.array_equal(y, base))

# 5) automating everything at once stays finite and below 0 dBFS
A12, B12 = signals(12, seed=3)
y, _ = run_cpp(A12, B12, host_block=0, sweep=1)
# (+24 dB of sidechain trim drives the soft limiter all the way to full scale, but never past it)
check("full automation sweep is finite, no overs", np.isfinite(y).all() and np.abs(y).max() <= 1.0, f"peak {np.abs(y).max():.4f}")

# 6) mono
Am, Bm = signals(ch=1)
y, _ = run_cpp(Am, Bm)
check("mono", y.shape[0] == 1 and np.isfinite(y).all())

# 7) the fight is visible: Bias moves territory the right way
_, ia = run_cpp(A, B, bias=-0.8, sharpness=1)
_, ib = run_cpp(A, B, bias=0.8, sharpness=1)
check("bias moves territory (A-favoured holds more A)", float(ia["holdA"]) > float(ib["holdA"]) and float(ib["holdB"]) > float(ia["holdB"]),
      f"A-bias holds A {float(ia['holdA']):.2f} B {float(ia['holdB']):.2f}; B-bias holds A {float(ib['holdA']):.2f} B {float(ib['holdB']):.2f}")

# 8) speed in the heaviest settings: 4 s loop, single-sample chunks, half random, stereo
A10, B10 = signals(10, seed=5)
_, info = run_cpp(A10, B10, loop_ms=4000, chunk=1, smooth=0, randomness=0.5, budget_kbs=200)
rtf = float(info["realtime_factor"])
y_c, _ = run_cpp(A, B, loop_ms=4000, chunk=1, smooth=0, budget_kbs=60)
y_p = run_py(A, B, loop_ms=4000, chunk=1, smooth=0, budget_kbs=60)
ec, ep = rms((y_c - A)[:, :-CUT]), rms((y_p - A)[:, :-CUT])
check("long loop with sampled candidates keeps Python's character", abs(ec - ep) / ep < 0.15, f"err vs A: C++ {ec:.4f} / Py {ep:.4f}")
check("fast enough at heaviest settings (>20x real time)", rtf > 20, f"{rtf:.0f}x real time")
_, info = run_cpp(A10, B10)
print(f"info  default settings run at {float(info['realtime_factor']):.0f}x real time")

print("\nall passed" if fails == 0 else f"\n{fails} failed")
sys.exit(1 if fails else 0)
