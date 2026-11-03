// SPDX-License-Identifier: GPL-2.0
/*
 * test_bw_spd: raw bandwidth / latency test for the USB link.
 *
 * Run it on the master (USB host) against /dev/c2c0 (c2c_host.ko), with the
 * slave running the usb_f_c2c gadget. Stop c2cd first, it owns /dev/c2c0.
 *
 *   1. link info              (what speed/packet size/burst we got)
 *   2. raw kernel bench       (no user copies, i.e. what the bus can do)
 *        OUT / IN sweep over URB sizes, then both at once
 *   3. latency ping-pong      (round trip through the gadget)
 *   4. userspace chardev      (write()/read(), copies included)
 *        write->SINK, SOURCE->read, LOOPBACK checking every word
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "c2c_proto.h"

#define MiB (1024.0 * 1024.0)

static const char *devpath = "/dev/c2c0";
static unsigned long long total_mb = 1024;  /* each bench run */
static unsigned int qdepth = 8;
static unsigned int ping_iters = 2000;
static int quick;
static FILE *jf;
static int jfirst = 1;

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void die(const char *what)
{
	fprintf(stderr, "test_bw_spd: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void json(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));
static void json(const char *fmt, ...)
{
	va_list ap;

	if (!jf)
		return;
	fprintf(jf, "%s\n  ", jfirst ? "" : ",");
	jfirst = 0;
	va_start(ap, fmt);
	vfprintf(jf, fmt, ap);
	va_end(ap);
}

static void set_mode(int fd, unsigned int mode)
{
	if (ioctl(fd, C2C_IOC_SET_MODE, &mode))
		die("SET_MODE");
}

static struct c2c_gadget_stats gstats(int fd)
{
	struct c2c_gadget_stats s;

	if (ioctl(fd, C2C_IOC_GET_GSTATS, &s))
		die("GET_GSTATS");
	return s;
}

static double mbps(unsigned long long bytes, unsigned long long ns)
{
	return ns ? bytes / MiB / (ns / 1e9) : 0;
}

static double gbit(unsigned long long bytes, unsigned long long ns)
{
	return ns ? bytes * 8.0 / ns : 0;
}

/* ------------------------------------------------------------------ */

static int bench(int fd, unsigned int dir, unsigned int xfer,
		 struct c2c_bench *res)
{
	memset(res, 0, sizeof(*res));
	res->dir = dir;
	res->xfer_size = xfer;
	res->qdepth = qdepth;
	res->total_bytes = total_mb << 20;
	if (ioctl(fd, C2C_IOC_BENCH, res)) {
		fprintf(stderr, "  bench dir=%u xfer=%u failed: %s\n", dir, xfer,
			strerror(errno));
		return -1;
	}
	return 0;
}

static void kernel_bench(int fd)
{
	static const unsigned int sizes_full[] = {
		16 << 10, 64 << 10, 256 << 10, 1 << 20, 4 << 20 };
	static const unsigned int sizes_quick[] = { 64 << 10, 1 << 20 };
	const unsigned int *sizes = quick ? sizes_quick : sizes_full;
	unsigned int nsizes = quick ? 2 : 5, i, best_xfer = 1 << 20;
	double best = 0;
	struct c2c_bench r;
	struct c2c_gadget_stats g0, g1;

	printf("\n[2] Raw kernel bulk bench  (%llu MiB per run, %u URBs in flight)\n",
	       total_mb, qdepth);
	printf("    %-9s %-8s %10s %10s %9s  %s\n", "dir", "URB", "MiB/s",
	       "Gbit/s", "time s", "gadget-check");

	for (i = 0; i < nsizes; i++) {
		/* host -> gadget */
		set_mode(fd, C2C_MODE_SINK);
		ioctl(fd, C2C_IOC_RESET_GSTATS);
		g0 = gstats(fd);
		if (!bench(fd, C2C_DIR_OUT, sizes[i], &r)) {
			g1 = gstats(fd);
			double v = mbps(r.out_bytes, r.out_ns);
			int ok = g1.rx_bytes - g0.rx_bytes == r.out_bytes;

			printf("    %-9s %-8u %10.1f %10.3f %9.2f  gadget rx %llu B %s\n",
			       "OUT h->d", sizes[i] >> 10, v,
			       gbit(r.out_bytes, r.out_ns), r.out_ns / 1e9,
			       (unsigned long long)(g1.rx_bytes - g0.rx_bytes),
			       ok ? "OK" : "MISMATCH");
			json("{\"test\":\"kernel\",\"dir\":\"out\",\"urb\":%u,\"bytes\":%llu,\"ns\":%llu,\"MiBps\":%.1f,\"gadget_match\":%d}",
			     sizes[i], (unsigned long long)r.out_bytes,
			     (unsigned long long)r.out_ns, v, ok);
			if (v > best) {
				best = v;
				best_xfer = sizes[i];
			}
		}
		/* gadget -> host */
		set_mode(fd, C2C_MODE_SOURCE);
		if (!bench(fd, C2C_DIR_IN, sizes[i], &r)) {
			double v = mbps(r.in_bytes, r.in_ns);

			printf("    %-9s %-8u %10.1f %10.3f %9.2f  errors %u\n",
			       "IN  d->h", sizes[i] >> 10, v,
			       gbit(r.in_bytes, r.in_ns), r.in_ns / 1e9, r.errors);
			json("{\"test\":\"kernel\",\"dir\":\"in\",\"urb\":%u,\"bytes\":%llu,\"ns\":%llu,\"MiBps\":%.1f}",
			     sizes[i], (unsigned long long)r.in_bytes,
			     (unsigned long long)r.in_ns, v);
		}
		set_mode(fd, C2C_MODE_IDLE);
	}

	/* both directions at once, at the best OUT size */
	set_mode(fd, C2C_MODE_BIDIR);
	if (!bench(fd, C2C_DIR_BIDIR, best_xfer, &r)) {
		double vo = mbps(r.out_bytes, r.out_ns);
		double vi = mbps(r.in_bytes, r.in_ns);

		printf("    %-9s %-8u %10.1f %10.3f %9.2f  (OUT %.1f + IN %.1f MiB/s)\n",
		       "BIDIR", best_xfer >> 10, vo + vi,
		       gbit(r.out_bytes, r.out_ns) + gbit(r.in_bytes, r.in_ns),
		       (r.out_ns > r.in_ns ? r.out_ns : r.in_ns) / 1e9, vo, vi);
		json("{\"test\":\"kernel\",\"dir\":\"bidir\",\"urb\":%u,\"out_MiBps\":%.1f,\"in_MiBps\":%.1f}",
		     best_xfer, vo, vi);
	}
	set_mode(fd, C2C_MODE_IDLE);
}

/* ------------------------------------------------------------------ */

static void pingpong(int fd)
{
	static const unsigned int sizes[] = { 8, 512, 1024, 4096, 65536, 1 << 20 };
	unsigned int i;

	printf("\n[3] Round-trip latency  (gadget LOOPBACK, %u iterations, data verified)\n",
	       ping_iters);
	printf("    %-9s %9s %9s %9s %9s %9s %8s\n", "size", "min us", "avg us",
	       "p50 us", "p99 us", "max us", "bad");
	set_mode(fd, C2C_MODE_LOOPBACK);
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		struct c2c_pingpong p = { .size = sizes[i], .iters = ping_iters };

		if (sizes[i] >= (1 << 20))
			p.iters = ping_iters / 10 ? ping_iters / 10 : 1;
		if (ioctl(fd, C2C_IOC_PINGPONG, &p)) {
			printf("    %-9u failed: %s (errors %u)\n", sizes[i],
			       strerror(errno), p.errors);
			set_mode(fd, C2C_MODE_LOOPBACK); /* also drains leftovers */
			continue;
		}
		printf("    %-9u %9.1f %9.1f %9.1f %9.1f %9.1f %8u\n", sizes[i],
		       p.min_ns / 1e3, p.sum_ns / 1e3 / p.iters, p.p50_ns / 1e3,
		       p.p99_ns / 1e3, p.max_ns / 1e3, p.mismatches + p.errors);
		json("{\"test\":\"pingpong\",\"size\":%u,\"iters\":%u,\"min_us\":%.1f,\"avg_us\":%.1f,\"p50_us\":%.1f,\"p99_us\":%.1f,\"max_us\":%.1f,\"bad\":%u}",
		     sizes[i], p.iters, p.min_ns / 1e3, p.sum_ns / 1e3 / p.iters,
		     p.p50_ns / 1e3, p.p99_ns / 1e3, p.max_ns / 1e3,
		     p.mismatches + p.errors);
	}
	set_mode(fd, C2C_MODE_IDLE);
}

/* ------------------------------------------------------------------ */
/* userspace chardev tests                                             */

static inline unsigned long long mix64(unsigned long long x)
{
	x += 0x9E3779B97F4A7C15ULL;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
	return x ^ (x >> 31);
}

/* word i of the test pattern */
static void fill(unsigned long long *w, size_t nwords, unsigned long long first)
{
	size_t i;

	for (i = 0; i < nwords; i++)
		w[i] = mix64(first + i);
}

#define UCHUNK (1u << 20)

struct lb_ctx {
	int fd;
	unsigned long long bytes;
	unsigned long long bad_words;
	unsigned long long got;
	int err;
};

static void *lb_writer(void *arg)
{
	struct lb_ctx *c = arg;
	unsigned long long *buf = aligned_alloc(4096, UCHUNK);
	unsigned long long off;

	for (off = 0; off < c->bytes; off += UCHUNK) {
		fill(buf, UCHUNK / 8, off / 8);
		size_t done = 0;

		while (done < UCHUNK) {
			ssize_t n = write(c->fd, (char *)buf + done, UCHUNK - done);

			if (n < 0) {
				c->err = errno;
				goto out;
			}
			done += n;
		}
	}
	fsync(c->fd);
out:
	free(buf);
	return NULL;
}

static void *lb_reader(void *arg)
{
	struct lb_ctx *c = arg;
	unsigned long long *buf = aligned_alloc(4096, UCHUNK);
	unsigned long long *ref = aligned_alloc(4096, UCHUNK);

	while (c->got < c->bytes) {
		size_t want = UCHUNK, have = 0;

		if (c->bytes - c->got < want)
			want = c->bytes - c->got;
		while (have < want) {
			ssize_t n = read(c->fd, (char *)buf + have, want - have);

			if (n < 0) {
				c->err = errno;
				goto out;
			}
			have += n;
		}
		fill(ref, want / 8, c->got / 8);
		for (size_t i = 0; i < want / 8; i++)
			if (buf[i] != ref[i])
				c->bad_words++;
		c->got += want;
	}
out:
	free(buf);
	free(ref);
	return NULL;
}

static void user_tests(void)
{
	unsigned long long bytes = (quick ? 256ULL : total_mb) << 20;
	char *buf = aligned_alloc(4096, UCHUNK);
	unsigned long long done;
	double t0, dt;
	int fd;

	printf("\n[4] User-space chardev %s  (%llu MiB, %u KiB read/write calls)\n",
	       devpath, bytes >> 20, UCHUNK >> 10);
	memset(buf, 0x5a, UCHUNK);

	/* write() -> gadget SINK */
	fd = open(devpath, O_RDWR);
	if (fd < 0)
		die("open");
	set_mode(fd, C2C_MODE_SINK);
	t0 = now_s();
	for (done = 0; done < bytes;) {
		ssize_t n = write(fd, buf, UCHUNK);

		if (n < 0)
			die("write");
		done += n;
	}
	fsync(fd);
	dt = now_s() - t0;
	printf("    write() -> SINK      %10.1f MiB/s  %8.3f Gbit/s\n",
	       done / MiB / dt, done * 8 / dt / 1e9);
	json("{\"test\":\"user\",\"dir\":\"write\",\"bytes\":%llu,\"MiBps\":%.1f}",
	     done, done / MiB / dt);

	/* gadget SOURCE -> read() */
	set_mode(fd, C2C_MODE_SOURCE);
	t0 = now_s();
	for (done = 0; done < bytes;) {
		ssize_t n = read(fd, buf, UCHUNK);

		if (n < 0)
			die("read");
		done += n;
	}
	dt = now_s() - t0;
	printf("    SOURCE -> read()     %10.1f MiB/s  %8.3f Gbit/s\n",
	       done / MiB / dt, done * 8 / dt / 1e9);
	json("{\"test\":\"user\",\"dir\":\"read\",\"bytes\":%llu,\"MiBps\":%.1f}",
	     done, done / MiB / dt);
	set_mode(fd, C2C_MODE_IDLE);
	close(fd);

	/* LOOPBACK w/ a writer and a reader thread, every word checked */
	fd = open(devpath, O_RDWR);
	if (fd < 0)
		die("open");
	set_mode(fd, C2C_MODE_LOOPBACK);
	{
		struct lb_ctx c = { .fd = fd, .bytes = bytes };
		pthread_t tw, tr;

		t0 = now_s();
		pthread_create(&tr, NULL, lb_reader, &c);
		pthread_create(&tw, NULL, lb_writer, &c);
		pthread_join(tw, NULL);
		pthread_join(tr, NULL);
		dt = now_s() - t0;
		printf("    LOOPBACK w+r verify  %10.1f MiB/s each way  (%llu MiB checked, %llu bad words%s%s)\n",
		       c.got / MiB / dt, c.got >> 20, c.bad_words,
		       c.err ? ", error: " : "", c.err ? strerror(c.err) : "");
		json("{\"test\":\"user\",\"dir\":\"loopback\",\"bytes\":%llu,\"MiBps\":%.1f,\"bad_words\":%llu,\"err\":%d}",
		     c.got, c.got / MiB / dt, c.bad_words, c.err);
	}
	set_mode(fd, C2C_MODE_IDLE);
	close(fd);
	free(buf);
}

/* ------------------------------------------------------------------ */

static void usage(void)
{
	printf("usage: test_bw_spd [-d /dev/c2c0] [-m MiB-per-run] [-q qdepth]\n"
	       "                   [-p ping-iters] [-j results.json] [--quick]\n"
	       "                   [--only kernel|ping|user]\n");
}

int main(int argc, char **argv)
{
	static const struct option lo[] = {
		{ "quick", no_argument, NULL, 'Q' },
		{ "only", required_argument, NULL, 'o' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 } };
	const char *only = NULL, *jpath = NULL;
	struct c2c_info info;
	struct c2c_gadget_stats g;
	int fd, c;

	while ((c = getopt_long(argc, argv, "d:m:q:p:j:h", lo, NULL)) != -1) {
		switch (c) {
		case 'd': devpath = optarg; break;
		case 'm': total_mb = strtoull(optarg, NULL, 0); break;
		case 'q': qdepth = atoi(optarg); break;
		case 'p': ping_iters = atoi(optarg); break;
		case 'j': jpath = optarg; break;
		case 'Q': quick = 1; break;
		case 'o': only = optarg; break;
		default: usage(); return c == 'h' ? 0 : 1;
		}
	}
	if (quick && total_mb > 256)
		total_mb = 256;
	if (jpath) {
		jf = fopen(jpath, "w");
		if (!jf)
			die(jpath);
		fprintf(jf, "[");
	}

	fd = open(devpath, O_RDWR);
	if (fd < 0)
		die(devpath);
	if (ioctl(fd, C2C_IOC_GET_INFO, &info))
		die("GET_INFO");

	/* for debugging the link: only control traffic / IN drains */
	if (only && (!strcmp(only, "ctl") || !strcmp(only, "drain"))) {
		int drain = !strcmp(only, "drain");
		unsigned int i, n = ping_iters;

		for (i = 0; i < n; i++) {
			if (drain)
				set_mode(fd, C2C_MODE_IDLE);
			else if (ioctl(fd, C2C_IOC_RESET_GSTATS))
				die("RESET_GSTATS");
			g = gstats(fd);
			if (i % 50 == 0 || i == n - 1) {
				printf("  %s %u/%u ok\n", only, i + 1, n);
				fflush(stdout);
			}
		}
		return 0;
	}
	set_mode(fd, C2C_MODE_IDLE);
	g = gstats(fd);

	printf("C2C link test  (%s)\n", devpath);
	printf("\n[1] Link\n");
	printf("    negotiated speed : %s (enum %u)\n", info.speed_str, info.speed);
	printf("    bulk IN  mps     : %u bytes, burst %u\n", info.mps_in,
	       info.maxburst + 1);
	printf("    bulk OUT mps     : %u bytes\n", info.mps_out);
	printf("    usb bus/dev      : %u/%u\n", info.busnum, info.devnum);
	printf("    gadget           : %u x %u B requests per endpoint\n",
	       g.qlen, g.buflen);
	json("{\"test\":\"info\",\"speed\":\"%s\",\"mps_in\":%u,\"mps_out\":%u,\"burst\":%u,\"gadget_qlen\":%u,\"gadget_buflen\":%u}",
	     info.speed_str, info.mps_in, info.mps_out, info.maxburst + 1,
	     g.qlen, g.buflen);

	if (!only || !strcmp(only, "kernel"))
		kernel_bench(fd);
	if (!only || !strcmp(only, "ping"))
		pingpong(fd);
	close(fd);
	if (!only || !strcmp(only, "user"))
		user_tests();

	fd = open(devpath, O_RDWR);
	if (fd >= 0) {
		g = gstats(fd);
		printf("\n    gadget totals: rx %.1f MiB (%u errs), tx %.1f MiB (%u errs)\n",
		       g.rx_bytes / MiB, g.rx_errs, g.tx_bytes / MiB, g.tx_errs);
		close(fd);
	}
	if (jf) {
		fprintf(jf, "\n]\n");
		fclose(jf);
	}
	return 0;
}
