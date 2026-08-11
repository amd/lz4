// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: BSD-3-Clause
/*
 * zswap_bench - impl-agnostic driver that measures a kernel LZ4 compressor over
 * the frozen 4 KiB-page corpus, exactly as zswap would store/load pages.
 *
 * It links against whichever compressor object set is supplied at build time
 * (kernel default lib/lz4 = baseline, or kernel-safe AOCL-LZ4 = candidate),
 * which must export the kernel API symbols:
 *     int LZ4_compress_default(const char*, char*, int, int, void*);
 *     int LZ4_decompress_safe(const char*, char*, int, int);
 *
 * Modes:
 *   (default)          print matrix CSV: class,pressure,op,mbps
 *   --selftest         round-trip correctness over all classes; exit 0/1
 *   --one CLASS PRESS  long single-workload loop for profiling (AMDuProf/SAGE)
 *
 * Deterministic: fixed corpus, fixed iteration counts, median of sub-trials.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

/* kernel LZ4 API (provided by the linked compressor objects) */
extern int LZ4_compress_default(const char *src, char *dst, int srcSize,
				int dstCapacity, void *wrkmem);
extern int LZ4_decompress_safe(const char *src, char *dst, int compressedSize,
			       int dstCapacity);

#define PAGE 4096
#define MAXPAGES 512
#define CORPUS_BYTES (PAGE * MAXPAGES)
#define DSTCAP 8192                 /* > LZ4_compressBound(4096) */
#define WRKMEM_BYTES (1 << 17)      /* 128 KiB: >= any LZ4_MEM_COMPRESS */
#define PAGEOPS_TARGET 16384        /* page-ops per op per sub-trial */
#define NSUB 3                      /* sub-trials; report median */

static const char *CLASSES[] = {"text", "binary", "structured", "mixed", "random"};
static const int NCLASS = 5;
static const char *PRESS_NAME[] = {"moderate", "high", "near_limit"};
static const int PRESS_PAGES[] = {64, 256, 512};
static const int NPRESS = 3;

static unsigned char g_corpus[CORPUS_BYTES];
static unsigned char g_comp[MAXPAGES][DSTCAP];
static int g_csize[MAXPAGES];
static unsigned char g_out[PAGE];
static unsigned char g_wrk[WRKMEM_BYTES] __attribute__((aligned(64)));

/*
 * Corpus location. Set ZSWAP_CORPUS to the directory holding the <class>.bin
 * files; if unset it defaults to "corpus" relative to the current working
 * directory (the harness is normally invoked from the integration root).
 */
static const char *corpus_dir(void)
{
	const char *e = getenv("ZSWAP_CORPUS");
	return e ? e : "corpus";
}

static int load_class(const char *cls)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.bin", corpus_dir(), cls);
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
	size_t n = fread(g_corpus, 1, CORPUS_BYTES, f);
	fclose(f);
	if (n != CORPUS_BYTES) { fprintf(stderr, "short read %s\n", path); return -1; }
	return 0;
}

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int dcmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}
static double median(double *v, int n)
{
	qsort(v, n, sizeof(double), dcmp);
	return v[n / 2];
}

/* Compress all `pages` once; fills g_comp/g_csize. Returns 0 on success. */
static int compress_all(int pages)
{
	for (int p = 0; p < pages; p++) {
		int cs = LZ4_compress_default((const char *)(g_corpus + p * PAGE),
					      (char *)g_comp[p], PAGE, DSTCAP, g_wrk);
		if (cs <= 0) { fprintf(stderr, "compress fail page %d\n", p); return -1; }
		g_csize[p] = cs;
	}
	return 0;
}

