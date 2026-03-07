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

#include "PowerStats.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <aidl/android/hardware/power/stats/EnergyConsumerResult.h>
#include <numeric>
#include <sstream>

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace stats {

void PowerStats::addStateResidencyDataProvider(std::unique_ptr<IStateResidencyDataProvider> p) {
    if (!p) {
        return;
    }

    auto info = p->getInfo();
    size_t index = mStateResidencyDataProviders.size();
    mStateResidencyDataProviders.emplace_back(std::move(p));

    for (const auto &[entityName, states] : info) {
        PowerEntity i = {
                .id = static_cast<int32_t>(mPowerEntityInfos.size()),
                .name = entityName,
                .states = states,
        };
        mPowerEntityInfos.emplace_back(i);
        mStateResidencyDataProviderIndex.emplace_back(index);
    }
}

void PowerStats::addEnergyConsumer(std::unique_ptr<IEnergyConsumer> p) {
    if (!p) {
        return;
    }

    auto [type, name] = p->getInfo();
    mEnergyConsumerInfos.emplace_back(EnergyConsumer{
            .id = static_cast<int32_t>(mEnergyConsumerInfos.size()),
            .type = type,
            .name = name,
    });
    mEnergyConsumers.emplace_back(std::move(p));
}

ndk::ScopedAStatus PowerStats::getPowerEntityInfo(std::vector<PowerEntity> *_aidl_return) {
    *_aidl_return = mPowerEntityInfos;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerStats::getStateResidency(const std::vector<int32_t> &in_powerEntityIds,
                                                 std::vector<StateResidencyResult> *_aidl_return) {
    if (mPowerEntityInfos.empty()) {
        return ndk::ScopedAStatus::ok();
    }

    std::vector<int32_t> ids = in_powerEntityIds;
    if (ids.empty()) {
        ids.resize(mPowerEntityInfos.size());
        std::iota(std::begin(ids), std::end(ids), 0);
    }

    std::unordered_map<std::string, std::vector<StateResidency>> stateResidencies;
    for (const int32_t id : ids) {
        if (id < 0 || id >= static_cast<int32_t>(mPowerEntityInfos.size())) {
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
        }

        std::string powerEntityName = mPowerEntityInfos[id].name;
        if (stateResidencies.find(powerEntityName) == stateResidencies.end()) {
            mStateResidencyDataProviders.at(mStateResidencyDataProviderIndex.at(id))
                    ->getStateResidencies(&stateResidencies);
        }

        auto it = stateResidencies.find(powerEntityName);
        if (it != stateResidencies.end()) {
            _aidl_return->emplace_back(StateResidencyResult{
                    .id = id,
                    .stateResidencyData = it->second,
            });
        }
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerStats::getEnergyConsumerInfo(std::vector<EnergyConsumer> *_aidl_return) {
    *_aidl_return = mEnergyConsumerInfos;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerStats::getEnergyConsumed(const std::vector<int32_t> &in_energyConsumerIds,
                                                 std::vector<EnergyConsumerResult> *_aidl_return) {
    std::vector<int32_t> ids = in_energyConsumerIds;
    if (ids.empty()) {
        ids.resize(mEnergyConsumerInfos.size());
        std::iota(ids.begin(), ids.end(), 0);
    }

    for (const auto &id : ids) {
        if (id < 0 || id >= static_cast<int32_t>(mEnergyConsumerInfos.size())) {
            continue;
        }

        auto result = mEnergyConsumers[id]->getEnergyConsumed();
        if (result) {
            result->id = id;
            _aidl_return->emplace_back(*result);
        } else {
            // The framework (system_server PowerStatsCollector) will throw
            // ArrayIndexOutOfBoundsException and crash if a declared consumer
            // returns no result for its ID.  Always emit a zero result as a
            // safe fallback rather than silently dropping the entry.
            LOG(WARNING) << "getEnergyConsumed: consumer " << id << " ("
                         << mEnergyConsumerInfos[id].name
                         << ") returned nullopt; emitting zero result.";
            _aidl_return->emplace_back(EnergyConsumerResult{
                    .id = id, .timestampMs = 0, .energyUWs = 0});
        }
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerStats::getEnergyMeterInfo(std::vector<Channel> *_aidl_return) {
    _aidl_return->clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerStats::readEnergyMeter(const std::vector<int32_t> &/*in_channelIds*/,
                                              std::vector<EnergyMeasurement> *_aidl_return) {
    _aidl_return->clear();
    return ndk::ScopedAStatus::ok();
}

binder_status_t PowerStats::dump(int fd, const char **/*args*/, uint32_t /*numArgs*/) {
    std::ostringstream oss;
    oss << "============= PowerStats AIDL HAL raphael ==============\n";
    
    std::vector<StateResidencyResult> results;
    getStateResidency({}, &results);

    for (const auto &result : results) {
        oss << "Entity [" << result.id << "]: " << mPowerEntityInfos[result.id].name << "\n";
        for (const auto &state : result.stateResidencyData) {
             oss << "  State [" << state.id << "]: " << mPowerEntityInfos[result.id].states[state.id].name
                 << " - " << state.totalTimeInStateMs << " ms, " << state.totalStateEntryCount << " entries\n";
        }
    }

    oss << "--------------------------------------------------------\n";
    std::vector<EnergyConsumerResult> energyResults;
    getEnergyConsumed({}, &energyResults);
    for (const auto &result : energyResults) {
        oss << "Energy Consumer [" << result.id << "]: " << mEnergyConsumerInfos[result.id].name
            << " - " << result.energyUWs << " uWs\n";
    }
    
    oss << "========================================================\n";
    ::android::base::WriteStringToFd(oss.str(), fd);
    fsync(fd);
    return STATUS_OK;
}

}  // namespace stats
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
