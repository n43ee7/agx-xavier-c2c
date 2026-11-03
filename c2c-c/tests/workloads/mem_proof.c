// mem_proof - dumb memory hog for the remote RAM tests. Knows nothing about C2C,
// that's the point.
//
//   mem_proof -g GiB [-s seed] [-q]
//
// Grabs GiB of anonymous memory, fills each 8-byte word with a pseudo-random
// value from its index (incompressible, so zram can't cheat), notes the
// footprint, then reads it all back and checks it. One JSON line on stdout.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static inline uint64_t mix(uint64_t x) {   // splitmix64 finalizer
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static long status_kb(const char* key) {
    FILE* f = fopen("/proc/self/status", "r");
    char line[256];
    long v = -1;
    size_t kl = strlen(key);
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == ':') { v = atol(line + kl + 1); break; }
    if (f) fclose(f);
    return v;
}

int main(int argc, char** argv) {
    double gib = 1;
    uint64_t seed = 0xC2C;
    int quiet = 0, opt;
    while ((opt = getopt(argc, argv, "g:s:q")) != -1) {
        if (opt == 'g') gib = atof(optarg);
        else if (opt == 's') seed = strtoull(optarg, 0, 0);
        else if (opt == 'q') quiet = 1;
        else { fprintf(stderr, "usage: %s -g GiB [-s seed] [-q]\n", argv[0]); return 2; }
    }
    size_t bytes = (size_t)(gib * 1073741824.0) & ~(size_t)4095;
    size_t words = bytes / 8;
    uint64_t* m = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }

    const size_t step = 1UL << 27;   // progress print every 1 GiB
    double t0 = now();
    long peak_rss = 0, peak_swap = 0;
    for (size_t i = 0; i < words; ++i) {
        m[i] = mix(i ^ seed);
        if ((i & (step - 1)) == step - 1) {
            long r = status_kb("VmRSS"), s = status_kb("VmSwap");
            if (r > peak_rss) peak_rss = r;
            if (s > peak_swap) peak_swap = s;
            if (!quiet) fprintf(stderr, "  write %5.1f / %.1f GiB  rss %.1f GiB  swapped %.1f GiB  %.0f s\n",
                                (i + 1) * 8 / 1073741824.0, gib, r / 1048576.0, s / 1048576.0, now() - t0);
        }
    }
    double t1 = now();
    long rss_w = status_kb("VmRSS"), swap_w = status_kb("VmSwap");
    if (rss_w > peak_rss) peak_rss = rss_w;
    if (swap_w > peak_swap) peak_swap = swap_w;

    size_t bad = 0;
    for (size_t i = 0; i < words; ++i) {
        if (m[i] != mix(i ^ seed)) ++bad;
        if (!quiet && (i & (step - 1)) == step - 1)
            fprintf(stderr, "  verify %5.1f / %.1f GiB  errors %zu  %.0f s\n",
                    (i + 1) * 8 / 1073741824.0, gib, bad, now() - t1);
    }
    double t2 = now();
    long swap_v = status_kb("VmSwap");
    if (swap_v > peak_swap) peak_swap = swap_v;

    printf("{\"gib\":%.3f,\"bytes\":%zu,\"write_s\":%.2f,\"verify_s\":%.2f,"
           "\"write_gib_s\":%.3f,\"verify_gib_s\":%.3f,\"errors\":%zu,"
           "\"rss_after_write_kb\":%ld,\"swap_after_write_kb\":%ld,"
           "\"peak_rss_kb\":%ld,\"peak_swap_kb\":%ld}\n",
           bytes / 1073741824.0, bytes, t1 - t0, t2 - t1, bytes / 1073741824.0 / (t1 - t0),
           bytes / 1073741824.0 / (t2 - t1), bad, rss_w, swap_w, peak_rss, peak_swap);
    return bad ? 3 : 0;
}
