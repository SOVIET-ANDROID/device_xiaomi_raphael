/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "GpuStateResidencyDataProvider.h"

#include <android-base/logging.h>

#include <fstream>
#include <sstream>

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace stats {

// SKU 0 fallback (speed-bin=0): 770, 715, 615, 515, 340 MHz
// Used when gpu_available_frequencies is unreadable at construction time.
static constexpr uint32_t kFallbackFreqsMhz[] = {770, 715, 615, 515, 340};

// -----------------------------------------------------------------------
// Construction — cache the frequency list once so getInfo() and
// getStateResidencies() always see the same state count / IDs.
// -----------------------------------------------------------------------
GpuStateResidencyDataProvider::GpuStateResidencyDataProvider() {
    mFrequencies = readAvailableFrequenciesMhz();

    if (mFrequencies.empty()) {
        LOG(WARNING) << "GpuStateResidencyDataProvider: could not read "
                        "gpu_available_frequencies; using SKU-0 fallback table.";
        for (uint32_t f : kFallbackFreqsMhz) {
            mFrequencies.push_back(f);
        }
    }

    LOG(INFO) << "GpuStateResidencyDataProvider: registered " << mFrequencies.size()
              << " active frequency states + 1 Suspend state.";
}

// -----------------------------------------------------------------------
// readAvailableFrequenciesMhz
//
// The kernel lists frequencies high-to-low, space-separated, with a 0 Hz
// placeholder at the end (the "off" OPP).  We convert Hz → MHz and drop
// the 0 Hz entry because:
//   • gpu_clock_stats reports one counter per *active* OPP, not for off.
//   • Suspend time is reported separately via suspend_time.
// -----------------------------------------------------------------------
/*static*/ std::vector<uint32_t>
GpuStateResidencyDataProvider::readAvailableFrequenciesMhz() {
    std::ifstream in("/sys/class/kgsl/kgsl-3d0/gpu_available_frequencies",
                     std::ifstream::in);
    if (!in.is_open()) {
        PLOG(ERROR) << "GpuStateResidencyDataProvider: failed to open "
                       "gpu_available_frequencies";
        return {};
    }

    std::vector<uint32_t> freqs;
    uint32_t hz = 0;
    while (in >> hz) {
        if (hz == 0) continue;  // drop the off placeholder
        freqs.push_back(hz / 1000000u);
    }
    return freqs;
}

// -----------------------------------------------------------------------
// getInfo — called once at HAL registration.
// Returns the cached frequency list + one trailing "Suspend" state.
// -----------------------------------------------------------------------
std::unordered_map<std::string, std::vector<State>>
GpuStateResidencyDataProvider::getInfo() {
    std::vector<State> states;
    states.reserve(mFrequencies.size() + 1);

    int32_t id = 0;
    for (uint32_t mhz : mFrequencies) {
        states.push_back({.id = id++, .name = std::to_string(mhz) + "MHz"});
    }
    states.push_back({.id = id, .name = "Suspend"});

    return {{"GPU", states}};
}

// -----------------------------------------------------------------------
// getStateResidencies — called periodically by the framework.
//
// gpu_clock_stats contains one uint64 per active OPP in the same
// high-to-low order as gpu_available_frequencies (without the 0 Hz entry).
// suspend_time contains a single uint64 in milliseconds.
//
// On any read failure we return a zero-filled result rather than returning
// false/nullopt, because the framework will crash (ArrayIndexOutOfBounds in
// system_server) if a declared entity yields no result.
// -----------------------------------------------------------------------
bool GpuStateResidencyDataProvider::getStateResidencies(
        std::unordered_map<std::string, std::vector<StateResidency>> *results) {

    const int32_t suspendId = static_cast<int32_t>(mFrequencies.size());

    // Pre-populate with zeroes so we always return a full result vector.
    std::vector<StateResidency> residencies;
    residencies.reserve(mFrequencies.size() + 1);
    for (int32_t i = 0; i <= suspendId; ++i) {
        residencies.push_back({.id = i, .totalTimeInStateMs = 0,
                               .totalStateEntryCount = 0,
                               .lastEntryTimestampMs = 0});
    }

    // --- Active frequency residencies ---
    {
        std::ifstream in("/sys/class/kgsl/kgsl-3d0/gpu_clock_stats",
                         std::ifstream::in);
        if (!in.is_open()) {
            LOG(ERROR) << "GpuStateResidencyDataProvider: failed to open gpu_clock_stats; "
                          "returning zero residencies.";
            results->emplace("GPU", residencies);
            return false;
        }

        int32_t idx = 0;
        uint64_t timeMs = 0;
        while (in >> timeMs && idx < static_cast<int32_t>(mFrequencies.size())) {
            residencies[idx].totalTimeInStateMs = static_cast<int64_t>(timeMs);
            ++idx;
        }

        // If the kernel reported fewer entries than we have states the leftover
        // states stay zero — not ideal, but safe.
        if (idx < static_cast<int32_t>(mFrequencies.size())) {
            LOG(WARNING) << "GpuStateResidencyDataProvider: gpu_clock_stats had fewer "
                            "entries (" << idx << ") than registered states ("
                         << mFrequencies.size() << ").";
        }
    }

    // --- Suspend residency ---
    {
        std::ifstream in("/sys/class/kgsl/kgsl-3d0/devfreq/suspend_time",
                         std::ifstream::in);
        if (!in.is_open()) {
            LOG(WARNING) << "GpuStateResidencyDataProvider: failed to open suspend_time; "
                            "Suspend state will read as 0.";
        } else {
            uint64_t suspendMs = 0;
            if (in >> suspendMs) {
                residencies[suspendId].totalTimeInStateMs = static_cast<int64_t>(suspendMs);
            }
        }
    }

    results->emplace("GPU", std::move(residencies));
    return true;
}

}  // namespace stats
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
