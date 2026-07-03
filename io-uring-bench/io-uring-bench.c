/*
 * io-uring-bench: self-contained io_uring storage microbenchmark.
 *
 * Drives a real io_uring workload (randread/randwrite/read/write) against a
 * file or block device using RAW io_uring syscalls (io_uring_setup(2) /
 * io_uring_enter(2)) and a hand-rolled mmap'd SQ/CQ ring -- no liburing, no
 * external dependency beyond libc, matching apps/ec2-audit style (C99, JSON
 * out via a growable struct buf).
 *
 * It keeps QD operations in flight, reaps completions, and reports total IOs,
 * elapsed time, IOPS, MB/s, and latency percentiles (p50/p99/p999) from a
 * fixed-bucket microsecond histogram. Results are emitted as JSON to stdout.
 *
 * Only IORING_OP_READ / IORING_OP_WRITE are used: single-buffer read/write
 * keyed on sqe->off, which OSv's core/io_uring.cc implements (verified against
 * the pr/io-uring-enhance branch). We deliberately avoid READV/READ_FIXED and
 * exotic opcodes so the harness runs identically on Linux and OSv.
 *
 * ---- Dual-boot A/B correspondence (OSv vs stock Linux) --------------------
 * The canonical default job below is mirrored exactly by baseline.fio so the
 * SAME EC2 instance can run this under OSv, then boot Amazon Linux and run
 * `fio baseline.fio` against the same device/file. Correspondence:
 *
 *   this bench flag        fio jobfile knob
 *   --------------------   ----------------------------
 *   --rw=randread          rw=randread
 *   --bs=4096              bs=4k
 *   --qd=32                iodepth=32
 *   --size=1073741824      size=1g
 *   --runtime=30           runtime=30 + time_based
 *   (buffered by default)  direct=0            <-- default; O_DIRECT off
 *   --direct               direct=1
 *   single ring, 1 job     ioengine=io_uring, numjobs=1
 *
 * Canonical default: QD=32, bs=4096, rw=randread, size=1 GiB, buffered
 * (direct=0), single io_uring, single thread. Override --runtime for a
 * time-based run (matches fio time_based).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/fs.h>

/* ---- io_uring syscall numbers (identical on x86_64 and aarch64) --------- */

#ifndef __NR_io_uring_setup
#if defined(__x86_64__) || defined(__aarch64__)
#define __NR_io_uring_setup 425
#define __NR_io_uring_enter 426
#define __NR_io_uring_register 427
#endif
#endif

/* ---- io_uring ABI (stable; mirrors linux/io_uring.h) -------------------- */

struct app_sqring_offsets {
	uint32_t head;
	uint32_t tail;
	uint32_t ring_mask;
	uint32_t ring_entries;
	uint32_t flags;
	uint32_t dropped;
	uint32_t array;
	uint32_t resv1;
	uint64_t resv2;
};

struct app_cqring_offsets {
	uint32_t head;
	uint32_t tail;
	uint32_t ring_mask;
	uint32_t ring_entries;
	uint32_t overflow;
	uint32_t cqes;
	uint32_t flags;
	uint32_t resv1;
	uint64_t resv2;
};

struct app_io_uring_params {
	uint32_t sq_entries;
	uint32_t cq_entries;
	uint32_t flags;
	uint32_t sq_thread_cpu;
	uint32_t sq_thread_idle;
	uint32_t features;
	uint32_t wq_fd;
	uint32_t resv[3];
	struct app_sqring_offsets sq_off;
	struct app_cqring_offsets cq_off;
};

struct app_io_uring_sqe {
	uint8_t opcode;
	uint8_t flags;
	uint16_t ioprio;
	int32_t fd;
	union {
		uint64_t off;
		uint64_t addr2;
	};
	union {
		uint64_t addr;
		uint64_t splice_off_in;
	};
	uint32_t len;
	union {
		uint32_t rw_flags;
		uint32_t fsync_flags;
	};
	uint64_t user_data;
	union {
		uint16_t buf_index;
		uint16_t buf_group;
	};
	uint16_t personality;
	union {
		int32_t splice_fd_in;
		uint32_t file_index;
	};
	uint64_t __pad2[2];
};

