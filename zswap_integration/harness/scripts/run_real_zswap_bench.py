#!/usr/bin/env python3
# Copyright Advanced Micro Devices, Inc.
# SPDX-License-Identifier: BSD-3-Clause
"""
run_real_zswap_bench.py - real-kernel zswap benchmark runner.

Compares Linux zswap with compressor= lz4 vs aocl-lz4. This script must run
as root because it loads an out-of-tree crypto module, changes zswap
parameters, creates a temporary swapfile, and places a workload in a cgroup.

Phase protocol with the workload binary:
  1. Runner starts workload (unconstrained, no cgroup yet)
  2. Workload loads corpus, allocates+fills all pages, signals FILL_DONE
  3. Runner moves workload into memory-limited cgroup, signals PAGEOUT_GO
  4. Workload calls MADV_PAGEOUT, signals PAGEOUT_DONE
  5. Runner samples zswap counters, checks residency, signals SCAN_GO
  6. Workload scans all pages (swapin/decompress), signals SCAN_DONE
  7. Runner samples post-scan counters, signals EXIT_GO
  8. Workload prints JSON, exits

Artifacts (under harness/):
  results/real_zswap/raw.jsonl
  results/real_zswap/summary.csv
  results/real_zswap/env.json
"""
import argparse
import csv
import json
import os
import statistics as st
import subprocess
import sys
import time
from pathlib import Path

# HARNESS = the harness/ dir (bench, build, results, public_corpus, ...).
# INTEG   = the integration root one level up (holds the product: module/).
HARNESS = Path(__file__).resolve().parents[1]
INTEG = HARNESS.parent
MODULE_DIR = INTEG / "module"
MODULE_KO = MODULE_DIR / "aocl_lz4.ko"
WORKLOAD = HARNESS / "build" / "zswap_kernel_workload"
CORPUS = HARNESS / "public_corpus" / "silesia"
RESULT_DIR = HARNESS / "results" / "real_zswap"
SWAPFILE = HARNESS / "results" / "real_zswap.swap"
CGROUP_ROOT = Path("/sys/fs/cgroup")
ZSWAP_PARAMS = Path("/sys/module/zswap/parameters")
ZSWAP_DEBUG = Path("/sys/kernel/debug/zswap")

def run(cmd, check=True, capture=True, **kwargs):
    res = subprocess.run(cmd, text=True, capture_output=capture, **kwargs)
    if check and res.returncode != 0:
        raise RuntimeError(
            f"command failed ({res.returncode}): {' '.join(map(str, cmd))}\n"
            f"stdout:\n{res.stdout}\nstderr:\n{res.stderr}"
        )
    return res


def write_text(path: Path, value: str):
    path.write_text(value)


def read_text(path: Path, default=None):
    try:
        return path.read_text().strip()
    except FileNotFoundError:
        return default
    except PermissionError:
        return default


def read_kv_files(path: Path):
    out = {}
    if not path.exists():
        return out
    for p in sorted(path.iterdir()):
        if p.is_file():
            v = read_text(p)
            if v is not None:
                out[p.name] = v
    return out


def read_vmstat():
    keys = {"pswpin", "pswpout", "pgmajfault", "pgfault"}
    out = {}
    with open("/proc/vmstat") as fh:
        for line in fh:
            k, v = line.split()
            if k in keys:
                out[k] = int(v)
    return out


def read_swaps():
    return Path("/proc/swaps").read_text()


def dmesg_marker():
    res = run(["dmesg", "--level=err,warn"], check=False)
    return res.stdout


def ensure_root():
    if os.geteuid() != 0:
        print("ERROR: run this script as root after validating sudo, e.g.:", file=sys.stderr)
        print(f"  sudo {sys.executable} {Path(__file__).resolve()} --rounds 5", file=sys.stderr)
        sys.exit(2)


def build_workload():
    WORKLOAD.parent.mkdir(parents=True, exist_ok=True)
    run([
        "gcc", "-O2", "-std=gnu11", "-Wall", "-Wextra",
        str(HARNESS / "bench" / "zswap_kernel_workload.c"),
        "-o", str(WORKLOAD),
    ])


