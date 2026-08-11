// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: BSD-3-Clause
/*
 * zswap_kernel_workload.c - real-kernel zswap benchmark workload.
 *
 * This program creates anonymous private pages filled from the public-domain
 * Silesia corpus, asks the kernel to reclaim them with MADV_PAGEOUT, then reads
 * them back and checksums every page so swapin/decompression cannot be optimized
 * away. It intentionally does not link any LZ4 code: compression/decompression
 * happens only inside the running kernel's zswap path.
 *
 * Phase protocol (env-var file handshakes with the runner):
 *   1. Workload starts, loads corpus, allocates+fills all pages (unconstrained)
 *   2. Signals FILL_DONE, waits for PAGEOUT_GO
 *      (runner moves process into cgroup here, then signals PAGEOUT_GO)
 *   3. Calls MADV_PAGEOUT -> kernel reclaims pages through zswap
 *   4. Signals PAGEOUT_DONE, waits for SCAN_GO
 *      (runner samples zswap counters, verifies residency, signals SCAN_GO)
 *   5. Scans all pages (triggers swapin/decompression), checksums them
 *   6. Signals SCAN_DONE, waits for EXIT_GO
 *      (runner samples post-scan counters, signals EXIT_GO)
 *   7. Prints JSON result, unmaps, exits
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

#define PAGE 4096UL

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint64_t fnv1a_update(uint64_t h, const unsigned char *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static int touch_file(const char *path)
{
	int fd;
	if (!path)
		return 0;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "touch %s: %s\n", path, strerror(errno));
		return -1;
	}
	close(fd);
	return 0;
}

static int wait_for_file(const char *path)
{
	if (!path)
		return 0;
	for (;;) {
		if (access(path, F_OK) == 0)
			return 0;
		usleep(1000);
	}
}

static int signal_and_wait(const char *signal_env, const char *wait_env)
{
	const char *sig = getenv(signal_env);
	const char *wait = getenv(wait_env);
	if (touch_file(sig) != 0)
		return -1;
	return wait_for_file(wait);
}

static unsigned char *load_corpus(const char *dir, size_t *out_len)
{
	static const char *files[] = {
		"dickens", "mozilla", "mr", "nci", "ooffice", "osdb",
		"reymont", "samba", "sao", "webster", "xml", "x-ray",
	};
	unsigned char *buf = NULL;
	size_t cap = 0, len = 0;

	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		char path[1024];
		struct stat st;
		int fd;
		ssize_t n;

		snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
		fd = open(path, O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "open %s: %s\n", path, strerror(errno));
			free(buf);
			return NULL;
		}
		if (fstat(fd, &st) != 0 || st.st_size <= 0) {
			fprintf(stderr, "stat %s failed\n", path);
			close(fd);
			free(buf);
			return NULL;
		}
		if (len + (size_t)st.st_size > cap) {
			size_t new_cap = len + (size_t)st.st_size;
			unsigned char *tmp = realloc(buf, new_cap);
			if (!tmp) {
				close(fd);
				free(buf);
				return NULL;
			}
			buf = tmp;
			cap = new_cap;
		}
		/* read() may return short; loop until the whole file is in. */
		{
			size_t got = 0;
			while (got < (size_t)st.st_size) {
				n = read(fd, buf + len + got,
					 (size_t)st.st_size - got);
				if (n < 0) {
					if (errno == EINTR)
						continue;
					break;
				}
				if (n == 0)
					break; /* unexpected EOF */
				got += (size_t)n;
			}
			close(fd);
			if (got != (size_t)st.st_size) {
				fprintf(stderr, "short read %s\n", path);
				free(buf);
				return NULL;
			}
			len += got;
		}
	}

	*out_len = len;
	return buf;
}

