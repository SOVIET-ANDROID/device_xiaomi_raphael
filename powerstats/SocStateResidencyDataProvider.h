/*
 * Copyright (C) 2021 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "IStateResidencyDataProvider.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aidl::android::hardware::power::stats {

// Tracks SoC-level sleep state residency for Snapdragon 855 (Raphael).
//
// States — matches Pixel 4 / coral (same SM8150 SoC):
//   AOSD  "RPM Mode:aosd"  Active-Only Sleep Domain
//   CXSD  "RPM Mode:cxsd"  CX Sleep Domain
//   DDR   "RPM Mode:ddr"   DDR self-refresh / low-power
//
// Source: /sys/power/system_sleep/stats
// CAF 4.14 format per block:
//   RPM Mode:<n>
//   count: <N>
//   actual last sleep(msec): <T>    <- last event duration only, not cumulative
//
// The kernel exposes no cumulative total. This provider maintains a HAL-side
// accumulator: accumulated_ms += delta_count * last_sleep_ms
class SocStateResidencyDataProvider : public IStateResidencyDataProvider {
public:
    explicit SocStateResidencyDataProvider(const std::string &path);
    ~SocStateResidencyDataProvider() = default;

    bool getStateResidencies(
            std::unordered_map<std::string, std::vector<StateResidency>> *results) override;
    std::unordered_map<std::string, std::vector<State>> getInfo() override;

private:
    struct StateAccum {
        uint64_t prevCount     = 0;
        int64_t  accumulatedMs = 0;
    };

    struct ParsedState {
        uint64_t count   = 0;
        int64_t  last_ms = 0;
    };

    struct ParsedStats {
        ParsedState aosd, cxsd, ddr;
    };

    bool parseStats(ParsedStats &out) const;

    const std::string mPath;

    mutable std::mutex mLock;
    StateAccum mAosd;
    StateAccum mCxsd;
    StateAccum mDdr;
};

} // namespace aidl::android::hardware::power::stats
