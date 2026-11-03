// local telemetry: CPU, RAM, GPU, video engines, temps
#pragma once
#include <atomic>
#include <string>
#include <thread>

#include "wire.hpp"

namespace c2c {

class SysInfoCollector {
public:
    SysInfoCollector();
    ~SysInfoCollector();
    void sample(SysInfo& out);  // cheap, just a few sysfs/procfs reads

private:
    // GPU load in sysfs only covers the last few tens of ms. Read it once per
    // telemetry period and bursty CUDA work mostly shows up as 0%. So a small
    // thread reads it every 20 ms and sample() returns the average since the
    // last call.
    void gpu_sampler();
    std::thread gpu_thr_;
    std::atomic<bool> gpu_stop_{false};
    std::atomic<unsigned long long> gpu_sum_{0}, gpu_n_{0};
    unsigned long long prev_busy_ = 0, prev_total_ = 0;
    std::string gpu_load_path_, gpu_freq_path_;
    std::string cpu_temp_path_, gpu_temp_path_;
    SysInfo static_{};
};

std::string format_sysinfo(const SysInfo& s, const char* indent);
std::string sysinfo_json(const SysInfo& s);

}  // namespace c2c
