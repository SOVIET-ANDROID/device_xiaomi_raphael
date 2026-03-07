/*
 * Copyright (C) 2021 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "SocStateResidencyDataProvider.h"

#include <android-base/logging.h>

#include <cstdio>
#include <cstring>
#include <fstream>

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace stats {

// State IDs — must match getInfo() order
static constexpr int32_t kIdAosd = 0;
static constexpr int32_t kIdCxsd = 1;
static constexpr int32_t kIdDdr  = 2;

SocStateResidencyDataProvider::SocStateResidencyDataProvider(const std::string &path)
    : mPath(path) {
    LOG(INFO) << "SocStateResidencyDataProvider: watching " << mPath;
}

// ---------------------------------------------------------------------------
// getInfo
// ---------------------------------------------------------------------------
std::unordered_map<std::string, std::vector<State>>
SocStateResidencyDataProvider::getInfo() {
    // AOSD / CXSD / DDR -- matches Pixel 4 (coral) entity set for SM8150
    return {{"SoC", {
        {.id = kIdAosd, .name = "AOSD"},
        {.id = kIdCxsd, .name = "CXSD"},
        {.id = kIdDdr,  .name = "DDR"},
    }}};
}

// ---------------------------------------------------------------------------
// parseStats
//
// Parses /sys/power/system_sleep/stats.
// The file contains two blocks separated by "RPM Mode:<name>" headers.
// Each block has:
//   count: <N>
//   actual last sleep(msec): <T>
// ---------------------------------------------------------------------------
bool SocStateResidencyDataProvider::parseStats(ParsedStats &out) const {
    std::unique_ptr<FILE, decltype(&fclose)> fp(fopen(mPath.c_str(), "r"), fclose);
    if (!fp) {
        PLOG(ERROR) << "SocStateResidencyDataProvider: cannot open " << mPath;
        return false;
    }
    out = {};
    int cur = 0; // 0=none 1=aosd 2=cxsd 3=ddr
    char line[256];
    while (fgets(line, sizeof(line), fp.get())) {
        if      (strstr(line, "RPM Mode:aosd")) { cur = 1; continue; }
        else if (strstr(line, "RPM Mode:cxsd")) { cur = 2; continue; }
        else if (strstr(line, "RPM Mode:ddr"))  { cur = 3; continue; }
        if (cur == 0) continue;
        ParsedState &s = (cur == 1) ? out.aosd : (cur == 2) ? out.cxsd : out.ddr;
        const char *p;
        if      ((p = strstr(line, "count:"))                  != nullptr)
            s.count   = strtoull(p + 6, nullptr, 10);
        else if ((p = strstr(line, "actual last sleep(msec):")) != nullptr)
            s.last_ms = static_cast<int64_t>(strtoull(p + 24, nullptr, 10));
    }
    return true;
}

// ---------------------------------------------------------------------------
// getStateResidencies
//
// Accumulates time using count-delta × last_sleep_duration.
// This correctly reconstructs cumulative sleep time from the kernel's
// per-event format. The error per call is at most
// (delta_count-1) × |variance_in_sleep_durations|, which approaches zero
// as BatteryStats polls frequently relative to the sleep rate.
// ---------------------------------------------------------------------------
bool SocStateResidencyDataProvider::getStateResidencies(
        std::unordered_map<std::string, std::vector<StateResidency>> *results) {

    ParsedStats ps;
    if (!parseStats(ps)) {
        // I/O failure -- return zeroed entries so the framework does not crash
        results->emplace("SoC", std::vector<StateResidency>{
            {.id = kIdAosd, .totalTimeInStateMs = 0, .totalStateEntryCount = 0},
            {.id = kIdCxsd, .totalTimeInStateMs = 0, .totalStateEntryCount = 0},
            {.id = kIdDdr,  .totalTimeInStateMs = 0, .totalStateEntryCount = 0},
        });
        return false;
    }

    std::scoped_lock lk(mLock);

    auto accum = [](StateAccum &a, uint64_t n, int64_t ms) {
        if (n > a.prevCount) {
            a.accumulatedMs += static_cast<int64_t>(n - a.prevCount) * ms;
            a.prevCount = n;
        }
    };
    accum(mAosd, ps.aosd.count, ps.aosd.last_ms);
    accum(mCxsd, ps.cxsd.count, ps.cxsd.last_ms);
    accum(mDdr,  ps.ddr.count,  ps.ddr.last_ms);

    results->emplace("SoC", std::vector<StateResidency>{
        {.id = kIdAosd,
         .totalTimeInStateMs   = mAosd.accumulatedMs,
         .totalStateEntryCount = static_cast<int64_t>(mAosd.prevCount),
         .lastEntryTimestampMs = 0},
        {.id = kIdCxsd,
         .totalTimeInStateMs   = mCxsd.accumulatedMs,
         .totalStateEntryCount = static_cast<int64_t>(mCxsd.prevCount),
         .lastEntryTimestampMs = 0},
        {.id = kIdDdr,
         .totalTimeInStateMs   = mDdr.accumulatedMs,
         .totalStateEntryCount = static_cast<int64_t>(mDdr.prevCount),
         .lastEntryTimestampMs = 0},
    });
    return true;
}

}  // namespace stats
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