def build_module(decode_backend="scalar"):
    """Build aocl_lz4.ko with the chosen decode back-end.

    decode_backend:
      "avx"    -> AVX2 decode wrapped in kernel_fpu_begin/end (per-page FPU tax).
      "scalar" -> kernel-safe scalar decode, NO SIMD, NO kernel_fpu (zero tax).
    """
    run(["make", "clean"], cwd=MODULE_DIR)
    make_args = ["make"]
    if decode_backend == "avx":
        make_args.append("AOCL_LZ4_AVX=1")
    print(f"[build] {' '.join(make_args)}", flush=True)
    run(make_args, cwd=MODULE_DIR)


def load_module():
    run(["rmmod", "aocl_lz4"], check=False)
    run(["insmod", str(MODULE_KO)])
    crypto = Path("/proc/crypto").read_text(errors="replace")
    if "name         : aocl-lz4" not in crypto and "name         : aocl-lz4\n" not in crypto:
        raise RuntimeError("aocl-lz4 did not appear in /proc/crypto after insmod")


def ensure_debugfs():
    if ZSWAP_DEBUG.exists():
        return
    run(["mount", "-t", "debugfs", "debugfs", "/sys/kernel/debug"], check=False)


def save_zswap_params():
    params = {}
    if ZSWAP_PARAMS.exists():
        for name in ["enabled", "compressor", "zpool", "max_pool_percent",
                     "accept_threshold_percent", "shrinker_enabled"]:
            v = read_text(ZSWAP_PARAMS / name)
            if v is not None:
                params[name] = v
    return params


def restore_zswap_params(params):
    try:
        if "enabled" in params:
            write_text(ZSWAP_PARAMS / "enabled", "0")
        for name in ["zpool", "max_pool_percent", "accept_threshold_percent",
                     "shrinker_enabled", "compressor"]:
            if name in params and (ZSWAP_PARAMS / name).exists():
                write_text(ZSWAP_PARAMS / name, params[name])
        if "enabled" in params:
            write_text(ZSWAP_PARAMS / "enabled", params["enabled"])
    except Exception as exc:
        print(f"WARN: failed to fully restore zswap params: {exc}", file=sys.stderr)


def setup_swapfile(size_mib):
    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    run(["swapoff", str(SWAPFILE)], check=False)
    if SWAPFILE.exists():
        SWAPFILE.unlink()
    run(["fallocate", "-l", f"{size_mib}M", str(SWAPFILE)])
    os.chmod(SWAPFILE, 0o600)
    run(["mkswap", str(SWAPFILE)])


def swapon_tmp():
    run(["swapon", "-p", "32767", str(SWAPFILE)])


def swapoff_tmp():
    run(["swapoff", str(SWAPFILE)], check=False)


def set_zswap_compressor(name):
    if not ZSWAP_PARAMS.exists():
        raise RuntimeError("zswap parameters not present; is zswap built/loaded?")
    swapoff_tmp()
    write_text(ZSWAP_PARAMS / "enabled", "0")
    write_text(ZSWAP_PARAMS / "compressor", name)
    write_text(ZSWAP_PARAMS / "enabled", "1")
    actual = read_text(ZSWAP_PARAMS / "compressor")
    if actual != name:
        raise RuntimeError(f"zswap compressor is {actual!r}, expected {name!r}")


def configure_zswap(args):
    """Retention-friendly settings applied identically to both compressors."""
    if not ZSWAP_PARAMS.exists():
        raise RuntimeError("zswap parameters not present; is zswap built/loaded?")
    write_text(ZSWAP_PARAMS / "enabled", "0")
    if args.zpool and (ZSWAP_PARAMS / "zpool").exists():
        write_text(ZSWAP_PARAMS / "zpool", args.zpool)
    if (ZSWAP_PARAMS / "max_pool_percent").exists():
        write_text(ZSWAP_PARAMS / "max_pool_percent", str(args.max_pool_percent))
    if (ZSWAP_PARAMS / "accept_threshold_percent").exists():
        write_text(ZSWAP_PARAMS / "accept_threshold_percent",
                   str(args.accept_threshold_percent))
    if args.disable_shrinker and (ZSWAP_PARAMS / "shrinker_enabled").exists():
        write_text(ZSWAP_PARAMS / "shrinker_enabled", "N")


def cgroup_create(name, memory_max_mib, swap_max_mib):
    cg = CGROUP_ROOT / name
    if cg.exists():
        # A stale cgroup can only be removed once it has no member tasks; if
        # rmdir fails the directory lingers and the mkdir below would throw a
        # confusing FileExistsError, so surface a clear, actionable message.
        run(["rmdir", str(cg)], check=False)
        if cg.exists():
            raise RuntimeError(
                f"stale cgroup {cg} could not be removed (still has tasks?); "
                f"move its members out or 'rmdir' it manually, then retry")
    cg.mkdir()
    write_text(cg / "memory.max", f"{memory_max_mib * 1024 * 1024}\n")
    write_text(cg / "memory.swap.max", f"{swap_max_mib * 1024 * 1024}\n")
    return cg


