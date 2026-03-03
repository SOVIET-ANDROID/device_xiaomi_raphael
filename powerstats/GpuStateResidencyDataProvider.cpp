/*
 * Copyright (C) 2021 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "GpuStateResidencyDataProvider.h"
#include <android-base/logging.h>
#include <fstream>

namespace aidl::android::hardware::power::stats {

static constexpr uint32_t kFallback[] = {770,715,615,515,340};
static constexpr char kClockStats[]   = "/sys/class/kgsl/kgsl-3d0/gpu_clock_stats";
static constexpr char kSuspendTime[]  = "/sys/class/kgsl/kgsl-3d0/devfreq/suspend_time";
static constexpr char kAvailFreq[]    = "/sys/class/kgsl/kgsl-3d0/gpu_available_frequencies";

// ---------------------------------------------------------------------------
GpuStateResidencyDataProvider::GpuStateResidencyDataProvider() {
    mFrequencies = readAvailableFrequenciesMhz();
    if (mFrequencies.empty()) {
        LOG(WARNING) << "GpuStateResidencyDataProvider: using SKU-0 fallback freqs";
        for (uint32_t f : kFallback) mFrequencies.push_back(f);
    }

    // Baseline: size = freqs + 1 (last slot = Suspend)
    mBaselineMs.resize(mFrequencies.size() + 1, 0);
    std::vector<uint64_t> clk;
    if (readClockStats(clk)) {
        for (size_t i = 0; i < mFrequencies.size() && i < clk.size(); ++i)
            mBaselineMs[i] = clk[i];
    } else {
        LOG(WARNING) << "GpuStateResidencyDataProvider: baseline read failed; "
                        "first-session GPU times may include cross-boot history";
    }
    mBaselineMs[mFrequencies.size()] = readSuspendTime();

    LOG(INFO) << "GpuStateResidencyDataProvider: "
              << mFrequencies.size() << " freq states + Suspend, baselines captured";
}

// ---------------------------------------------------------------------------
/*static*/ std::vector<uint32_t>
GpuStateResidencyDataProvider::readAvailableFrequenciesMhz() {
    std::ifstream in(kAvailFreq);
    if (!in) { PLOG(ERROR) << "cannot open " << kAvailFreq; return {}; }
    std::vector<uint32_t> freqs;
    uint32_t hz = 0;
    while (in >> hz) if (hz) freqs.push_back(hz / 1000000u);
    return freqs;
}

bool GpuStateResidencyDataProvider::readClockStats(std::vector<uint64_t>& out) const {
    std::ifstream in(kClockStats);
    if (!in) { PLOG(ERROR) << "cannot open " << kClockStats; return false; }
    out.clear();
    uint64_t v = 0;
    while (in >> v) out.push_back(v);
    return !out.empty();
}

uint64_t GpuStateResidencyDataProvider::readSuspendTime() const {
    std::ifstream in(kSuspendTime);
    if (!in) { PLOG(WARNING) << "cannot open " << kSuspendTime; return 0; }
    uint64_t v = 0; in >> v; return v;
}

// ---------------------------------------------------------------------------
std::unordered_map<std::string,std::vector<State>>
GpuStateResidencyDataProvider::getInfo() {
    std::vector<State> s;
    s.reserve(mFrequencies.size() + 1);
    int32_t id = 0;
    for (uint32_t mhz : mFrequencies)
        s.push_back({.id = id++, .name = std::to_string(mhz) + "MHz"});
    s.push_back({.id = id, .name = "Suspend"});
    return {{"GPU", s}};
}

// ---------------------------------------------------------------------------
// getStateResidencies — returns (raw - baseline) for each counter.
// Clamps to 0 if raw < baseline (counter reset, e.g. kernel module reload).
// ---------------------------------------------------------------------------
bool GpuStateResidencyDataProvider::getStateResidencies(
        std::unordered_map<std::string,std::vector<StateResidency>>* results) {

    const int32_t suspendId = static_cast<int32_t>(mFrequencies.size());

    auto zeroed = [&]() {
        std::vector<StateResidency> r;
        for (int32_t i = 0; i <= suspendId; ++i)
            r.push_back({.id=i, .totalTimeInStateMs=0,
                         .totalStateEntryCount=0, .lastEntryTimestampMs=0});
        return r;
    };

    std::vector<uint64_t> clk;
    if (!readClockStats(clk)) {
        LOG(ERROR) << "GpuStateResidencyDataProvider: gpu_clock_stats unreadable";
        results->emplace("GPU", zeroed());
        return false;
    }

    if (static_cast<int32_t>(clk.size()) < static_cast<int32_t>(mFrequencies.size())) {
        LOG(WARNING) << "GpuStateResidencyDataProvider: expected "
                     << mFrequencies.size() << " entries, got " << clk.size();
    }

    auto res = zeroed();
    for (int32_t i = 0; i < static_cast<int32_t>(mFrequencies.size()); ++i) {
        if (i < static_cast<int32_t>(clk.size())) {
            uint64_t raw = clk[i], base = mBaselineMs[i];
            res[i].totalTimeInStateMs = static_cast<int64_t>(raw >= base ? raw-base : 0);
        }
    }

    uint64_t suspRaw  = readSuspendTime();
    uint64_t suspBase = mBaselineMs[suspendId];
    res[suspendId].totalTimeInStateMs =
            static_cast<int64_t>(suspRaw >= suspBase ? suspRaw-suspBase : 0);

    results->emplace("GPU", std::move(res));
    return true;
}

} // namespace
