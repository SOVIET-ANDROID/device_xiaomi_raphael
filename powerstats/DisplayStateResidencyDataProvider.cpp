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

#include "DisplayStateResidencyDataProvider.h"

#include <android-base/file.h>
#include <android-base/logging.h>

#include <chrono>
#include <fcntl.h>
#include <unistd.h>

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace stats {

// -----------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------
DisplayStateResidencyDataProvider::DisplayStateResidencyDataProvider(
        const std::string &blPath, const std::string &clkPath)
    : mBlPath(blPath),
      mClkPath(clkPath),
      mBlFd(-1),
      mCurState(-1),
      mLooper(new ::android::Looper(true)),
      mRunThread(true) {

    // Initialise residency slots to zero.
    mResidencies.resize(NUM_STATES);
    for (int32_t i = 0; i < NUM_STATES; ++i) {
        mResidencies[i].id = i;
        mResidencies[i].totalTimeInStateMs = 0;
        mResidencies[i].totalStateEntryCount = 0;
        mResidencies[i].lastEntryTimestampMs = 0;
    }

    // Open bl_power for epoll-based change notification.
    mBlFd = open(mBlPath.c_str(), O_RDONLY | O_NONBLOCK);
    if (mBlFd < 0) {
        PLOG(ERROR) << "DisplayStateResidencyDataProvider: failed to open " << mBlPath
                    << ". Display residency will be all-zero until the node is accessible.";
        // Do NOT start the poll thread — mBlFd == -1 is checked in pollLoop / updateStats.
        // getStateResidencies() still returns a valid (zeroed) result, preventing crashes.
        mRunThread = false;
        return;
    }

    mLooper->addFd(mBlFd, 0, ::android::Looper::EVENT_INPUT, nullptr, nullptr);

    // Capture initial state before the thread starts so the first call to
    // getStateResidencies() is not completely stale.
    updateStats();

    mThread = std::thread(&DisplayStateResidencyDataProvider::pollLoop, this);
}

// -----------------------------------------------------------------------
// Destruction
// -----------------------------------------------------------------------
DisplayStateResidencyDataProvider::~DisplayStateResidencyDataProvider() {
    mRunThread = false;
    if (mLooper != nullptr) {
        mLooper->wake();
    }
    if (mThread.joinable()) {
        mThread.join();
    }
    if (mBlFd >= 0) {
        close(mBlFd);
        mBlFd = -1;
    }
}

// -----------------------------------------------------------------------
// getInfo — called once at HAL registration.
// -----------------------------------------------------------------------
std::unordered_map<std::string, std::vector<State>>
DisplayStateResidencyDataProvider::getInfo() {
    return {{"Display", {
        {.id = STATE_OFF,  .name = "Off"},
        {.id = STATE_60HZ, .name = "60Hz"},
        {.id = STATE_90HZ, .name = "90Hz"},
    }}};
}

// -----------------------------------------------------------------------
// getStateResidencies — called by the framework periodically.
//
// We snapshot the running totals under the lock and add the elapsed time
// for the current active state so callers always get up-to-date values
// without requiring the poll thread to have fired recently.
// -----------------------------------------------------------------------
bool DisplayStateResidencyDataProvider::getStateResidencies(
        std::unordered_map<std::string, std::vector<StateResidency>> *results) {

    std::scoped_lock lk(mLock);

    // Guard against mBlFd == -1: return zeroed residencies, don't crash.
    if (mBlFd < 0) {
        results->emplace("Display", mResidencies);
        return false;
    }

    uint64_t now = nowMs();
    std::vector<StateResidency> snapshot = mResidencies;

    // Add time elapsed in the current state since the last transition.
    if (mCurState >= 0 && mCurState < NUM_STATES) {
        snapshot[mCurState].totalTimeInStateMs +=
                static_cast<int64_t>(now - static_cast<uint64_t>(
                        snapshot[mCurState].lastEntryTimestampMs));
    }

    results->emplace("Display", std::move(snapshot));
    return true;
}

// -----------------------------------------------------------------------
// nowMs — monotonic boot-clock time in milliseconds.
// -----------------------------------------------------------------------
/*static*/ uint64_t DisplayStateResidencyDataProvider::nowMs() {
    return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
}

// -----------------------------------------------------------------------
// updateStats — reads current display state and updates mResidencies.
//
// bl_power semantics (kernel FB_BLANK / DRM connector):
//   0         → display ON  (backlight enabled)
//   non-zero  → display OFF (blanked / DPMS off)
//
// When ON, the DSI clock node is read to distinguish 60 Hz from 90 Hz:
//   > kDsiClockThresholdHz (1200 MHz) → 90 Hz
//   ≤ kDsiClockThresholdHz            → 60 Hz (or fallback if unreadable)
// -----------------------------------------------------------------------
void DisplayStateResidencyDataProvider::updateStats() {
    if (mBlFd < 0) return;

    // Read bl_power value.
    char buf[16] = {};
    ssize_t n = pread(mBlFd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        PLOG(ERROR) << "DisplayStateResidencyDataProvider: pread bl_power failed.";
        return;
    }
    buf[n] = '\0';

    // atoi: "0" → display ON, anything else → display OFF.
    const int blPower = atoi(buf);

    int32_t newState;
    if (blPower != 0) {
        // Display is blanked / off.
        newState = STATE_OFF;
    } else {
        // Display is on — check refresh rate via DSI clock.
        std::string clkStr;
        if (::android::base::ReadFileToString(mClkPath, &clkStr)) {
            uint64_t clkHz = strtoull(clkStr.c_str(), nullptr, 0);
            newState = (clkHz > kDsiClockThresholdHz) ? STATE_90HZ : STATE_60HZ;
        } else {
            // Clock node unreadable (e.g. early boot) — default to 60 Hz.
            LOG(WARNING) << "DisplayStateResidencyDataProvider: could not read "
                         << mClkPath << "; defaulting to 60Hz.";
            newState = STATE_60HZ;
        }
    }

    std::scoped_lock lk(mLock);

    if (newState == mCurState) return;  // No transition, nothing to do.

    uint64_t now = nowMs();

    // Accumulate time spent in the previous state.
    if (mCurState >= 0 && mCurState < NUM_STATES) {
        mResidencies[mCurState].totalTimeInStateMs +=
                static_cast<int64_t>(now - static_cast<uint64_t>(
                        mResidencies[mCurState].lastEntryTimestampMs));
    }

    // Transition to the new state.
    mCurState = newState;
    mResidencies[mCurState].totalStateEntryCount++;
    mResidencies[mCurState].lastEntryTimestampMs = static_cast<int64_t>(now);
}

// -----------------------------------------------------------------------
// pollLoop — background thread.
// -----------------------------------------------------------------------
void DisplayStateResidencyDataProvider::pollLoop() {
    while (mRunThread) {
        int ret = mLooper->pollOnce(-1 /* timeout: block forever */);
        if (ret >= 0) {
            updateStats();
        }
    }
}

}  // namespace stats
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
