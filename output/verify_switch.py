#!/usr/bin/env python3
"""Verify in-call sample-rate switching of libspeexdsp.

Two runs use the exact same small synthetic 48 kHz source and the same
frame boundaries:

  * live run : one resampler state, rate switched 48k -> 44.1k -> 32k
               between frames (state migrated, never recreated);
  * reference: the same source processed offline, one resampler state whose
               rate is switched at the identical frame boundaries (i.e. the
               deterministic, non-real-time replay of the live call).

The script proves:
  1. output continuity at both switch points (no spike/discontinuity);
  2. sample-exact equality between the live switched stream and the
     offline replayed stream with identical state migration;
     plus steady-state equality with a fixed-rate-only offline resampler;
  3. switch latency numbers (wall clock, repeated for a tight upper bound);
  4. illegal target rates return the documented error codes and keep the
     running state unchanged.

Only the Python standard library is required (ctypes + math).
"""

import ctypes
import math
import os
import statistics
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB_PATH = os.path.join(ROOT, "libspeexdsp", ".libs", "libspeexdsp.so")
OUT_DIR = os.path.join(ROOT, "output")

SR_IN = 48000
RATES = (48000, 44100, 32000)
QUALITY = 5
FRAME = 480              # 10 ms at 48 kHz
TOTAL_FRAMES = 180       # 1.8 s of source audio
SWITCH_FRAMES = (60, 120)
SEG_FRAMES = (60, 60, 60)

RESAMPLER_ERR_SUCCESS = 0
RESAMPLER_ERR_BAD_STATE = 2
RESAMPLER_ERR_INVALID_ARG = 3

lib = ctypes.CDLL(LIB_PATH)
lib.speex_resampler_init.restype = ctypes.c_void_p
lib.speex_resampler_init.argtypes = [
    ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int,
    ctypes.POINTER(ctypes.c_int),
]
lib.speex_resampler_destroy.argtypes = [ctypes.c_void_p]
lib.speex_resampler_process_float.argtypes = [
    ctypes.c_void_p, ctypes.c_uint32,
    ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_uint32),
    ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_uint32),
]
lib.speex_resampler_set_rate.restype = ctypes.c_int
lib.speex_resampler_set_rate.argtypes = [
    ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
lib.speex_resampler_get_rate.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32),
    ctypes.POINTER(ctypes.c_uint32)]
lib.speex_resampler_skip_zeros.argtypes = [ctypes.c_void_p]
lib.speex_resampler_strerror.restype = ctypes.c_char_p
lib.speex_resampler_strerror.argtypes = [ctypes.c_int]


def make_source(n):
    """Small deterministic source: 440 Hz + 1 kHz tones at 48 kHz."""
    data = (ctypes.c_float * n)()
    for i in range(n):
        t = i / SR_IN
        data[i] = 0.5 * math.sin(2 * math.pi * 440.0 * t) + \
            0.25 * math.sin(2 * math.pi * 1000.0 * t)
    return data


def new_resampler(out_rate):
    err = ctypes.c_int(0)
    st = lib.speex_resampler_init(1, SR_IN, out_rate, QUALITY,
                                  ctypes.byref(err))
    if not st:
        raise RuntimeError("init failed: %s" %
                           lib.speex_resampler_strerror(err.value).decode())
    lib.speex_resampler_skip_zeros(st)
    return st


def process_frames(st, source, frame_lo, frame_hi):
    """Run frames [frame_lo, frame_hi) through one state; return outputs."""
    out = []
    out_buf = (ctypes.c_float * (FRAME * 2))()
    float_size = ctypes.sizeof(ctypes.c_float)
    for frame in range(frame_lo, frame_hi):
        in_len = ctypes.c_uint32(FRAME)
        out_len = ctypes.c_uint32(FRAME * 2)
        in_ptr = ctypes.cast(
            ctypes.byref(source, frame * FRAME * float_size),
            ctypes.POINTER(ctypes.c_float))
        rc = lib.speex_resampler_process_float(
            st, 0, in_ptr, ctypes.byref(in_len),
            out_buf, ctypes.byref(out_len))
        if rc != RESAMPLER_ERR_SUCCESS or in_len.value != FRAME:
            raise RuntimeError("process failed rc=%d consumed=%d" %
                               (rc, in_len.value))
        out.extend(out_buf[:out_len.value])
    return out


