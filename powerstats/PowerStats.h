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

#include <aidl/android/hardware/power/stats/BnPowerStats.h>
#include <memory>
#include <vector>

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace stats {

class PowerStats : public BnPowerStats {
  public:
    class IEnergyConsumer {
      public:
        virtual ~IEnergyConsumer() = default;
        virtual std::pair<EnergyConsumerType, std::string> getInfo() = 0;
        virtual std::optional<EnergyConsumerResult> getEnergyConsumed() = 0;
        virtual std::string getConsumerName() = 0;
    };

    PowerStats() = default;
    ~PowerStats() = default;

    // IPowerStats AIDL functions
    ndk::ScopedAStatus getPowerEntityInfo(std::vector<PowerEntity> *_aidl_return) override;
    ndk::ScopedAStatus getStateResidency(const std::vector<int32_t> &in_powerEntityIds,
                                         std::vector<StateResidencyResult> *_aidl_return) override;
    ndk::ScopedAStatus getEnergyConsumerInfo(std::vector<EnergyConsumer> *_aidl_return) override;
    ndk::ScopedAStatus getEnergyConsumed(const std::vector<int32_t> &in_energyConsumerIds,
                                         std::vector<EnergyConsumerResult> *_aidl_return) override;
    ndk::ScopedAStatus getEnergyMeterInfo(std::vector<Channel> *_aidl_return) override;
    ndk::ScopedAStatus readEnergyMeter(const std::vector<int32_t> &in_channelIds,
                                      std::vector<EnergyMeasurement> *_aidl_return) override;
    binder_status_t dump(int fd, const char **args, uint32_t numArgs) override;

    // Helper functions
    void addStateResidencyDataProvider(std::unique_ptr<IStateResidencyDataProvider> p);
    void addEnergyConsumer(std::unique_ptr<IEnergyConsumer> p);

  private:
    std::vector<std::unique_ptr<IStateResidencyDataProvider>> mStateResidencyDataProviders;
    std::vector<PowerEntity> mPowerEntityInfos;
    std::vector<int32_t> mStateResidencyDataProviderIndex;

    std::vector<std::unique_ptr<IEnergyConsumer>> mEnergyConsumers;
    std::vector<EnergyConsumer> mEnergyConsumerInfos;
};

}  // namespace stats
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
