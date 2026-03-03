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
#include <thread>
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
      mLastBlValue(-1),
      mCurState(-1),
      mRunThread(true) {

    mResidencies.resize(NUM_STATES);
    for (int32_t i = 0; i < NUM_STATES; ++i) {
        mResidencies[i].id = i;
        mResidencies[i].totalTimeInStateMs = 0;
        mResidencies[i].totalStateEntryCount = 0;
        mResidencies[i].lastEntryTimestampMs = 0;
    }

    mBlFd = open(mBlPath.c_str(), O_RDONLY);
    if (mBlFd < 0) {
        PLOG(ERROR) << "DisplayStateResidencyDataProvider: failed to open "
                    << mBlPath << ". Display residency will be zeroed.";
        mRunThread = false;
        return;
    }

    // Read initial state synchronously before starting the thread so the
    // very first getStateResidencies() call returns meaningful data.
    updateStats();

    mThread = std::thread(&DisplayStateResidencyDataProvider::pollLoop, this);
}

// -----------------------------------------------------------------------
// Destruction
// -----------------------------------------------------------------------
DisplayStateResidencyDataProvider::~DisplayStateResidencyDataProvider() {
    mRunThread = false;
    if (mThread.joinable()) {
        mThread.join();
    }
    if (mBlFd >= 0) {
        close(mBlFd);
        mBlFd = -1;
    }
}

// -----------------------------------------------------------------------
// getInfo
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
// getStateResidencies
// -----------------------------------------------------------------------
bool DisplayStateResidencyDataProvider::getStateResidencies(
        std::unordered_map<std::string, std::vector<StateResidency>> *results) {

    std::scoped_lock lk(mLock);

    if (mBlFd < 0 && mCurState < 0) {
        results->emplace("Display", mResidencies);
        return false;
    }

    uint64_t now = nowMs();
    std::vector<StateResidency> snapshot = mResidencies;

    // Add elapsed time in current state so callers always see up-to-date
    // values even if no transition has occurred since the last poll wakeup.
    if (mCurState >= 0 && mCurState < NUM_STATES) {
        snapshot[mCurState].totalTimeInStateMs +=
                static_cast<int64_t>(now - static_cast<uint64_t>(
                        snapshot[mCurState].lastEntryTimestampMs));
    }

    results->emplace("Display", std::move(snapshot));
    return true;
}

// -----------------------------------------------------------------------
// nowMs
// -----------------------------------------------------------------------
/*static*/ uint64_t DisplayStateResidencyDataProvider::nowMs() {
    return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
}

// -----------------------------------------------------------------------
// updateStats
//
// Reads bl_power and, if ON, reads the DSI clock to distinguish 60/90Hz.
// Only records a state transition when the computed new state differs from
// the current state — so this can be called on every poll tick cheaply.
//
// Additionally skips all work if the raw bl_power VALUE hasn't changed
// since the last read (checked by the caller in pollLoop via mLastBlValue).
// -----------------------------------------------------------------------
void DisplayStateResidencyDataProvider::updateStats() {
    if (mBlFd < 0) return;

    char buf[16] = {};
    ssize_t n = pread(mBlFd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        PLOG(ERROR) << "DisplayStateResidencyDataProvider: pread bl_power failed.";
        return;
    }
    buf[n] = '\0';

    const int blValue = atoi(buf);

    // If the raw value hasn't changed, the state hasn't changed — skip work.
    // mLastBlValue is only written from the poll thread (single writer) so
    // reading it here without a lock is safe.
    if (blValue == mLastBlValue) return;
    mLastBlValue = blValue;

    int32_t newState;
    if (blValue != 0) {
        newState = STATE_OFF;
    } else {
        std::string clkStr;
        if (::android::base::ReadFileToString(mClkPath, &clkStr)) {
            uint64_t clkHz = strtoull(clkStr.c_str(), nullptr, 0);
            newState = (clkHz > kDsiClockThresholdHz) ? STATE_90HZ : STATE_60HZ;
        } else {
            newState = STATE_60HZ;
        }
    }

    std::scoped_lock lk(mLock);

    if (newState == mCurState) return;

    uint64_t now = nowMs();

    if (mCurState >= 0 && mCurState < NUM_STATES) {
        mResidencies[mCurState].totalTimeInStateMs +=
                static_cast<int64_t>(now - static_cast<uint64_t>(
                        mResidencies[mCurState].lastEntryTimestampMs));
    }

    mCurState = newState;
    mResidencies[mCurState].totalStateEntryCount++;
    mResidencies[mCurState].lastEntryTimestampMs = static_cast<int64_t>(now);
}

// -----------------------------------------------------------------------
// pollLoop
//
// Wakes every kPollIntervalMs and calls updateStats().
// updateStats() does a single pread() and returns immediately if the
// bl_power value has not changed — so idle cost is one syscall per 200ms.
//
// Why timed polling and not epoll/POLLPRI:
//   EPOLLIN on sysfs is level-triggered (always "readable") → spin loop.
//   POLLPRI requires sysfs_notify() in the driver; the Raphael CAF 4.14
//   backlight driver does not call sysfs_notify() → transitions never wake.
//   Timed polling at 200ms latency costs ~0.01% CPU and works correctly.
// -----------------------------------------------------------------------
void DisplayStateResidencyDataProvider::pollLoop() {
    while (mRunThread) {
        std::this_thread::sleep_for(
                std::chrono::milliseconds(kPollIntervalMs));
        if (!mRunThread) break;
        updateStats();
    }
}

}  // namespace stats
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
