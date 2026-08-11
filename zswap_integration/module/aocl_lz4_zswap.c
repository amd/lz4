// SPDX-License-Identifier: GPL-2.0
/*
 * aocl_lz4_zswap.c - kernel crypto (scomp) provider "aocl-lz4" for zswap/zram.
 *
 * Registers an LZ4 compressor backed by the kernel-safe AOCL-LZ4 core
 * (aocl-kernel/lz4.c + glue.c), selectable by zswap via:
 *
 *     echo aocl-lz4 > /sys/module/zswap/parameters/compressor
 *     # or boot with: zswap.compressor=aocl-lz4 zswap.enabled=1
 *
 * Compress:   scalar, kernel-safe (no FPU) -> AOCL LZ4_compress_default().
 * Decompress: kernel-safe scalar decoder by default (built with
 *             AOCL_LZ4_SCALAR), which needs no FPU state. When the module is
 *             built with AOCL_LZ4_AVX=1, LZ4_decompress_safe() routes to AOCL's
 *             hand-written AVX2 decoder instead; because that touches YMM
 *             registers, the decode is then bracketed by kernel_fpu_begin() /
 *             kernel_fpu_end() (one pair per page), which save & restore the
 *             FPU/SSE/AVX state. This is the same accepted technique used by
 *             the crypto and raid6 drivers, and is permitted in the
 *             process/kswapd (writeback) context in which zswap runs.
 *
 * Compress uses the default hash, so the compressed byte stream is
 * bit-identical to stock LZ4.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/crypto.h>
#include <linux/vmalloc.h>
#include <linux/err.h>
#ifndef AOCL_LZ4_SCALAR
/*
 * Only the AVX decode back-end needs x86-only facilities: kernel_fpu_begin/end
 * (<asm/fpu/api.h>) around the YMM region, and boot_cpu_has(X86_FEATURE_AVX2)
 * (<asm/cpufeature.h>) for the insmod-time capability gate. The AVX decoder is
 * itself x86-only, so fail fast if an AVX build is attempted on another arch.
 * The scalar back-end (default) includes neither and builds on any arch.
 */
#ifndef CONFIG_X86
#error "AOCL_LZ4_AVX build is x86-only; build the scalar back-end (AOCL_LZ4_SCALAR) on non-x86."
#endif
#include <asm/fpu/api.h>
#include <asm/cpufeature.h>
#endif /* !AOCL_LZ4_SCALAR */
#include <crypto/internal/scompress.h>

/*
 * AOCL-LZ4 kernel API, provided by the linked AOCL core objects
 * (glue.c defines the 5-arg LZ4_compress_default; lz4.c defines
 * LZ4_decompress_safe, which routes to the AVX decoder under
 * -DAOCL_LZ4_AVX_OPT). Declared here so this TU need not pull in AOCL's
 * userspace lz4.h.
 */
extern int LZ4_compress_default(const char *src, char *dst, int srcSize,
				int dstCapacity, void *wrkmem);
extern int LZ4_decompress_safe(const char *src, char *dst,
			       int compressedSize, int dstCapacity);

/*
 * Per-request compression scratch (hash table + state). Must be >= the AOCL
 * compressor's LZ4_MEM_COMPRESS. 128 KiB comfortably covers the DEFAULT
 * LZ4_MEMORY_USAGE hash-table footprint used by the ratio-neutral build.
 */
#define AOCL_LZ4_MEM_COMPRESS (128 * 1024)

static void *aocl_lz4_alloc_ctx(struct crypto_scomp *tfm)
{
	void *ctx;

	ctx = vmalloc(AOCL_LZ4_MEM_COMPRESS);
	if (!ctx)
		return ERR_PTR(-ENOMEM);
	return ctx;
}

static void aocl_lz4_free_ctx(struct crypto_scomp *tfm, void *ctx)
{
	vfree(ctx);
}

static int aocl_lz4_compress(struct crypto_scomp *tfm, const u8 *src,
			    unsigned int slen, u8 *dst, unsigned int *dlen,
			    void *ctx)
{
	int out_len;

	/*
	 * The AOCL LZ4 API is byte-oriented (char pointers, int lengths); scomp
	 * hands us u8 pointers and unsigned int lengths. Cast explicitly so the
	 * build stays clean under -Wpointer-sign / -Werror. The kernel already
	 * caps a page at INT_MAX, so the size casts cannot overflow here.
	 */
	out_len = LZ4_compress_default((const char *)src, (char *)dst,
				       (int)slen, (int)*dlen, ctx);
	if (out_len <= 0)
		return -EINVAL;

	*dlen = out_len;
	return 0;
}

static int aocl_lz4_decompress(struct crypto_scomp *tfm, const u8 *src,
			      unsigned int slen, u8 *dst, unsigned int *dlen,
			      void *ctx)
{
	int out_len;

#ifdef AOCL_LZ4_SCALAR
	/*
	 * SCALAR back-end (built with AOCL_LZ4_SCALAR=1): the decoder emits no
	 * SIMD and touches no FPU/YMM state, so NO kernel_fpu bracketing is
	 * required. This removes the per-page XSAVE/XRSTOR tax entirely, so the
	 * scalar decode uplift measured in userspace translates ~1:1 into the
	 * kernel.
	 */
	out_len = LZ4_decompress_safe((const char *)src, (char *)dst,
				      (int)slen, (int)*dlen);
#else
	/*
	 * AVX back-end: the decoder uses YMM registers; save/restore the FPU
	 * state around the SIMD region. One begin/end pair per page keeps the
	 * save/restore amortized over a full page of decode work. Legal here
	 * because zswap load runs in process/kswapd context, not hard IRQ.
	 * NOTE: this per-page save/restore erodes most of the AVX gain; prefer
	 * the AOCL_LZ4_SCALAR=1 build for in-kernel deployment.
	 */
	kernel_fpu_begin();
	out_len = LZ4_decompress_safe((const char *)src, (char *)dst,
				      (int)slen, (int)*dlen);
	kernel_fpu_end();
#endif

	if (out_len < 0)
		return -EINVAL;

	*dlen = out_len;
	return 0;
}

static struct scomp_alg scomp = {
	.alloc_ctx	= aocl_lz4_alloc_ctx,
	.free_ctx	= aocl_lz4_free_ctx,
	.compress	= aocl_lz4_compress,
	.decompress	= aocl_lz4_decompress,
	.base		= {
		.cra_name	= "aocl-lz4",
		.cra_driver_name = "aocl-lz4-scomp",
		.cra_module	= THIS_MODULE,
		.cra_priority	= 300,
	},
};

static int __init aocl_lz4_mod_init(void)
{
#ifndef AOCL_LZ4_SCALAR
	/*
	 * The AVX build routes decode through AOCL's AVX2 decoder. Refuse to
	 * load on a CPU without AVX2 so we fail cleanly at insmod instead of
	 * taking a #UD on the first page decode.
	 */
	if (!boot_cpu_has(X86_FEATURE_AVX2)) {
		pr_err("aocl-lz4: built for AVX2 decode but CPU lacks AVX2; not loading\n");
		return -ENODEV;
	}
#endif
	return crypto_register_scomp(&scomp);
}

static void __exit aocl_lz4_mod_exit(void)
{
	crypto_unregister_scomp(&scomp);
}

module_init(aocl_lz4_mod_init);
module_exit(aocl_lz4_mod_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AOCL-LZ4 crypto compressor for zswap/zram (scalar or AVX decode)");
MODULE_ALIAS_CRYPTO("aocl-lz4");