struct app_io_uring_cqe {
	uint64_t user_data;
	int32_t res;
	uint32_t flags;
};

#define APP_IORING_OP_READ 22
#define APP_IORING_OP_WRITE 23

#define APP_IORING_OFF_SQ_RING 0ULL
#define APP_IORING_OFF_CQ_RING 0x8000000ULL
#define APP_IORING_OFF_SQES 0x10000000ULL

#define APP_IORING_ENTER_GETEVENTS 1U

/* ---- growable string buffer for JSON assembly (ec2-audit style) --------- */

struct buf {
	char *p;
	size_t len;
	size_t cap;
};

static void buf_reserve(struct buf *b, size_t extra)
{
	if (b->len + extra + 1 <= b->cap)
		return;
	size_t ncap = b->cap ? b->cap * 2 : 4096;
	while (ncap < b->len + extra + 1)
		ncap *= 2;
	char *np = realloc(b->p, ncap);
	if (!np) {
		perror("realloc");
		exit(1);
	}
	b->p = np;
	b->cap = ncap;
}

static void buf_add(struct buf *b, const char *s, size_t n)
{
	buf_reserve(b, n);
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = '\0';
}

#define buf_lit(b, s) buf_add((b), (s), sizeof(s) - 1)

static void buf_printf(struct buf *b, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n < sizeof(tmp)) {
		buf_add(b, tmp, (size_t)n);
		return;
	}
	buf_reserve(b, (size_t)n);
	va_start(ap, fmt);
	vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->len += (size_t)n;
}

static void buf_json_str(struct buf *b, const char *s)
{
	buf_lit(b, "\"");
	for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
		switch (*c) {
		case '"':  buf_lit(b, "\\\""); break;
		case '\\': buf_lit(b, "\\\\"); break;
		case '\n': buf_lit(b, "\\n"); break;
		case '\r': buf_lit(b, "\\r"); break;
		case '\t': buf_lit(b, "\\t"); break;
		default:
			if (*c < 0x20)
				buf_printf(b, "\\u%04x", *c);
			else
				buf_add(b, (const char *)c, 1);
		}
	}
	buf_lit(b, "\"");
}

/* ---- latency histogram: fixed microsecond buckets ----------------------- */

/*
 * 4096 buckets. Bucket i for i<1024 == i microseconds (1us granularity to 1ms).
 * Above 1ms we widen: buckets 1024..2047 == 1ms..~64ms in ~64us steps, etc.
 * A monotone bucket->us mapping is enough for percentile estimation and is
 * fully dependency-free.
 */
#define HIST_BUCKETS 4096

static uint64_t hist[HIST_BUCKETS];

static unsigned lat_bucket(uint64_t us)
{
	if (us < 1024)
		return (unsigned)us;
	if (us < 65536)
		return 1024 + (unsigned)((us - 1024) >> 6);
	if (us < 1048576)
		return 2048 + (unsigned)((us - 65536) >> 10);
	unsigned b = 3072 + (unsigned)((us - 1048576) >> 14);
	return b < HIST_BUCKETS ? b : HIST_BUCKETS - 1;
}

/* Lower bound (us) represented by a bucket, for percentile reporting. */
static uint64_t bucket_us(unsigned b)
{
	if (b < 1024)
		return b;
	if (b < 2048)
		return 1024 + ((uint64_t)(b - 1024) << 6);
	if (b < 3072)
		return 65536 + ((uint64_t)(b - 2048) << 10);
	return 1048576 + ((uint64_t)(b - 3072) << 14);
}