/* Returns compress / decompress / roundtrip MB/s (median of NSUB) + ratio. */
static void measure_cell(int pages, double *c_mbps, double *d_mbps,
			 double *rt_mbps, double *ratio)
{
	int reps = PAGEOPS_TARGET / pages;
	if (reps < 1) reps = 1;
	double in_mb = (double)pages * PAGE * reps / (1024.0 * 1024.0);

	double cs[NSUB], ds[NSUB], rs[NSUB];
	long comp_bytes = 0;
	for (int s = 0; s < NSUB; s++) {
		/* compress timing */
		double t0 = now_s();
		for (int r = 0; r < reps; r++)
			if (compress_all(pages)) { *c_mbps = *d_mbps = *rt_mbps = *ratio = 0; return; }
		double tc = now_s() - t0;

		comp_bytes = 0;
		for (int p = 0; p < pages; p++) comp_bytes += g_csize[p];

		/* decompress timing (uses last compression pass) */
		t0 = now_s();
		for (int r = 0; r < reps; r++) {
			for (int p = 0; p < pages; p++) {
				int n = LZ4_decompress_safe((const char *)g_comp[p],
						(char *)g_out, g_csize[p], PAGE);
				if (n != PAGE) { fprintf(stderr, "decompress fail p%d n%d\n", p, n);
					*c_mbps = *d_mbps = *rt_mbps = *ratio = 0; return; }
			}
		}
		double td = now_s() - t0;

		cs[s] = in_mb / tc;
		ds[s] = in_mb / td;
		rs[s] = in_mb / (tc + td);
	}
	*c_mbps = median(cs, NSUB);
	*d_mbps = median(ds, NSUB);
	*rt_mbps = median(rs, NSUB);
	*ratio = (double)(pages * PAGE) / (double)comp_bytes; /* >1 = more compression */
}

static int run_matrix(void)
{
	printf("class,pressure,op,mbps\n");
	for (int ci = 0; ci < NCLASS; ci++) {
		if (load_class(CLASSES[ci])) return 1;
		for (int pi = 0; pi < NPRESS; pi++) {
			double c, d, rt, ratio;
			measure_cell(PRESS_PAGES[pi], &c, &d, &rt, &ratio);
			const char *cl = CLASSES[ci], *pn = PRESS_NAME[pi];
			printf("%s,%s,compress,%.3f\n", cl, pn, c);
			printf("%s,%s,decompress,%.3f\n", cl, pn, d);
			printf("%s,%s,roundtrip,%.3f\n", cl, pn, rt);
			printf("%s,%s,ratio,%.4f\n", cl, pn, ratio);
		}
	}
	return 0;
}

static int run_selftest(void)
{
	int fails = 0;
	for (int ci = 0; ci < NCLASS; ci++) {
		if (load_class(CLASSES[ci])) return 1;
		if (compress_all(MAXPAGES)) { fprintf(stderr, "[selftest] compress error %s\n", CLASSES[ci]); return 1; }
		for (int p = 0; p < MAXPAGES; p++) {
			int n = LZ4_decompress_safe((const char *)g_comp[p], (char *)g_out,
						    g_csize[p], PAGE);
			if (n != PAGE || memcmp(g_out, g_corpus + p * PAGE, PAGE) != 0) {
				fprintf(stderr, "[selftest] MISMATCH %s page %d (n=%d)\n",
					CLASSES[ci], p, n);
				fails++;
				if (fails > 8) { fprintf(stderr, "[selftest] aborting\n"); break; }
			}
		}
	}
	if (fails) { printf("[selftest] FAIL (%d mismatches)\n", fails); return 1; }
	printf("[selftest] PASS (all classes round-trip OK)\n");
	return 0;
}

static int run_one(const char *cls, const char *press)
{
	int pages = PRESS_PAGES[NPRESS - 1];
	for (int i = 0; i < NPRESS; i++) if (!strcmp(press, PRESS_NAME[i])) pages = PRESS_PAGES[i];
	if (load_class(cls)) return 1;
	/* long, steady loop so a profiler captures the hot compressor path */
	for (int iter = 0; iter < 4000; iter++) {
		if (compress_all(pages)) return 1;
		for (int p = 0; p < pages; p++)
			if (LZ4_decompress_safe((const char *)g_comp[p], (char *)g_out,
						g_csize[p], PAGE) != PAGE) return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "--selftest")) return run_selftest();
	if (argc >= 4 && !strcmp(argv[1], "--one")) return run_one(argv[2], argv[3]);
	return run_matrix();
}
