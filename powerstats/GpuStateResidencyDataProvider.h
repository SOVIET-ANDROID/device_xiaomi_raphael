/*
 * Copyright (C) 2021 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "IStateResidencyDataProvider.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace stats {

// Per-frequency + suspend residency for Adreno 640 (Snapdragon 855, Raphael).
//
// SKUs:  0 → 770/715/615/515/340 MHz
//        1 → 692/675/615/515/340 MHz
//
// gpu_clock_stats and suspend_time are monotonic since kernel boot.
// A baseline is captured at construction; all returned values are deltas
// so BatteryStats always sees time-since-HAL-start, not cross-boot totals.
class GpuStateResidencyDataProvider : public IStateResidencyDataProvider {
public:
    GpuStateResidencyDataProvider();
    ~GpuStateResidencyDataProvider() = default;
    bool getStateResidencies(
            std::unordered_map<std::string,std::vector<StateResidency>>*) override;
    std::unordered_map<std::string,std::vector<State>> getInfo() override;
private:
    static std::vector<uint32_t> readAvailableFrequenciesMhz();
    bool readClockStats(std::vector<uint64_t>&) const;
    uint64_t readSuspendTime() const;

    std::vector<uint32_t> mFrequencies;   // MHz, high-to-low
    std::vector<uint64_t> mBaselineMs;    // [0..N-1]=freq baselines, [N]=suspend baseline
};

}  // namespace stats
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
