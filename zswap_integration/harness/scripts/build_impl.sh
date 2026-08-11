#!/usr/bin/env bash
# Copyright Advanced Micro Devices, Inc.
# SPDX-License-Identifier: BSD-3-Clause
# build_impl.sh <impl> <out> [driver.c]
#
# Builds the zswap_bench userspace driver against one compressor implementation:
#   baseline   -> kernel-lz4-baseline/  (faithful Linux v6.8 lib/lz4)
#   aocl       -> aocl-kernel/           (kernel-safe AOCL-LZ4, scalar decode)
#   aocl_avx   -> aocl-kernel/           (AOCL-LZ4 with the AVX2 decoder)
#
# The compressor sources are compiled with KERNEL-SAFE flags (freestanding,
# no SIMD/intrinsics, no autovectorization) so the comparison reflects what
# would actually run inside zswap. The userspace driver itself may use
# -march=native (it is a harness, not the measured compressor).
set -euo pipefail
# HARNESS = the harness/ dir (bench, kcompat, kernel-lz4-baseline, build, ...).
# INTEG   = the integration root one level up (holds the product: aocl-kernel/).
HARNESS="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INTEG="$(cd "$HARNESS/.." && pwd)"
IMPL="$1"; OUT="$2"
# Optional 3rd arg: benchmark driver source (default: matrix driver).
DRIVER="${3:-$HARNESS/bench/zswap_bench.c}"
BUILD="$HARNESS/build/$IMPL"; mkdir -p "$BUILD"

# Kernel-safe compressor flags: no FPU/SSE/AVX (FPU is unavailable in kernel
# context without kernel_fpu_begin), no tree vectorization, freestanding.
KFLAGS="-O3 -std=gnu11 -fno-strict-aliasing -fno-tree-vectorize -ffreestanding \
        -fno-stack-protector -fno-builtin-printf \
        -mno-sse -mno-sse2 -mno-mmx -mno-3dnow -mno-avx \
        -DCONFIG_64BIT=1 -D__LITTLE_ENDIAN=1"
# Userspace driver flags (helper only; not the measured compressor).
UFLAGS="-O2 -std=gnu11 -march=native"

INC_KC="-I$HARNESS/kcompat"

# Extra compressor-only flags (appended to KFLAGS for the compressor TUs only,
# never the driver). Empty unless a specific impl sets it.
KEXTRA=""

collect_aocl_srcs() {
  SRC_DIR="$INTEG/aocl-kernel"
  INC="$INC_KC -I$SRC_DIR"
  if [ ! -d "$SRC_DIR" ]; then
    echo "[build] ERROR: $SRC_DIR not present" >&2; exit 2
  fi
  mapfile -t SRCS < <(find "$SRC_DIR" -maxdepth 1 -name '*.c' | sort)
  if [ "${#SRCS[@]}" -eq 0 ]; then echo "[build] ERROR: no .c in $SRC_DIR" >&2; exit 2; fi
}

case "$IMPL" in
  baseline)
    SRC_DIR="$HARNESS/kernel-lz4-baseline"
    INC="$INC_KC -I$SRC_DIR"
    SRCS=("$SRC_DIR/lz4_compress.c" "$SRC_DIR/lz4_decompress.c")
    ;;
  aocl)
    # Production scalar decode: AOCL decode prefetch is enabled in-source under
    # AOCL_LZ4_OPT. Matches the kernel module's default build (module/Kbuild).
    collect_aocl_srcs
    KEXTRA=""
    ;;
  aocl_avx)
    # AVX2 decoder variant (userspace correctness/throughput check). In-kernel
    # this decode path is bracketed by kernel_fpu_begin()/kernel_fpu_end();
    # in userspace those resolve to the kcompat no-op shim, so timing here does
    # NOT include the per-page FPU save/restore cost. Compress stays scalar.
    collect_aocl_srcs
    KEXTRA="-mavx2 -mavx -mno-avx512f -ftree-vectorize -DAOCL_LZ4_AVX_OPT"
    ;;
  *) echo "[build] unknown impl: $IMPL (expected: baseline | aocl | aocl_avx)" >&2; exit 2 ;;
esac

OBJS=()
for s in "${SRCS[@]}"; do
  o="$BUILD/$(basename "${s%.c}").o"
  gcc $KFLAGS $KEXTRA $INC -c "$s" -o "$o"
  OBJS+=("$o")
done

# driver (default zswap_bench.c matrix driver; overridable via 3rd arg)
gcc $UFLAGS -c "$DRIVER" -o "$BUILD/driver.o"
OBJS+=("$BUILD/driver.o")

gcc -o "$OUT" "${OBJS[@]}"
echo "[build] $IMPL -> $OUT (compressor: $(basename "$SRC_DIR"), kernel-safe flags)"