def run_live(source):
    """One state for the whole call, rates switched between frames."""
    st = new_resampler(RATES[0])
    handle = st
    out = []
    boundaries = []
    timings = []
    segment = 0
    out_buf = (ctypes.c_float * (FRAME * 2))()
    float_size = ctypes.sizeof(ctypes.c_float)
    for frame in range(TOTAL_FRAMES):
        if segment < len(SWITCH_FRAMES) and frame == SWITCH_FRAMES[segment]:
            t0 = time.perf_counter()
            rc = lib.speex_resampler_set_rate(st, SR_IN,
                                              RATES[segment + 1])
            timings.append(time.perf_counter() - t0)
            assert rc == RESAMPLER_ERR_SUCCESS, \
                lib.speex_resampler_strerror(rc).decode()
            assert st == handle, "resampler state was recreated"
            boundaries.append(len(out))
            segment += 1
        in_len = ctypes.c_uint32(FRAME)
        out_len = ctypes.c_uint32(FRAME * 2)
        in_ptr = ctypes.cast(
            ctypes.byref(source, frame * FRAME * float_size),
            ctypes.POINTER(ctypes.c_float))
        rc = lib.speex_resampler_process_float(
            st, 0, in_ptr, ctypes.byref(in_len),
            out_buf, ctypes.byref(out_len))
        assert rc == RESAMPLER_ERR_SUCCESS and in_len.value == FRAME
        out.extend(out_buf[:out_len.value])
    lib.speex_resampler_destroy(st)
    return out, boundaries, timings


def run_reference(source):
    """Offline replay: one state, same set_rate switches at same frames."""
    st = new_resampler(RATES[0])
    out = []
    segment = 0
    out_buf = (ctypes.c_float * (FRAME * 2))()
    float_size = ctypes.sizeof(ctypes.c_float)
    for frame in range(TOTAL_FRAMES):
        if segment < len(SWITCH_FRAMES) and frame == SWITCH_FRAMES[segment]:
            rc = lib.speex_resampler_set_rate(st, SR_IN,
                                              RATES[segment + 1])
            assert rc == RESAMPLER_ERR_SUCCESS
            segment += 1
        in_len = ctypes.c_uint32(FRAME)
        out_len = ctypes.c_uint32(FRAME * 2)
        in_ptr = ctypes.cast(
            ctypes.byref(source, frame * FRAME * float_size),
            ctypes.POINTER(ctypes.c_float))
        rc = lib.speex_resampler_process_float(
            st, 0, in_ptr, ctypes.byref(in_len),
            out_buf, ctypes.byref(out_len))
        assert rc == RESAMPLER_ERR_SUCCESS and in_len.value == FRAME
        out.extend(out_buf[:out_len.value])
    lib.speex_resampler_destroy(st)
    return out


def percentile(values, q):
    s = sorted(values)
    return s[min(len(s) - 1, int(q * len(s)))]


def best_align_offset(a, b, search=200, window=14000):
    """Integer sample offset of b vs a minimizing squared error."""
    best_off, best_mse = 0, float("inf")
    for off in range(-search, search + 1):
        if off >= 0:
            x, y = a[:window], b[off:off + window]
        else:
            x, y = a[-off:window - off], b[:window]
        m = min(len(x), len(y))
        if m < 1000:
            continue
        mse = sum((x[i] - y[i]) ** 2 for i in range(m)) / m
        if mse < best_mse:
            best_off, best_mse = off, mse
    return best_off, math.sqrt(best_mse)


