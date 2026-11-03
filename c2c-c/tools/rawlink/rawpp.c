// rawpp: round trip + throughput from user space over a raw c2c char device.
//   slave:  rawpp echo  /dev/c2c0            (echoes every message back)
//   master: rawpp ping  /dev/c2c0 [size] [iters] [spin_us]
//           rawpp tput  /dev/c2c0 [MiB]      (master->slave w/ slave counting, then the other way)
// On /dev/c2c0 the master switches the gadget to STREAM itself, so stop c2cd first.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "../../common/c2c_proto.h"
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e6+t.tv_nsec/1e3;}
static int spin_us = 0;
// read exactly n bytes. spins (O_NONBLOCK) for spin_us before falling back to poll
static int rd(int fd, void *b, size_t n) {
    size_t got = 0; double t0 = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)b + got, n - got);
        if (r > 0) { got += r; t0 = 0; continue; }
        if (r < 0 && errno != EAGAIN && errno != EINTR) return -1;
        if (r == 0) return -1;
        if (spin_us) { if (!t0) t0 = now(); if (now() - t0 < spin_us) continue; }
        struct pollfd p = {fd, POLLIN, 0}; poll(&p, 1, 1000);
    }
    return 0;
}
static int wr(int fd, const void *b, size_t n) {
    size_t d = 0;
    while (d < n) { ssize_t r = write(fd, (const char *)b + d, n - d);
        if (r < 0) { if (errno == EAGAIN || errno == EINTR) { struct pollfd p = {fd, POLLOUT, 0}; poll(&p, 1, 1000); continue; } return -1; }
        d += r; }
    return 0;
}
// channels only: wait till the master opens a session (ACCEPT is non-blocking here)
static int accept_session(int fd) {
    for (;;) {
        if (ioctl(fd, C2C_IOC_ACCEPT) == 0) return 0;
        if (errno != EAGAIN) return -1;
        struct pollfd p = {fd, POLLIN, 0}; poll(&p, 1, 1000);
    }
}
static int cmpd(const void *a, const void *b){double x=*(double*)a,y=*(double*)b;return x<y?-1:x>y;}
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: rawpp echo|ping|tput DEV [args]\n"); return 2; }
    const char *mode = argv[1];
    int fd = open(argv[2], O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror(argv[2]); return 1; }
    static char buf[4 << 20];
    int chan = strstr(argv[2], "c2cch") != NULL;
    if (!strcmp(mode, "echo")) {
        spin_us = argc > 3 ? atoi(argv[3]) : 0;
        uint32_t len;
        if (chan && accept_session(fd)) { perror("ACCEPT"); return 1; }
        for (;;) {
            if (rd(fd, &len, 4)) {
                if (chan && errno == ECONNRESET) { if (accept_session(fd)) { perror("ACCEPT"); return 1; } continue; }
                fprintf(stderr, "echo: read failed: %s\n", strerror(errno)); usleep(100000); continue; }
            if (len == 0xFFFFFFFF) { // tput mode: next u64 is the total byte count
                uint64_t tot; rd(fd, &tot, 8); uint64_t got = 0; double t0 = now();
                while (got < tot) { size_t n = tot - got > sizeof buf ? sizeof buf : tot - got; rd(fd, buf, n); got += n; }
                double dt = now() - t0; wr(fd, &dt, 8);
                // then send the same amount back
                rd(fd, &tot, 8); for (uint64_t s = 0; s < tot;) { size_t n = tot - s > (1 << 20) ? (1 << 20) : tot - s; wr(fd, buf, n); s += n; }
                continue;
            }
            if (len > sizeof buf) { fprintf(stderr, "bad len %u\n", len); return 1; }
            rd(fd, buf, len); wr(fd, &len, 4); wr(fd, buf, len);
        }
    }
    uint32_t m = C2C_MODE_STREAM;   /* channels already started a session at open() */
    if (!chan && ioctl(fd, C2C_IOC_SET_MODE, &m)) { perror("SET_MODE"); return 1; }
    if (!strcmp(mode, "ping")) {
        uint32_t size = argc > 3 ? atoi(argv[3]) : 16; int iters = argc > 4 ? atoi(argv[4]) : 5000;
        spin_us = argc > 5 ? atoi(argv[5]) : 0;
        double *lat = malloc(sizeof(double) * iters);
        memset(buf, 0x5a, size);
        for (int i = -100; i < iters; i++) {
            // header + payload in one write so it's one USB transfer
            static char msg[4 << 20]; memcpy(msg, &size, 4); memcpy(msg + 4, buf, size);
            double t0 = now(); wr(fd, msg, 4 + size); uint32_t l; rd(fd, &l, 4); rd(fd, buf, l); double dt = now() - t0;
            if (i >= 0) lat[i] = dt;
        }
        qsort(lat, iters, sizeof(double), cmpd);
        double s = 0; for (int i = 0; i < iters; i++) s += lat[i];
        printf("size %u spin %d us: avg %.1f us  min %.1f  p50 %.1f  p99 %.1f\n", size, spin_us, s / iters, lat[0], lat[iters / 2], lat[iters * 99 / 100]);
        return 0;
    }
    if (!strcmp(mode, "tput")) {
        uint64_t tot = (uint64_t)(argc > 3 ? atoi(argv[3]) : 1024) << 20; uint32_t mark = 0xFFFFFFFF;
        wr(fd, &mark, 4); wr(fd, &tot, 8); double t0 = now();
        for (uint64_t s = 0; s < tot;) { size_t n = tot - s > (1 << 20) ? (1 << 20) : tot - s; wr(fd, buf, n); s += n; }
        double dt; rd(fd, &dt, 8); double t1 = now();
        printf("master->slave: %.1f MB/s (slave rx window %.1f MB/s)\n", tot / (t1 - t0), tot / dt);
        wr(fd, &tot, 8); t0 = now();
        for (uint64_t g = 0; g < tot;) { size_t n = tot - g > sizeof buf ? sizeof buf : tot - g; rd(fd, buf, n); g += n; }
        printf("slave->master: %.1f MB/s\n", tot / (now() - t0));
        return 0;
    }
    return 2;
}
