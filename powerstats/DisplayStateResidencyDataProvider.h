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

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace stats {

// Tracks display state residency (Off / 60Hz / 90Hz) for Xiaomi Raphael.
//
// State detection uses two sysfs nodes:
//   mBlPath  — /sys/class/backlight/panel0-backlight/bl_power
//              Kernel FB_BLANK convention: 0 = display ON, non-zero = display OFF.
//   mClkPath — /sys/devices/platform/soc/soc:qcom,dsi-display-primary/dynamic_dsi_clock
//              Reports the current DSI link clock in Hz.
//
// Poll mechanism — timed polling with value-change detection:
//   The background thread wakes every kPollIntervalMs (200ms), reads bl_power,
//   and calls updateStats() ONLY when the value changes from the previous read.
//
//   This is the only reliable approach on Raphael because:
//     - EPOLLIN (Looper) is level-triggered on sysfs: always "readable", causes
//       100% CPU spin (confirmed: thread 1372 at 100% one core continuously).
//     - POLLPRI requires the driver to call sysfs_notify() on value change.
//       The Raphael CAF 4.14 backlight driver does NOT call sysfs_notify(),
//       so poll(POLLPRI) times out every time and misses all transitions.
//     - Timed polling correctly detects all transitions with ≤200ms latency
//       at ~0.01% CPU overhead (1 pread() per 200ms).
//
// Thread safety: mResidencies and mCurState are protected by mLock.
class DisplayStateResidencyDataProvider : public IStateResidencyDataProvider {
  public:
    static constexpr int32_t STATE_OFF   = 0;
    static constexpr int32_t STATE_60HZ  = 1;
    static constexpr int32_t STATE_90HZ  = 2;
    static constexpr int32_t NUM_STATES  = 3;

    // DSI clock threshold separating 60 Hz from 90 Hz.
    static constexpr uint64_t kDsiClockThresholdHz = 1200000000ULL;

    // Sampling interval. 200ms gives good accuracy at negligible CPU cost.
    static constexpr int kPollIntervalMs = 200;

    DisplayStateResidencyDataProvider(const std::string &blPath,
                                      const std::string &clkPath);
    ~DisplayStateResidencyDataProvider();

    bool getStateResidencies(
            std::unordered_map<std::string, std::vector<StateResidency>> *results) override;
    std::unordered_map<std::string, std::vector<State>> getInfo() override;

  private:
    void updateStats();
    void pollLoop();
    static uint64_t nowMs();

    const std::string mBlPath;
    const std::string mClkPath;

    int mBlFd;          // opened O_RDONLY for pread(); -1 if unavailable
    int mLastBlValue;   // last raw bl_power value; -1 = unread

    mutable std::mutex mLock;
    std::vector<StateResidency> mResidencies;  // guarded by mLock
    int32_t mCurState;                         // guarded by mLock

    std::atomic<bool> mRunThread;
    std::thread mThread;
};

}  // namespace stats
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
