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

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace stats {

struct StateResidencyConfig {
    std::string name;
    std::string header;
    std::string entryCountPrefix;
    std::string totalTimePrefix;
    std::string lastEntryPrefix;
    std::function<uint64_t(uint64_t)> entryCountTransform;
    std::function<uint64_t(uint64_t)> totalTimeTransform;
    std::function<uint64_t(uint64_t)> lastEntryTransform;
    bool entryCountSupported;
    bool totalTimeSupported;
    bool lastEntrySupported;
};

class GenericStateResidencyDataProvider : public IStateResidencyDataProvider {
  public:
    GenericStateResidencyDataProvider(const std::string &path) : mPath(path) {}
    ~GenericStateResidencyDataProvider() = default;

    // IStateResidencyDataProvider functions
    bool getStateResidencies(
            std::unordered_map<std::string, std::vector<StateResidency>> *results) override;
    std::unordered_map<std::string, std::vector<State>> getInfo() override;

    // Helper functions
    void addEntity(const std::string &name, const std::vector<StateResidencyConfig> &config);

  private:
    const std::string mPath;
    std::vector<std::pair<std::string, std::vector<StateResidencyConfig>>> mConfigs;
};

}  // namespace stats
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