static uint64_t percentile(uint64_t total, double pct)
{
	uint64_t target = (uint64_t)(total * pct);
	uint64_t acc = 0;
	for (unsigned i = 0; i < HIST_BUCKETS; i++) {
		acc += hist[i];
		if (acc >= target)
			return bucket_us(i);
	}
	return bucket_us(HIST_BUCKETS - 1);
}

/* ---- time + rng helpers ------------------------------------------------- */

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* SplitMix64: tiny, fast, dependency-free PRNG for random offsets. */
static uint64_t rng_state;

static uint64_t rng_next(void)
{
	uint64_t z = (rng_state += 0x9e3779b97f4a7c15ULL);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

/* ---- ring state --------------------------------------------------------- */

struct ring {
	int fd;
	unsigned sq_entries;

	/* SQ ring pointers */
	unsigned *sq_head, *sq_tail, *sq_mask, *sq_array;
	struct app_io_uring_sqe *sqes;

	/* CQ ring pointers */
	unsigned *cq_head, *cq_tail, *cq_mask;
	struct app_io_uring_cqe *cqes;

	void *sq_ptr;
	size_t sq_sz;
	void *cq_ptr;
	size_t cq_sz;
	void *sqe_ptr;
	size_t sqe_sz;
};

static int ring_setup(struct ring *r, unsigned entries)
{
	struct app_io_uring_params p;
	memset(&p, 0, sizeof(p));

	long fd = syscall(__NR_io_uring_setup, entries, &p);
	if (fd < 0)
		return -1;
	r->fd = (int)fd;
	r->sq_entries = p.sq_entries;

	r->sq_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
	r->cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct app_io_uring_cqe);
	r->sqe_sz = p.sq_entries * sizeof(struct app_io_uring_sqe);

	r->sq_ptr = mmap(NULL, r->sq_sz, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_POPULATE, r->fd, APP_IORING_OFF_SQ_RING);
	if (r->sq_ptr == MAP_FAILED)
		return -1;
	r->cq_ptr = mmap(NULL, r->cq_sz, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_POPULATE, r->fd, APP_IORING_OFF_CQ_RING);
	if (r->cq_ptr == MAP_FAILED)
		return -1;
	r->sqe_ptr = mmap(NULL, r->sqe_sz, PROT_READ | PROT_WRITE,
			  MAP_SHARED | MAP_POPULATE, r->fd, APP_IORING_OFF_SQES);
	if (r->sqe_ptr == MAP_FAILED)
		return -1;

	char *sq = (char *)r->sq_ptr;
	r->sq_head = (unsigned *)(sq + p.sq_off.head);
	r->sq_tail = (unsigned *)(sq + p.sq_off.tail);
	r->sq_mask = (unsigned *)(sq + p.sq_off.ring_mask);
	r->sq_array = (unsigned *)(sq + p.sq_off.array);
	r->sqes = (struct app_io_uring_sqe *)r->sqe_ptr;

	char *cq = (char *)r->cq_ptr;
	r->cq_head = (unsigned *)(cq + p.cq_off.head);
	r->cq_tail = (unsigned *)(cq + p.cq_off.tail);
	r->cq_mask = (unsigned *)(cq + p.cq_off.ring_mask);
	r->cqes = (struct app_io_uring_cqe *)(cq + p.cq_off.cqes);

	return 0;
}

/* ---- workload config ---------------------------------------------------- */

enum rw_mode { RW_RANDREAD, RW_RANDWRITE, RW_READ, RW_WRITE };

struct cfg {
	const char *path;
	enum rw_mode mode;
	unsigned qd;
	unsigned bs;
	uint64_t size;    /* file/device size to operate over (bytes) */
	unsigned runtime; /* seconds; 0 == run until `size` bytes done */
	int direct;       /* O_DIRECT */
};

static int is_write(enum rw_mode m)
{
	return m == RW_RANDWRITE || m == RW_WRITE;
}

