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

#include <aidl/android/hardware/power/stats/PowerEntity.h>
#include <aidl/android/hardware/power/stats/StateResidency.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace stats {

class IStateResidencyDataProvider {
  public:
    virtual ~IStateResidencyDataProvider() = default;
    virtual bool getStateResidencies(
            std::unordered_map<std::string, std::vector<StateResidency>> *results) = 0;
    virtual std::unordered_map<std::string, std::vector<State>> getInfo() = 0;
};

}  // namespace stats
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