def cgroup_destroy(cg):
    try:
        run(["rmdir", str(cg)], check=False)
    except Exception:
        pass


def cleanup_stale_bench_state():
    """Best-effort cleanup from a previous interrupted root run."""
    run(["pkill", "-f", str(WORKLOAD)], check=False)
    for cg in CGROUP_ROOT.glob("aocl_lz4-bench-*"):
        cgroup_destroy(cg)
    if RESULT_DIR.exists():
        for p in RESULT_DIR.iterdir():
            if any(token in p.name for token in [
                "fill_done.", "pageout_go.", "pageout_done.",
                "scan_go.", "scan_done.", "exit_go.",
            ]):
                try:
                    p.unlink()
                except FileNotFoundError:
                    pass


def start_workload(mib, compressor, round_idx):
    """Start the workload process WITHOUT putting it in any cgroup.

    The workload loads corpus, allocates and fills all pages, then signals
    fill_done. The caller should wait for fill_done, THEN move the process
    into the memory-limited cgroup, THEN signal pageout_go.
    """
    pid = os.getpid()
    fill_done = RESULT_DIR / f"fill_done.{compressor}.{round_idx}.{pid}"
    pageout_go = RESULT_DIR / f"pageout_go.{compressor}.{round_idx}.{pid}"
    pageout_done = RESULT_DIR / f"pageout_done.{compressor}.{round_idx}.{pid}"
    scan_go = RESULT_DIR / f"scan_go.{compressor}.{round_idx}.{pid}"
    scan_done = RESULT_DIR / f"scan_done.{compressor}.{round_idx}.{pid}"
    exit_go = RESULT_DIR / f"exit_go.{compressor}.{round_idx}.{pid}"
    all_files = [fill_done, pageout_go, pageout_done, scan_go, scan_done, exit_go]
    for p in all_files:
        if p.exists():
            p.unlink()

    env = os.environ.copy()
    env["ZSWAP_BENCH_FILL_DONE_FILE"] = str(fill_done)
    env["ZSWAP_BENCH_PAGEOUT_GO_FILE"] = str(pageout_go)
    env["ZSWAP_BENCH_PAGEOUT_DONE_FILE"] = str(pageout_done)
    env["ZSWAP_BENCH_SCAN_GO_FILE"] = str(scan_go)
    env["ZSWAP_BENCH_SCAN_DONE_FILE"] = str(scan_done)
    env["ZSWAP_BENCH_EXIT_GO_FILE"] = str(exit_go)
    cmd = [str(WORKLOAD), "--corpus-dir", str(CORPUS), "--mib", str(mib)]
    proc = subprocess.Popen(cmd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env)

    return proc, {
        "fill_done": fill_done,
        "pageout_go": pageout_go,
        "pageout_done": pageout_done,
        "scan_go": scan_go,
        "scan_done": scan_done,
        "exit_go": exit_go,
        "all": all_files,
    }


def wait_for_phase(proc, path: Path, timeout_s: float, phase: str):
    deadline = time.time() + timeout_s
    while time.time() < deadline and not path.exists() and proc.poll() is None:
        time.sleep(0.05)
    if proc.poll() is not None:
        out, err = proc.communicate()
        raise RuntimeError(
            f"workload exited (rc={proc.returncode}) before {phase}\n"
            f"stdout={out}\nstderr={err}"
        )
    if not path.exists():
        raise RuntimeError(f"timeout waiting for workload phase: {phase}")


def release_and_collect_workload(proc, phase_files):
    write_text(phase_files["exit_go"], "go\n")
    out, err = proc.communicate(timeout=30)
    for p in phase_files["all"]:
        if p.exists():
            p.unlink()
    if proc.returncode != 0:
        raise RuntimeError(f"workload failed rc={proc.returncode}\nstdout={out}\nstderr={err}")
    return json.loads(out.strip())


def terminate_workload(proc, phase_files=None):
    if proc and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    if phase_files:
        for p in phase_files.get("all", []):
            try:
                p.unlink()
            except FileNotFoundError:
                pass