static int is_random(enum rw_mode m)
{
	return m == RW_RANDREAD || m == RW_RANDWRITE;
}

static const char *mode_name(enum rw_mode m)
{
	switch (m) {
	case RW_RANDREAD:  return "randread";
	case RW_RANDWRITE: return "randwrite";
	case RW_READ:      return "read";
	case RW_WRITE:     return "write";
	}
	return "?";
}

/* ---- per-slot buffer + timestamp ---------------------------------------- */

struct slot {
	void *buf;
	uint64_t submit_ns;
	int in_use;
};

int main(int argc, char **argv)
{
	struct cfg c = {
		.path = getenv("BENCH_FILE") ? getenv("BENCH_FILE") : "/bench.dat",
		.mode = RW_RANDREAD,
		.qd = 32,
		.bs = 4096,
		.size = 1024ULL * 1024 * 1024,
		.runtime = 0,
		.direct = 0,
	};

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--file") && i + 1 < argc)
			c.path = argv[++i];
		else if (!strcmp(argv[i], "--rw") && i + 1 < argc) {
			const char *m = argv[++i];
			if (!strcmp(m, "randread")) c.mode = RW_RANDREAD;
			else if (!strcmp(m, "randwrite")) c.mode = RW_RANDWRITE;
			else if (!strcmp(m, "read")) c.mode = RW_READ;
			else if (!strcmp(m, "write")) c.mode = RW_WRITE;
			else { fprintf(stderr, "bad --rw %s\n", m); return 2; }
		} else if (!strcmp(argv[i], "--qd") && i + 1 < argc)
			c.qd = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--bs") && i + 1 < argc)
			c.bs = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--size") && i + 1 < argc)
			c.size = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--runtime") && i + 1 < argc)
			c.runtime = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--direct"))
			c.direct = 1;
		else {
			fprintf(stderr, "unknown arg: %s\n", argv[i]);
			return 2;
		}
	}

	if (c.qd == 0 || c.qd > 4096) {
		fprintf(stderr, "qd out of range\n");
		return 2;
	}
	if (c.bs == 0) {
		fprintf(stderr, "bs must be > 0\n");
		return 2;
	}

	rng_state = now_ns() ^ 0xdeadbeefcafef00dULL;

	/* Open target. For write modes, create/extend; for read, must exist. */
	int oflags = is_write(c.mode) ? (O_RDWR | O_CREAT) : O_RDONLY;
	if (c.direct)
		oflags |= O_DIRECT;
	int tfd = open(c.path, oflags, 0644);
	if (tfd < 0) {
		fprintf(stderr, "open(%s): %s\n", c.path, strerror(errno));
		return 1;
	}

	/* Determine usable byte range. */
	uint64_t dev_bytes = 0;
	struct stat st;
	if (fstat(tfd, &st) == 0) {
		if (S_ISBLK(st.st_mode)) {
#ifdef BLKGETSIZE64
			if (ioctl(tfd, BLKGETSIZE64, &dev_bytes) != 0)
				dev_bytes = 0;
#endif
		} else {
			dev_bytes = (uint64_t)st.st_size;
		}
	}

	/* For write to a regular file, grow it to `size` so offsets are valid. */
	if (is_write(c.mode) && !S_ISBLK(st.st_mode)) {
		if (ftruncate(tfd, (off_t)c.size) != 0) {
			fprintf(stderr, "ftruncate(%llu): %s\n",
				(unsigned long long)c.size, strerror(errno));
			close(tfd);
			return 1;
		}
		dev_bytes = c.size;
	}

	/* Clamp the working set to what the target actually holds. */
	uint64_t span = c.size;
	if (dev_bytes > 0 && dev_bytes < span)
		span = dev_bytes;
	if (span < c.bs) {
		fprintf(stderr, "target too small: span=%llu bs=%u\n",
			(unsigned long long)span, c.bs);
		close(tfd);
		return 1;
	}
	uint64_t nblocks = span / c.bs;
	uint64_t total_ios = nblocks; /* one pass over the span when size-bound */

	struct ring r;
	memset(&r, 0, sizeof(r));
	if (ring_setup(&r, c.qd) != 0) {
		fprintf(stderr, "io_uring_setup: %s\n", strerror(errno));
		close(tfd);
		return 1;
	}

	/* Per-slot aligned buffers (align helps O_DIRECT and is harmless else). */
	unsigned nslots = c.qd;
	struct slot *slots = calloc(nslots, sizeof(*slots));
	if (!slots) { perror("calloc"); return 1; }
	for (unsigned i = 0; i < nslots; i++) {
		if (posix_memalign(&slots[i].buf, 4096, c.bs) != 0) {
			fprintf(stderr, "posix_memalign\n");
			return 1;
		}
		if (is_write(c.mode))
			memset(slots[i].buf, 0xa5, c.bs);
	}

	uint64_t seq_block = 0;   /* next sequential block index */
	uint64_t submitted = 0;   /* ops submitted so far */
	uint64_t completed = 0;   /* ops completed so far */
	uint64_t bytes_done = 0;
	uint64_t errors = 0;
	unsigned inflight = 0;

	uint64_t start_ns = now_ns();
	uint64_t deadline_ns = c.runtime ? start_ns + (uint64_t)c.runtime * 1000000000ULL : 0;

	int stop_submitting = 0;

	while (completed < submitted || !stop_submitting) {
		/* Decide whether we are done issuing new work. */
		if (!stop_submitting) {
			if (c.runtime) {
				if (now_ns() >= deadline_ns)
					stop_submitting = 1;
			} else if (submitted >= total_ios) {
				stop_submitting = 1;
			}
		}

		/* Fill the SQ with new ops while slots are free. */
		unsigned to_submit = 0;
		unsigned tail = *r.sq_tail;
		while (!stop_submitting && inflight < c.qd) {
			/* find a free slot */
			unsigned s = (unsigned)-1;
			for (unsigned i = 0; i < nslots; i++)
				if (!slots[i].in_use) { s = i; break; }
			if (s == (unsigned)-1)
				break;

			uint64_t blk;
			if (is_random(c.mode))
				blk = rng_next() % nblocks;
			else
				blk = seq_block++ % nblocks;

			unsigned idx = tail & *r.sq_mask;
			struct app_io_uring_sqe *sqe = &r.sqes[idx];
			memset(sqe, 0, sizeof(*sqe));
			sqe->opcode = is_write(c.mode) ? APP_IORING_OP_WRITE
						       : APP_IORING_OP_READ;
			sqe->fd = tfd;
			sqe->off = blk * c.bs;
			sqe->addr = (uint64_t)(uintptr_t)slots[s].buf;
			sqe->len = c.bs;
			sqe->user_data = s;
			r.sq_array[idx] = idx;

			slots[s].in_use = 1;
			slots[s].submit_ns = now_ns();
			tail++;
			to_submit++;
			inflight++;
			submitted++;

			if (!c.runtime && submitted >= total_ios) {
				stop_submitting = 1;
				break;
			}
		}

		if (to_submit) {
			__atomic_store_n(r.sq_tail, tail, __ATOMIC_RELEASE);
		}

		/* Enter the kernel: submit new SQEs and, if the ring is full or
		 * we are draining, block until at least one CQE is ready. */
		unsigned min_complete = 0;
		if (inflight >= c.qd || (stop_submitting && inflight > 0))
			min_complete = 1;

		if (to_submit || min_complete) {
			long ret = syscall(__NR_io_uring_enter, r.fd, to_submit,
					   min_complete,
					   min_complete ? APP_IORING_ENTER_GETEVENTS : 0,
					   NULL, 0);
			if (ret < 0) {
				if (errno == EINTR)
					continue;
				fprintf(stderr, "io_uring_enter: %s\n",
					strerror(errno));
				break;
			}
		}

		/* Reap all available completions. */
		unsigned chead = *r.cq_head;
		unsigned ctail = __atomic_load_n(r.cq_tail, __ATOMIC_ACQUIRE);
		uint64_t done_ns = now_ns();
		while (chead != ctail) {
			struct app_io_uring_cqe *cqe =
				&r.cqes[chead & *r.cq_mask];
			unsigned s = (unsigned)cqe->user_data;
			if (cqe->res < 0) {
				errors++;
			} else {
				bytes_done += (uint64_t)cqe->res;
				uint64_t lat_us =
					(done_ns - slots[s].submit_ns) / 1000;
				hist[lat_bucket(lat_us)]++;
			}
			if (s < nslots)
				slots[s].in_use = 0;
			inflight--;
			completed++;
			chead++;
		}
		__atomic_store_n(r.cq_head, chead, __ATOMIC_RELEASE);

		if (stop_submitting && inflight == 0)
			break;
	}

	uint64_t end_ns = now_ns();
	double elapsed_s = (double)(end_ns - start_ns) / 1e9;
	if (elapsed_s <= 0)
		elapsed_s = 1e-9;

	uint64_t ok_ios = completed - errors;
	double iops = (double)ok_ios / elapsed_s;
	double mbps = ((double)bytes_done / (1024.0 * 1024.0)) / elapsed_s;

	uint64_t p50 = percentile(ok_ios, 0.50);
	uint64_t p99 = percentile(ok_ios, 0.99);
	uint64_t p999 = percentile(ok_ios, 0.999);

	struct buf out = {0};
	buf_lit(&out, "{\"io_uring_bench\":{");
	buf_lit(&out, "\"file\":");
	buf_json_str(&out, c.path);
	buf_lit(&out, ",\"rw\":");
	buf_json_str(&out, mode_name(c.mode));
	buf_printf(&out, ",\"qd\":%u,\"bs\":%u", c.qd, c.bs);
	buf_printf(&out, ",\"direct\":%s", c.direct ? "true" : "false");
	buf_printf(&out, ",\"span_bytes\":%llu", (unsigned long long)span);
	buf_printf(&out, ",\"runtime_cfg_s\":%u", c.runtime);
	buf_printf(&out, ",\"elapsed_s\":%.6f", elapsed_s);
	buf_printf(&out, ",\"ios_completed\":%llu", (unsigned long long)completed);
	buf_printf(&out, ",\"ios_ok\":%llu", (unsigned long long)ok_ios);
	buf_printf(&out, ",\"errors\":%llu", (unsigned long long)errors);
	buf_printf(&out, ",\"bytes\":%llu", (unsigned long long)bytes_done);
	buf_printf(&out, ",\"iops\":%.1f", iops);
	buf_printf(&out, ",\"mb_per_s\":%.2f", mbps);
	buf_printf(&out, ",\"lat_us_p50\":%llu", (unsigned long long)p50);
	buf_printf(&out, ",\"lat_us_p99\":%llu", (unsigned long long)p99);
	buf_printf(&out, ",\"lat_us_p999\":%llu", (unsigned long long)p999);
	const char *status = (ok_ios > 0 && errors == 0) ? "ok"
			   : (ok_ios > 0 ? "degraded" : "fail");
	buf_lit(&out, ",\"status\":");
	buf_json_str(&out, status);
	buf_lit(&out, "}}");
	printf("%s\n", out.p);
	fflush(stdout);
	free(out.p);

	for (unsigned i = 0; i < nslots; i++)
		free(slots[i].buf);
	free(slots);
	munmap(r.sq_ptr, r.sq_sz);
	munmap(r.cq_ptr, r.cq_sz);
	munmap(r.sqe_ptr, r.sqe_sz);
	close(r.fd);
	close(tfd);

	return status[0] == 'f' ? 1 : 0;
}