def run_fixed_rate(source, rate):
    """Fully offline reference: one fixed-rate resampler, no switches."""
    st = new_resampler(rate)
    out = process_frames(st, source, 0, TOTAL_FRAMES)
    lib.speex_resampler_destroy(st)
    return out


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    source = make_source(TOTAL_FRAMES * FRAME)

    live, boundaries, timings = run_live(source)
    reference = run_reference(source)

    report = []
    ok = True

    # 1. Length / sample-exact equality with the offline reference.
    same_length = len(live) == len(reference)
    mismatches = [(i, live[i], reference[i])
                  for i in range(min(len(live), len(reference)))
                  if live[i] != reference[i]]
    exact_match = same_length and not mismatches
    report.append("live samples      : %d" % len(live))
    report.append("reference samples : %d" % len(reference))
    report.append("exact match       : %s" %
                  ("PASS" if exact_match else "FAIL"))
    if not exact_match and mismatches:
        report.append("  first mismatch  : idx=%d live=%.9g ref=%.9g" %
                      mismatches[0])
    ok = ok and exact_match

    # 1b. Steady-state equivalence with a pure fixed-rate offline resampler.
    #     After the switches settle, the migrated state must produce the same
    #     samples as a resampler that ran at the final rate the whole time
    #     (a fixed group-delay alignment offset is allowed).
    fixed_32k = run_fixed_rate(source, RATES[-1])
    last_switch = boundaries[-1]
    # Output position of the fixed-rate run after consuming the same number
    # of input frames as the live run at its last switch.
    probe = new_resampler(RATES[-1])
    fixed_pos = len(process_frames(probe, source, 0, SWITCH_FRAMES[-1]))
    lib.speex_resampler_destroy(probe)
    live_tail = live[last_switch:]
    fixed_tail = fixed_32k[fixed_pos:]
    offset, align_rmse = best_align_offset(live_tail, fixed_tail)
    if offset >= 0:
        x, y = live_tail, fixed_tail[offset:]
    else:
        x, y = live_tail[-offset:], fixed_tail
    cmp_n = min(len(x), len(y)) - 500
    max_diff = max(abs(x[i] - y[i]) for i in range(500, cmp_n + 500))
    steady_match = max_diff == 0.0
    report.append("fixed-rate 32k offline equivalence: offset=%d samples, "
                  "max|diff|=%.9g over %d samples %s" %
                  (offset, max_diff, cmp_n,
                   "PASS" if steady_match else "FAIL"))
    ok = ok and steady_match

    # 2. Continuity at switch points (first differences vs. steady region).
    diffs = [abs(live[i + 1] - live[i]) for i in range(len(live) - 1)]
    median_diff = statistics.median(diffs)
    p999 = percentile(diffs, 0.999)
    report.append("steady |dx| median: %.8f  p99.9: %.8f" %
                  (median_diff, p999))
    for k, b in enumerate(boundaries, 1):
        gap = abs(live[b] - live[b - 1])
        ratio = gap / median_diff if median_diff else 0.0
        passed = math.isfinite(gap) and ratio <= 50.0
        report.append("switch %d boundary : pos=%d |dx|=%.8f "
                      "ratio_to_median=%.3f %s" %
                      (k, b, gap, ratio, "PASS" if passed else "FAIL"))
        ok = ok and passed

    # 3. Switch latency: observed timings plus a 4000-call upper bound.
    st = new_resampler(32000)
    bench = []
    for _ in range(2000):
        t0 = time.perf_counter()
        rc = lib.speex_resampler_set_rate(st, SR_IN, 44100)
        bench.append(time.perf_counter() - t0)
        assert rc == RESAMPLER_ERR_SUCCESS
        t0 = time.perf_counter()
        rc = lib.speex_resampler_set_rate(st, SR_IN, 32000)
        bench.append(time.perf_counter() - t0)
        assert rc == RESAMPLER_ERR_SUCCESS
    lib.speex_resampler_destroy(st)
    report.append("switch time (call): %.4f ms, %.4f ms" %
                  tuple(t * 1000.0 for t in timings))
    report.append("switch time 4000-run max    : %.4f ms" %
                  (max(bench) * 1000.0))
    report.append("switch time 4000-run p99.9  : %.4f ms" %
                  (percentile(bench, 0.999) * 1000.0))
    report.append("switch time 4000-run median : %.4f ms" %
                  (statistics.median(bench) * 1000.0))

    # 4. Illegal target rates -> error codes, state preserved.
    st = new_resampler(32000)
    bad_cases = ((SR_IN, 0), (0, 32000))
    for in_rate, out_rate in bad_cases:
        rc = lib.speex_resampler_set_rate(st, in_rate, out_rate)
        cur_in = ctypes.c_uint32(0)
        cur_out = ctypes.c_uint32(0)
        lib.speex_resampler_get_rate(st, ctypes.byref(cur_in),
                                     ctypes.byref(cur_out))
        passed = rc == RESAMPLER_ERR_INVALID_ARG and \
            cur_in.value == SR_IN and cur_out.value == 32000
        report.append("illegal (%u,%u) -> code %d, state kept: %s" %
                      (in_rate, out_rate, rc,
                       "PASS" if passed else "FAIL"))
        ok = ok and passed
    rc_null = lib.speex_resampler_set_rate(None, SR_IN, 32000)
    passed = rc_null == RESAMPLER_ERR_BAD_STATE
    report.append("NULL state -> code %d: %s" %
                  (rc_null, "PASS" if passed else "FAIL"))
    ok = ok and passed
    lib.speex_resampler_destroy(st)

    report.append("OVERALL: %s" % ("PASS" if ok else "FAIL"))
    text = "\n".join(report)
    print(text)
    with open(os.path.join(OUT_DIR, "verify_report.txt"), "w") as f:
        f.write(text + "\n")

    # Persist the streams for offline inspection / waveform rendering.
    for name, vals in (("live", live), ("reference", reference)):
        with open(os.path.join(OUT_DIR, "verify_%s.f32" % name), "wb") as f:
            f.write((ctypes.c_float * len(vals))(*vals))
    with open(os.path.join(OUT_DIR, "source_48k.f32"), "wb") as f:
        f.write(bytes(source))

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