int main(int argc, char **argv)
{
	const char *corpus_dir = NULL;
	size_t mib = 1536;
	size_t corpus_len = 0;
	unsigned char *corpus;
	unsigned char *mem;
	size_t bytes, pages;
	double t0, fill_s, pageout_s, scan_s;
	uint64_t checksum = 1469598103934665603ULL;
	int do_pageout = 1;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--corpus-dir") && i + 1 < argc) {
			corpus_dir = argv[++i];
		} else if (!strcmp(argv[i], "--mib") && i + 1 < argc) {
			mib = strtoull(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--no-pageout")) {
			do_pageout = 0;
		} else {
			fprintf(stderr, "usage: %s --corpus-dir DIR [--mib N] [--no-pageout]\n", argv[0]);
			return 2;
		}
	}
	if (!corpus_dir) {
		fprintf(stderr, "--corpus-dir is required\n");
		return 2;
	}

	corpus = load_corpus(corpus_dir, &corpus_len);
	if (!corpus)
		return 2;

	/* The fill loop below indexes the corpus as `... % (corpus_len - PAGE)`
	 * and memcpy's a full PAGE from it, so the corpus must be strictly larger
	 * than one page. Guard here to fail with a clear message instead of a
	 * size_t underflow / divide-by-zero (corpus_len == PAGE) or an
	 * out-of-bounds read (corpus_len < PAGE) when the corpus dir is incomplete
	 * or holds unexpectedly small files. */
	if (corpus_len <= PAGE) {
		fprintf(stderr,
			"corpus too small: %zu bytes (need > %zu, one page); "
			"check that '%s' contains real corpus files\n",
			corpus_len, (size_t)PAGE, corpus_dir);
		free(corpus);
		return 2;
	}

	/* Allocate anonymous pages WITHOUT MAP_POPULATE — the fill loop below
	 * touches every page anyway. This avoids double-faulting. */
	bytes = (mib * 1024UL * 1024UL) & ~(PAGE - 1);
	pages = bytes / PAGE;
	mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		fprintf(stderr, "mmap %zu MiB failed: %s\n", mib, strerror(errno));
		free(corpus);
		return 2;
	}

	/* Fill every page from the corpus. This happens BEFORE the runner
	 * moves us into the memory-limited cgroup. */
	t0 = now_s();
	for (size_t p = 0; p < pages; p++) {
		size_t off = (p * PAGE) % (corpus_len - PAGE);
		memcpy(mem + p * PAGE, corpus + off, PAGE);
	}
	fill_s = now_s() - t0;
	free(corpus);

	/* PHASE: signal fill_done, wait for pageout_go.
	 * The runner moves us into the cgroup between these two. */
	if (signal_and_wait("ZSWAP_BENCH_FILL_DONE_FILE",
			    "ZSWAP_BENCH_PAGEOUT_GO_FILE") != 0)
		return 2;

	/* Now inside the cgroup — MADV_PAGEOUT triggers reclaim through zswap */
	t0 = now_s();
	if (do_pageout && madvise(mem, bytes, MADV_PAGEOUT) != 0) {
		fprintf(stderr, "madvise(MADV_PAGEOUT) failed: %s\n", strerror(errno));
	}
	pageout_s = now_s() - t0;

	usleep(500000);

	/* PHASE: signal pageout_done, wait for scan_go */
	if (signal_and_wait("ZSWAP_BENCH_PAGEOUT_DONE_FILE",
			    "ZSWAP_BENCH_SCAN_GO_FILE") != 0)
		return 2;

	/* Scan all pages — triggers swapin / zswap decompression */
	t0 = now_s();
	for (size_t p = 0; p < pages; p++)
		checksum = fnv1a_update(checksum, mem + p * PAGE, 64);
	scan_s = now_s() - t0;

	/* PHASE: signal scan_done, wait for exit_go */
	if (signal_and_wait("ZSWAP_BENCH_SCAN_DONE_FILE",
			    "ZSWAP_BENCH_EXIT_GO_FILE") != 0)
		return 2;

	printf("{\"mib\":%zu,\"bytes\":%zu,\"pages\":%zu,"
	       "\"fill_s\":%.6f,\"pageout_s\":%.6f,\"scan_s\":%.6f,"
	       "\"checksum\":\"%016llx\"}\n",
	       mib, bytes, pages, fill_s, pageout_s, scan_s,
	       (unsigned long long)checksum);

	munmap(mem, bytes);
	return 0;
}