def capture_state():
    return {
        "zswap": read_kv_files(ZSWAP_DEBUG),
        "vmstat": read_vmstat(),
        "swaps": read_swaps(),
        "params": read_kv_files(ZSWAP_PARAMS),
    }


def delta(after, before):
    out = {}
    for k, v in after.items():
        try:
            out[k] = int(v) - int(before.get(k, 0))
        except (TypeError, ValueError):
            pass
    return out


def summarize(rows):
    by_comp = {}
    for row in rows:
        # Rows are tagged with "decode_backend" (scalar/avx); group on that so
        # scalar and avx results never get folded together.
        key = (row.get("decode_backend", "scalar"), row["compressor"])
        by_comp.setdefault(key, []).append(row)
    summary = []
    for (backend, comp), items in sorted(by_comp.items()):
        summary.append({
            "decode_backend": backend,
            "compressor": comp,
            "n": len(items),
            "scan_s_median": st.median([x["workload"]["scan_s"] for x in items]),
            "pageout_s_median": st.median([x["workload"]["pageout_s"] for x in items]),
            "stored_pages_after_pageout_median": st.median([
                x["zswap_after_pageout_delta"].get("stored_pages", 0) for x in items
            ]),
            "written_back_after_pageout_median": st.median([
                x["zswap_after_pageout_delta"].get("written_back_pages", 0) for x in items
            ]),
            "pool_total_size_after_pageout_median": st.median([
                x["zswap_after_pageout_delta"].get("pool_total_size", 0) for x in items
            ]),
        })
    backends = sorted({backend for backend, _ in by_comp})
    for backend in backends:
        baseline = by_comp.get((backend, "lz4"), [])
        candidate = by_comp.get((backend, "aocl-lz4"), [])
        if baseline and candidate:
            n = min(len(baseline), len(candidate))
            paired = []
            for i in range(n):
                b = baseline[i]["workload"]["scan_s"]
                c = candidate[i]["workload"]["scan_s"]
                paired.append((b / c - 1.0) * 100.0)
            summary.append({
                "decode_backend": backend,
                "compressor": "paired_lz4_aocl_vs_lz4",
                "n": n,
                "scan_speedup_percent_median": st.median(paired),
                "scan_speedup_percent_min": min(paired),
                "scan_speedup_percent_max": max(paired),
            })
    return summary


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--mib", type=int, default=1536)
    ap.add_argument("--memory-max-mib", type=int, default=512)
    ap.add_argument("--swap-max-mib", type=int, default=4096)
    ap.add_argument("--swapfile-mib", type=int, default=4096)
    ap.add_argument("--min-stored-pages", type=int, default=100000)
    ap.add_argument("--phase-timeout-s", type=float, default=300.0)
    ap.add_argument("--zpool", default="zsmalloc")
    ap.add_argument("--max-pool-percent", type=int, default=80)
    ap.add_argument("--accept-threshold-percent", type=int, default=100)
    # Shrinker is disabled by default for reproducible pool residency; make it
    # actually toggleable via --no-disable-shrinker instead of being a no-op.
    ap.add_argument("--disable-shrinker", action=argparse.BooleanOptionalAction,
                    default=True)
    ap.add_argument("--decode-backend", choices=["avx", "scalar"], default="scalar",
                    help="aocl-lz4 decode back-end: 'avx' (kernel_fpu-taxed) or "
                         "'scalar' (kernel-safe, zero FPU tax; recommended)")
    ap.add_argument("--skip-build", action="store_true")
    args = ap.parse_args()

    ensure_root()
    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    if not CORPUS.exists():
        raise RuntimeError(f"missing corpus dir: {CORPUS}")

    print(f"[cfg] decode_backend={args.decode_backend}", flush=True)
    if not args.skip_build:
        build_workload()
    cleanup_stale_bench_state()
    ensure_debugfs()
    original_params = save_zswap_params()
    configure_zswap(args)
    setup_swapfile(args.swapfile_mib)

    env_info = {
        "uname": run(["uname", "-a"]).stdout.strip(),
        "decode_backend": args.decode_backend,
        "original_zswap_params": original_params,
        "args": vars(args),
    }
    (RESULT_DIR / "env.json").write_text(json.dumps(env_info, indent=2))

    rows = []
    raw_path = RESULT_DIR / "raw.jsonl"
    raw_fh = raw_path.open("w")
    cg = None
    proc = None
    phase_files = None
    try:
        print(f"[build] aocl_lz4.ko ({args.decode_backend} decode)", flush=True)
        if not args.skip_build:
            build_module(decode_backend=args.decode_backend)
        load_module()
        configure_zswap(args)
        for round_idx in range(args.rounds):
            for compressor in ["lz4", "aocl-lz4"]:
                print(f"[run] round={round_idx} compressor={compressor}", flush=True)
                set_zswap_compressor(compressor)
                swapon_tmp()
                cg = cgroup_create(f"aocl_lz4-bench-{os.getpid()}",
                                   args.memory_max_mib, args.swap_max_mib)
                before = capture_state()
                dmesg_before = dmesg_marker()
                t0 = time.time()

                # Start workload outside cgroup so it fills memory unconstrained
                proc, phase_files = start_workload(args.mib, compressor, round_idx)

                # Wait for workload to finish loading corpus and filling pages
                print(f"  waiting for fill_done (up to {args.phase_timeout_s}s)...", flush=True)
                wait_for_phase(proc, phase_files["fill_done"],
                               args.phase_timeout_s, "fill_done")
                print(f"  fill_done received, moving pid {proc.pid} into cgroup", flush=True)

                # NOW move it into the memory-limited cgroup
                write_text(cg / "cgroup.procs", f"{proc.pid}\n")
                time.sleep(0.1)

                # Signal workload to call MADV_PAGEOUT (now cgroup-constrained)
                write_text(phase_files["pageout_go"], "go\n")

                # Wait for pageout phase to complete
                wait_for_phase(proc, phase_files["pageout_done"],
                               args.phase_timeout_s, "pageout_done")
                time.sleep(0.3)

                after_pageout = capture_state()
                pageout_delta = delta(after_pageout["zswap"], before["zswap"])
                stored = pageout_delta.get("stored_pages", 0)
                print(f"  stored_pages={stored} (min={args.min_stored_pages})", flush=True)
                if stored < args.min_stored_pages:
                    write_text(phase_files["scan_go"], "go\n")
                    write_text(phase_files["exit_go"], "go\n")
                    out, err = proc.communicate(timeout=30)
                    raise RuntimeError(
                        f"low zswap residency for {compressor} round {round_idx}: "
                        f"stored_pages={stored}, required={args.min_stored_pages}; "
                        f"written_back_pages={pageout_delta.get('written_back_pages', 0)}; "
                        f"stdout={out}; stderr={err}"
                    )

                # Signal workload to scan (swapin / decompress) all pages
                write_text(phase_files["scan_go"], "go\n")
                wait_for_phase(proc, phase_files["scan_done"],
                               args.phase_timeout_s, "scan_done")
                after_scan = capture_state()
                workload = release_and_collect_workload(proc, phase_files)
                elapsed = time.time() - t0
                after = capture_state()
                dmesg_after = dmesg_marker()
                row = {
                    "round": round_idx,
                    "compressor": compressor,
                    "decode_backend": args.decode_backend,
                    "elapsed_s": elapsed,
                    "workload": workload,
                    "zswap_before": before["zswap"],
                    "zswap_after_pageout": after_pageout["zswap"],
                    "zswap_after_scan": after_scan["zswap"],
                    "zswap_after": after["zswap"],
                    "zswap_after_pageout_delta": pageout_delta,
                    "zswap_after_scan_delta": delta(after_scan["zswap"], before["zswap"]),
                    "zswap_final_delta": delta(after["zswap"], before["zswap"]),
                    "vmstat_delta": delta(after["vmstat"], before["vmstat"]),
                    "dmesg_new_warn_err": dmesg_after.replace(dmesg_before, "", 1),
                }
                raw_fh.write(json.dumps(row, sort_keys=True) + "\n")
                raw_fh.flush()
                rows.append(row)
                print(f"  done: scan_s={workload['scan_s']:.4f} "
                      f"pageout_s={workload['pageout_s']:.4f}", flush=True)
                proc = None
                phase_files = None
                cgroup_destroy(cg)
                cg = None
                swapoff_tmp()
        set_zswap_compressor("lz4")
        run(["rmmod", "aocl_lz4"], check=False)
    finally:
        raw_fh.close()
        terminate_workload(proc, phase_files)
        if cg is not None:
            cgroup_destroy(cg)
        swapoff_tmp()
        restore_zswap_params(original_params)

    summary = summarize(rows)
    with (RESULT_DIR / "summary.csv").open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=sorted({k for row in summary for k in row}))
        writer.writeheader()
        writer.writerows(summary)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
