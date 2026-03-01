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

#pragma once

#include "IStateResidencyDataProvider.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace stats {

// Provides per-frequency-bin + suspend state residency for the Adreno 640 GPU
// (Snapdragon 855, Xiaomi Raphael / Redmi K20 Pro).
//
// Two SKU frequency tables exist in the device tree:
//   SKU 0 (speed-bin=0): 770, 715, 615, 515, 340 MHz
//   SKU 1 (speed-bin=1): 692, 675, 615, 515, 340 MHz
//
// The kernel exports the active frequency list at runtime via:
//   /sys/class/kgsl/kgsl-3d0/gpu_available_frequencies
// The 0 Hz entry (off placeholder) is intentionally excluded from states.
//
// Residency data comes from:
//   /sys/class/kgsl/kgsl-3d0/gpu_clock_stats   — time in ms per active freq bin
//   /sys/class/kgsl/kgsl-3d0/devfreq/suspend_time — total GPU suspend time in ms
//
// The frequency list is cached at construction time to guarantee that the
// state IDs returned by getInfo() always match those from getStateResidencies().
// A hardcoded SKU-0 fallback is used if the sysfs node is unavailable at init.
class GpuStateResidencyDataProvider : public IStateResidencyDataProvider {
  public:
    GpuStateResidencyDataProvider();
    ~GpuStateResidencyDataProvider() = default;

    bool getStateResidencies(
            std::unordered_map<std::string, std::vector<StateResidency>> *results) override;
    std::unordered_map<std::string, std::vector<State>> getInfo() override;

  private:
    // Reads /sys/class/kgsl/kgsl-3d0/gpu_available_frequencies, strips the 0 Hz
    // placeholder, and returns frequencies in MHz (high-to-low, matching clock_stats order).
    static std::vector<uint32_t> readAvailableFrequenciesMhz();

    // Frequencies in MHz, high-to-low, cached at construction.
    // Index N in mFrequencies corresponds to state ID N.
    // State ID mFrequencies.size() is always "Suspend".
    std::vector<uint32_t> mFrequencies;
};

}  // namespace stats
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
