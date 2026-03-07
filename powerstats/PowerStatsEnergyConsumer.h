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

#include "PowerStats.h"

#include <android-base/logging.h>

#include <map>
#include <string>

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace stats {

// Computes energy consumed (in µW·s) by a named power entity by multiplying
// the time spent in each state (from getStateResidency) by a per-state
// power coefficient in milliwatts.
//
// IMPORTANT — zero-result contract:
//   The AIDL PowerStats V2 framework (system_server) will crash with an
//   ArrayIndexOutOfBoundsException if a declared energy consumer returns no
//   result for its ID.  Therefore this class NEVER returns std::nullopt.
//   On any error (entity not found, sysfs unreadable, empty residency data)
//   it returns an EnergyConsumerResult with energyUWs = 0 so the framework
//   always receives a valid (if stale) result.
class PowerStatsEnergyConsumer : public PowerStats::IEnergyConsumer {
  public:
    // stateCoefficients maps state name → power in milliwatts (mW).
    // States not present in the map contribute 0 energy.
    PowerStatsEnergyConsumer(std::shared_ptr<PowerStats> service,
                             EnergyConsumerType type,
                             std::string name,
                             std::string entityName,
                             std::map<std::string, int32_t> stateCoefficients)
        : mService(std::move(service)),
          mType(type),
          mName(std::move(name)),
          mEntityName(std::move(entityName)),
          mStateCoefficients(std::move(stateCoefficients)) {}

    std::pair<EnergyConsumerType, std::string> getInfo() override {
        return {mType, mName};
    }

    std::string getConsumerName() override {
        return mName;
    }

    // Returns a valid EnergyConsumerResult on all code paths.
    // energyUWs will be 0 whenever underlying data is unavailable.
    std::optional<EnergyConsumerResult> getEnergyConsumed() override {
        // Zero result used as the safe fallback on any error.
        EnergyConsumerResult zeroResult{.id = 0, .timestampMs = 0, .energyUWs = 0};

        // Look up the entity by name.
        std::vector<PowerEntity> entities;
        if (!mService->getPowerEntityInfo(&entities).isOk()) {
            LOG(ERROR) << "PowerStatsEnergyConsumer[" << mName
                       << "]: getPowerEntityInfo failed; returning 0.";
            return zeroResult;
        }

        int32_t entityId = -1;
        for (const auto &e : entities) {
            if (e.name == mEntityName) {
                entityId = e.id;
                break;
            }
        }

        if (entityId < 0) {
            LOG(ERROR) << "PowerStatsEnergyConsumer[" << mName
                       << "]: entity '" << mEntityName << "' not found; returning 0.";
            return zeroResult;
        }

        // Fetch residency data for this entity.
        std::vector<StateResidencyResult> residencyResults;
        auto status = mService->getStateResidency({entityId}, &residencyResults);
        if (!status.isOk() || residencyResults.empty()) {
            LOG(ERROR) << "PowerStatsEnergyConsumer[" << mName
                       << "]: getStateResidency failed or returned empty; returning 0.";
            return zeroResult;
        }

        const auto &stateData = residencyResults[0].stateResidencyData;
        if (stateData.empty()) {
            LOG(WARNING) << "PowerStatsEnergyConsumer[" << mName
                         << "]: residency data empty (sysfs not ready?); returning 0.";
            return zeroResult;
        }

        // Accumulate energy: Σ (time_in_state_ms × mW) = µW·s
        int64_t totalEnergyUWs = 0;
        int64_t latestTimestampMs = 0;

        const auto &stateInfos = entities[entityId].states;
        for (const auto &sr : stateData) {
            // Bounds check: state ID must be valid in the registered state list.
            if (sr.id < 0 || sr.id >= static_cast<int32_t>(stateInfos.size())) {
                LOG(WARNING) << "PowerStatsEnergyConsumer[" << mName
                             << "]: state id " << sr.id << " out of range; skipping.";
                continue;
            }

            const std::string &stateName = stateInfos[sr.id].name;
            auto it = mStateCoefficients.find(stateName);
            if (it != mStateCoefficients.end()) {
                totalEnergyUWs += sr.totalTimeInStateMs * static_cast<int64_t>(it->second);
            }

            if (sr.lastEntryTimestampMs > latestTimestampMs) {
                latestTimestampMs = sr.lastEntryTimestampMs;
            }
        }

        return EnergyConsumerResult{
                .id = 0,  // overwritten by PowerStats::addEnergyConsumer
                .timestampMs = latestTimestampMs,
                .energyUWs = totalEnergyUWs,
        };
    }

  private:
    std::shared_ptr<PowerStats> mService;
    EnergyConsumerType mType;
    std::string mName;
    std::string mEntityName;
    std::map<std::string, int32_t> mStateCoefficients;  // state name → mW
};

}  // namespace stats
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
