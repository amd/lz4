# `aocl-lz4` — in-kernel zswap compressor

This directory contains the kernel-module integration of the kernel-safe
AOCL-LZ4 path so it can be used by **zswap** (and zram) as a selectable
compressor named `aocl-lz4`.

## What it does

| Path       | Default implementation | FPU/SIMD |
|------------|------------------------|----------|
| Compress   | AOCL scalar `LZ4_compress_default` (default hash) | none (kernel-safe) |
| Decompress | AOCL scalar decoder + software-pipelined prefetch | none (kernel-safe, no `kernel_fpu` tax) |

The compressed byte stream is **bit-identical to stock LZ4** (the compress side
is unchanged), so pages written by `lz4` can be read by `aocl-lz4` and
vice-versa.

## Build options (independent, additive)

The default build (`make`) is the kernel-safe scalar decoder. The following
toggles stack on top of it in any combination:

| Toggle | Effect |
|--------|--------|
| `AOCL_LZ4_AVX=1` | Route decode through AOCL's hand-written **AVX2** decoder (`AOCL_LZ4_AVX_OPT`), bracketed by `kernel_fpu_begin()/end()`. See caveat below. |
| `AOCL_LZ4_OPTSTACK=1` | Enable the compressor hash + codegen tuning (`-DLZ4_MEMORY_USAGE=12` + scalar codegen flags). Composes with either decode back-end; trades a little compression ratio. |

Example: `make AOCL_LZ4_AVX=1 AOCL_LZ4_OPTSTACK=1`.

> **AVX caveat:** `AOCL_LZ4_AVX_OPT` routes `LZ4_decompress_safe` to a *distinct*
> AVX decoder that does **not** execute the scalar pipe hints, so `AOCL_LZ4_AVX=1`
> **replaces** (does not accelerate) the scalar-pipe decode. The per-page
> `kernel_fpu` save/restore also adds overhead, which is why the scalar decode is
> the recommended in-kernel default.

## Why `kernel_fpu_begin()/kernel_fpu_end()` (AVX build only)

The AVX2 decoder touches YMM registers. Kernel code must not clobber user
FPU/SIMD state, so any SIMD region must be wrapped with
`kernel_fpu_begin()` / `kernel_fpu_end()`, which save & restore that state.
This is the same accepted technique used by the kernel **crypto** and
**raid6** drivers.

`zswap` compress/decompress run in **process / kswapd (writeback) context**,
where `kernel_fpu_begin()` is permitted (it is *not* permitted from hard IRQ).
One begin/end pair wraps each **page** decode (`PAGE_SIZE` = 4 KiB on x86_64),
amortizing the save/restore over a full page of decode work.

See `aocl_lz4_decompress()` in `aocl_lz4_zswap.c`:

```c
kernel_fpu_begin();
out_len = LZ4_decompress_safe(src, dst, slen, *dlen); /* AVX decoder */
kernel_fpu_end();
```

## Building and deploying

```sh
make                          # builds aocl_lz4.ko (scalar decode, default)
# or, to stack optional optimizations:
#   make AOCL_LZ4_AVX=1       # AVX2 decoder (kernel_fpu-bracketed)
#   make AOCL_LZ4_OPTSTACK=1  # compressor hash + codegen tuning
sudo insmod aocl_lz4.ko
# select it for zswap:
echo aocl-lz4 | sudo tee /sys/module/zswap/parameters/compressor
echo 1        | sudo tee /sys/module/zswap/parameters/enabled
# or at boot:  zswap.enabled=1 zswap.compressor=aocl-lz4
```

> **Note:** building the full `.ko` links the AOCL core (`aocl_core.o`) from
> `../aocl-kernel/lz4.c` under `-nostdinc`; see **Build status** below for the
> current porting state and what is validated today.

To compile just the crypto/`kernel_fpu` glue against the running kernel headers
(without the AOCL core objects):

```sh
make gluecheck                # -> aocl_lz4_zswap.o
```

## Build status

- **Kernel-clean today:** `aocl_lz4_zswap.c` (the crypto `scomp` glue +
  `kernel_fpu` bracketing) compiles against the running kernel's headers via
  `make gluecheck`.
- **Full in-tree `.ko`:** building `aocl_core.o` from `../aocl-kernel/lz4.c`
  under `-nostdinc` works via `-isystem $(cc_intrin_dir)` in `Kbuild`, which
  provides the compiler's freestanding headers (`stddef.h`, `stdatomic.h`, etc.).
  The `stdlib.h` shim (`include/stdlib.h`) satisfies `mm_malloc.h` for the
  intrinsic path. Remaining item: resolving a couple of kernel-vs-AOCL type
  clashes. The decode/`kernel_fpu` logic itself is validated in userspace via
  the `aocl` / `aocl_avx` builds of `../harness/scripts/build_impl.sh`
  (see `../README.md`).
