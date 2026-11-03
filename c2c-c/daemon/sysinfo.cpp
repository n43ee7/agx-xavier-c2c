#include "sysinfo.hpp"

#include <dirent.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

#include "util.hpp"

namespace c2c {

static bool exists(const std::string& p) { return access(p.c_str(), F_OK) == 0; }

static long long read_ll(const std::string& p, long long dflt = -1) {
    std::string s;
    if (p.empty() || !read_file(p, s)) return dflt;
    return std::atoll(s.c_str());
}

static int count_dev(const char* prefix) {
    int n = 0;
    DIR* d = opendir("/dev");
    if (!d) return 0;
    size_t pl = std::strlen(prefix);
    while (dirent* e = readdir(d))
        if (std::strncmp(e->d_name, prefix, pl) == 0) n++;
    closedir(d);
    return n;
}

static void copy_str(char* dst, size_t n, const std::string& s) {
    std::snprintf(dst, n, "%s", s.c_str());
}

SysInfoCollector::SysInfoCollector() {
    for (const char* p : {"/sys/devices/gpu.0/load",
                          "/sys/devices/17000000.gv11b/load",
                          "/sys/devices/platform/gpu.0/load"})
        if (exists(p)) { gpu_load_path_ = p; break; }
    for (const char* p :
         {"/sys/devices/17000000.gv11b/devfreq/17000000.gv11b/cur_freq",
          "/sys/devices/gpu.0/devfreq/17000000.gv11b/cur_freq"})
        if (exists(p)) { gpu_freq_path_ = p; break; }
    for (int i = 0; i < 32; i++) {
        std::string z = "/sys/devices/virtual/thermal/thermal_zone" +
                        std::to_string(i);
        std::string t;
        if (!read_file(z + "/type", t)) break;
        if (t.rfind("CPU-therm", 0) == 0) cpu_temp_path_ = z + "/temp";
        if (t.rfind("GPU-therm", 0) == 0) gpu_temp_path_ = z + "/temp";
    }

    static_.version = 1;
    char hn[64] = {0};
    gethostname(hn, sizeof(hn) - 1);
    copy_str(static_.host, sizeof(static_.host), hn);

    std::string rel;  // "# R35 (release), REVISION: 6.5, ..."
    if (read_file("/etc/nv_tegra_release", rel)) {
        int major = 0;
        char rev[16] = {0};
        if (std::sscanf(rel.c_str(), "# R%d (release), REVISION: %15[0-9.]",
                        &major, rev) == 2)
            std::snprintf(static_.l4t, sizeof(static_.l4t), "R%d.%s", major, rev);
    }
    std::string cj;  // /usr/local/cuda/version.json: "cuda" : { ..."version" : "11.4.315"
    if (read_file("/usr/local/cuda/version.json", cj)) {
        size_t c = cj.find("\"cuda\"");
        size_t v = c == std::string::npos ? c : cj.find("\"version\"", c);
        if (v != std::string::npos) {
            size_t q1 = cj.find('"', cj.find(':', v) + 1);
            size_t q2 = cj.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
                copy_str(static_.cuda, sizeof(static_.cuda),
                         cj.substr(q1 + 1, q2 - q1 - 1));
        }
    }
    static_.ncpu = uint16_t(sysconf(_SC_NPROCESSORS_CONF));
    static_.nvenc = uint8_t(count_dev("nvhost-msenc"));
    static_.nvdec = uint8_t(count_dev("nvhost-nvdec"));
    static_.nvjpg = uint8_t(count_dev("nvhost-nvjpg"));
    static_.dla = uint8_t(count_dev("nvhost-nvdla"));
    if (!gpu_load_path_.empty()) gpu_thr_ = std::thread(&SysInfoCollector::gpu_sampler, this);
}

SysInfoCollector::~SysInfoCollector() {
    gpu_stop_ = true;
    if (gpu_thr_.joinable()) gpu_thr_.join();
}

void SysInfoCollector::gpu_sampler() {
    while (!gpu_stop_) {
        long long g = read_ll(gpu_load_path_);
        if (g >= 0) {
            gpu_sum_ += (unsigned long long)g;
            gpu_n_ += 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void SysInfoCollector::sample(SysInfo& s) {
    s = static_;
    s.ncpu_online = uint16_t(sysconf(_SC_NPROCESSORS_ONLN));

    std::string st;
    if (read_file("/proc/stat", st)) {
        unsigned long long u, n, sy, id, io, irq, sirq, steal;
        if (std::sscanf(st.c_str(), "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                        &u, &n, &sy, &id, &io, &irq, &sirq, &steal) == 8) {
            unsigned long long busy = u + n + sy + irq + sirq + steal;
            unsigned long long total = busy + id + io;
            if (prev_total_ && total > prev_total_)
                s.cpu_util = 100.0f * float(busy - prev_busy_) /
                             float(total - prev_total_);
            prev_busy_ = busy;
            prev_total_ = total;
        }
    }
    double la[1];
    if (getloadavg(la, 1) == 1) s.load1 = float(la[0]);

    std::string mi;
    if (read_file("/proc/meminfo", mi)) {
        auto field = [&](const char* k) -> uint64_t {
            size_t p = mi.find(k);
            return p == std::string::npos
                       ? 0
                       : std::strtoull(mi.c_str() + p + std::strlen(k), nullptr, 10);
        };
        s.mem_total_kb = field("MemTotal:");
        s.mem_avail_kb = field("MemAvailable:");
        s.swap_total_kb = field("SwapTotal:");
        s.swap_free_kb = field("SwapFree:");
    }
    unsigned long long n = gpu_n_.exchange(0), sum = gpu_sum_.exchange(0);
    if (n) {
        s.gpu_util = float(double(sum) / double(n) / 10.0);
    } else {  // sampler not up yet, just read it directly once
        long long g = read_ll(gpu_load_path_);
        s.gpu_util = g >= 0 ? float(g) / 10.0f : -1.0f;
    }
    long long f = read_ll(gpu_freq_path_);
    s.gpu_mhz = f > 0 ? uint32_t(f / 1000000) : 0;
    long long tc = read_ll(cpu_temp_path_), tg = read_ll(gpu_temp_path_);
    s.temp_cpu = tc > 0 ? float(tc) / 1000.0f : 0;
    s.temp_gpu = tg > 0 ? float(tg) / 1000.0f : 0;
    struct sysinfo si;
    if (sysinfo(&si) == 0) s.uptime_s = uint32_t(si.uptime);
}

std::string format_sysinfo(const SysInfo& s, const char* in) {
    char b[1024];
    std::snprintf(
        b, sizeof(b),
        "%shost      : %s   (L4T %s, CUDA %s, up %uh%02um)\n"
        "%sCPU       : %u cores (%u online), %.1f%% busy, load %.2f, %.1f C\n"
        "%sRAM       : %.1f / %.1f GiB available, swap %.1f / %.1f GiB free\n"
        "%sGPU       : %.1f%% busy @ %u MHz, %.1f C\n"
        "%sengines   : NVENC x%u  NVDEC x%u  NVJPG x%u  DLA x%u\n",
        in, s.host, s.l4t[0] ? s.l4t : "?", s.cuda[0] ? s.cuda : "?",
        s.uptime_s / 3600, (s.uptime_s / 60) % 60, in, s.ncpu, s.ncpu_online,
        s.cpu_util, s.load1, s.temp_cpu, in, s.mem_avail_kb / 1048576.0,
        s.mem_total_kb / 1048576.0, s.swap_free_kb / 1048576.0,
        s.swap_total_kb / 1048576.0, in, s.gpu_util, s.gpu_mhz, s.temp_gpu,
        in, s.nvenc, s.nvdec, s.nvjpg, s.dla);
    return b;
}

std::string sysinfo_json(const SysInfo& s) {
    char b[1024];
    std::snprintf(
        b, sizeof(b),
        "{\"host\":\"%s\",\"l4t\":\"%s\",\"cuda\":\"%s\",\"ncpu\":%u,"
        "\"ncpu_online\":%u,\"cpu_util\":%.1f,\"load1\":%.2f,"
        "\"mem_total_kb\":%llu,\"mem_avail_kb\":%llu,\"swap_total_kb\":%llu,"
        "\"swap_free_kb\":%llu,\"gpu_util\":%.1f,\"gpu_mhz\":%u,"
        "\"temp_cpu\":%.1f,\"temp_gpu\":%.1f,\"nvenc\":%u,\"nvdec\":%u,"
        "\"nvjpg\":%u,\"dla\":%u,\"uptime_s\":%u}",
        s.host, s.l4t, s.cuda, s.ncpu, s.ncpu_online, s.cpu_util, s.load1,
        (unsigned long long)s.mem_total_kb, (unsigned long long)s.mem_avail_kb,
        (unsigned long long)s.swap_total_kb, (unsigned long long)s.swap_free_kb,
        s.gpu_util, s.gpu_mhz, s.temp_cpu, s.temp_gpu, s.nvenc, s.nvdec,
        s.nvjpg, s.dla, s.uptime_s);
    return b;
}

}  // namespace c2c
