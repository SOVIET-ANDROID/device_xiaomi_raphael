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

#include <utils/Looper.h>

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
// 90Hz vs 60Hz threshold:
//   The Amoled panel (Samsung / Tianma) runs at ~1100 MHz DSI clock at 60Hz
//   and ~1300 MHz at 90Hz.  We use 1200 MHz (1_200_000_000 Hz) as the
//   split point, which sits safely between both known operating points.
//
// A background thread polls via Looper/epoll on the bl_power fd so state
// transitions are captured promptly without busy-waiting.
//
// Thread safety: mResidencies and mCurState are protected by mLock.
// All public methods are safe to call from any thread.
//
// Robustness: if bl_power cannot be opened the provider still functions —
// getStateResidencies() returns a zeroed result rather than crashing.
class DisplayStateResidencyDataProvider : public IStateResidencyDataProvider {
  public:
    // State IDs — must match getInfo() order.
    static constexpr int32_t STATE_OFF   = 0;
    static constexpr int32_t STATE_60HZ  = 1;
    static constexpr int32_t STATE_90HZ  = 2;
    static constexpr int32_t NUM_STATES  = 3;

    // DSI clock threshold in Hz.  Clocks above this value are classified as
    // 90 Hz; at or below are classified as 60 Hz.
    static constexpr uint64_t kDsiClockThresholdHz = 1200000000ULL;

    DisplayStateResidencyDataProvider(const std::string &blPath,
                                      const std::string &clkPath);
    ~DisplayStateResidencyDataProvider();

    bool getStateResidencies(
            std::unordered_map<std::string, std::vector<StateResidency>> *results) override;
    std::unordered_map<std::string, std::vector<State>> getInfo() override;

  private:
    // Reads current bl_power + DSI clock and updates mResidencies / mCurState.
    // Must be called from the poll thread OR with the lock NOT held (it acquires it).
    void updateStats();

    // Background thread entry point.
    void pollLoop();

    // Returns current boot-clock time in milliseconds.
    static uint64_t nowMs();

    const std::string mBlPath;
    const std::string mClkPath;

    int mBlFd;  // fd for bl_power, -1 if open failed

    mutable std::mutex mLock;
    std::vector<StateResidency> mResidencies;  // guarded by mLock
    int32_t mCurState;                         // guarded by mLock; -1 = unknown

    ::android::sp<::android::Looper> mLooper;
    std::atomic<bool> mRunThread;
    std::thread mThread;
};

}  // namespace stats
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
