#!/usr/bin/env python3
# Copyright Advanced Micro Devices, Inc.
# SPDX-License-Identifier: BSD-3-Clause
"""Deterministic zswap page-corpus generator.

Produces 5 fixed 2 MiB corpora (512 x 4 KiB pages each) spanning the
compressibility classes that matter for a zswap compressor study:

  text       highly compressible, english-like byte stream
  structured highly compressible, record/counter layout with long zero runs
  binary     semi-compressible, code/data-like (patterns + entropy)
  mixed      realistic mix: alternating compressible and incompressible regions
  random     incompressible (zswap would reject these; kept as guardrail)

Everything is seeded so the byte output is bit-for-bit reproducible. The
companion manifest.csv pins each file by sha256 and is the canonical
reproducibility anchor for the benchmark.
"""
import csv
import hashlib
import os
import random

PAGE = 4096
PAGES = 512
SIZE = PAGE * PAGES  # 2 MiB
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "corpus"))

WORDS = (
    "the quick brown fox jumps over a lazy dog while the system swaps memory "
    "pages into compressed storage and the kernel reclaims resident set under "
    "pressure to keep the working set warm in zswap pools and avoid disk io "
).split()


def gen_text(rng):
    out = bytearray()
    while len(out) < SIZE:
        line = " ".join(rng.choice(WORDS) for _ in range(rng.randint(6, 14)))
        out += line.encode() + b"\n"
    return bytes(out[:SIZE])


def gen_structured(rng):
    """Record-like: fixed header, mostly-zero payload, monotonic counters."""
    out = bytearray()
    counter = 0
    while len(out) < SIZE:
        rec = bytearray(64)
        rec[0:4] = b"\xde\xad\xbe\xef"
        rec[4:8] = counter.to_bytes(4, "little")
        # sparse non-zero fields -> long zero runs (very compressible)
        rec[16] = rng.randint(0, 7)
        rec[32] = 0x01
        out += rec
        counter += 1
    return bytes(out[:SIZE])


def gen_binary(rng):
    """Semi-compressible: repeated opcode-like motifs interleaved with entropy."""
    motifs = [bytes(rng.randint(0, 255) for _ in range(rng.randint(3, 8)))
              for _ in range(24)]
    out = bytearray()
    while len(out) < SIZE:
        if rng.random() < 0.6:
            out += rng.choice(motifs)
        else:
            out += bytes(rng.randint(0, 255) for _ in range(rng.randint(2, 6)))
    return bytes(out[:SIZE])


def gen_random(rng):
    return bytes(rng.randrange(256) for _ in range(SIZE))


def gen_mixed(rng):
    """Per-page: ~half pages compressible (text), ~half incompressible."""
    text = gen_text(random.Random(rng.random()))
    rnd = gen_random(random.Random(rng.random()))
    out = bytearray()
    for p in range(PAGES):
        src = text if (p % 2 == 0) else rnd
        out += src[p * PAGE:(p + 1) * PAGE]
    return bytes(out[:SIZE])


GENERATORS = {
    "text": (1, gen_text),
    "binary": (2, gen_binary),
    "structured": (3, gen_structured),
    "random": (4, gen_random),
    "mixed": (5, gen_mixed),
}


def main():
    os.makedirs(OUT, exist_ok=True)
    rows = []
    for name, (seed, fn) in GENERATORS.items():
        data = fn(random.Random(seed))
        assert len(data) == SIZE, (name, len(data))
        path = os.path.join(OUT, f"{name}.bin")
        with open(path, "wb") as f:
            f.write(data)
        sha = hashlib.sha256(data).hexdigest()
        rows.append((name, PAGES, SIZE, sha))
        print(f"  {name:10s} {SIZE} bytes sha256={sha[:16]}...")
    rows.sort(key=lambda r: GENERATORS[r[0]][0])
    with open(os.path.join(OUT, "manifest.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["class", "pages", "bytes", "sha256"])
        w.writerows(rows)
    print(f"wrote {len(rows)} corpora + manifest to {OUT}")


if __name__ == "__main__":
    main()
