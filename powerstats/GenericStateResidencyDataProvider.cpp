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

#include "GenericStateResidencyDataProvider.h"

#include <android-base/logging.h>
#include <android-base/strings.h>

#include <cstdio>
#include <cstring>
#include <fstream>

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace stats {

static bool extractStat(const char *line, const std::string &prefix, uint64_t &stat) {
    const char *p = strstr(line, prefix.c_str());
    if (p) {
        stat = strtoull(p + prefix.length(), nullptr, 0);
        return true;
    }
    return false;
}

void GenericStateResidencyDataProvider::addEntity(
        const std::string &name, const std::vector<StateResidencyConfig> &config) {
    mConfigs.emplace_back(name, config);
}

// ---------------------------------------------------------------------------
// getStateResidencies
//
// Supports two file formats, auto-detected per entity:
//
// FORMAT A — name-line entities (rpmh_stats):
//   The entity name appears as a standalone line.  State configs have empty
//   .header fields.  All states for the entity are parsed from the lines
//   that follow the name line.
//   Example:
//     APSS
//     Sleep Count:0x0
//     Sleep Accumulated Duration:0x0
//
// FORMAT B — header-driven states (system_sleep/stats):
//   No entity name line exists in the file.  Each STATE is delimited by
//   a per-state header line (config.header).  An entity uses FORMAT B when
//   ALL of its state configs have a non-empty .header.
//   The entity's result slots are pre-inserted at parse start so they appear
//   in the results map even when all values are zero (device never slept).
//   Example:
//     RPM Mode:aosd          <- .header for AOSD state of "SoC" entity
//     count:0
//     actual last sleep(msec):0
//     RPM Mode:cxsd          <- .header for CXSD state
//     count:0
//     actual last sleep(msec):0
// ---------------------------------------------------------------------------
bool GenericStateResidencyDataProvider::getStateResidencies(
        std::unordered_map<std::string, std::vector<StateResidency>> *results) {

    std::unique_ptr<FILE, decltype(&fclose)> fp(fopen(mPath.c_str(), "r"), fclose);
    if (!fp) {
        PLOG(ERROR) << __func__ << ": failed to open " << mPath;
        return false;
    }

    // Pre-insert zeroed residencies for FORMAT B entities.
    // This guarantees that header-driven entities always appear in the results
    // map regardless of whether the device has entered those sleep states.
    // Without this, "SoC" would be absent from results when count=0/time=0,
    // because the parser would never find a matching entity-name line.
    for (const auto &entityConfig : mConfigs) {
        bool allHaveHeaders = !entityConfig.second.empty();
        for (const auto &s : entityConfig.second) {
            if (s.header.empty()) { allHaveHeaders = false; break; }
        }
        if (!allHaveHeaders) continue;

        auto &residencies = (*results)[entityConfig.first];
        if (residencies.empty()) {
            residencies.resize(entityConfig.second.size());
            for (size_t i = 0; i < entityConfig.second.size(); ++i) {
                residencies[i].id = static_cast<int32_t>(i);
            }
        }
    }

    // Build header-string → (entityName, stateIndex) map for FORMAT B lookup.
    std::unordered_map<std::string, std::pair<std::string, int32_t>> headerMap;
    for (const auto &entityConfig : mConfigs) {
        for (size_t i = 0; i < entityConfig.second.size(); ++i) {
            const std::string &hdr = entityConfig.second[i].header;
            if (!hdr.empty()) {
                headerMap[hdr] = {entityConfig.first, static_cast<int32_t>(i)};
            }
        }
    }

    size_t lineLen = 0;
    char *lineBuf = nullptr;

    // FORMAT A active context
    std::string curEntity;
    const std::vector<StateResidencyConfig> *curConfigs = nullptr;

    // FORMAT B active context
    std::string curHdrEntity;
    int32_t curHdrStateIdx = -1;
    const std::vector<StateResidencyConfig> *curHdrConfigs = nullptr;

    while (getline(&lineBuf, &lineLen, fp.get()) != -1) {
        const std::string trimmed = ::android::base::Trim(lineBuf);

        // --- FORMAT A: check for an entity name line ---
        bool switchedEntity = false;
        for (const auto &entityConfig : mConfigs) {
            // Only consider FORMAT A entities (at least one state has no header)
            bool hasAnyHeader = false;
            for (const auto &s : entityConfig.second) {
                if (!s.header.empty()) { hasAnyHeader = true; break; }
            }
            if (hasAnyHeader) continue;

            const std::string &name = entityConfig.first;
            if (trimmed == name || trimmed == (name + ":")) {
                curEntity = name;
                curConfigs = &entityConfig.second;
                // Entering a FORMAT A entity clears any active FORMAT B context.
                curHdrEntity.clear();
                curHdrStateIdx = -1;
                curHdrConfigs = nullptr;
                switchedEntity = true;
                break;
            }
        }
        if (switchedEntity) continue;

        // --- FORMAT B: check for a state header line ---
        auto headerIt = headerMap.find(trimmed);
        if (headerIt != headerMap.end()) {
            curHdrEntity    = headerIt->second.first;
            curHdrStateIdx  = headerIt->second.second;
            curHdrConfigs   = nullptr;
            for (const auto &ec : mConfigs) {
                if (ec.first == curHdrEntity) { curHdrConfigs = &ec.second; break; }
            }
            // Entering a FORMAT B state clears any active FORMAT A context.
            curEntity.clear();
            curConfigs = nullptr;
            continue;
        }

        // --- Extract stats for the active FORMAT A entity ---
        if (!curEntity.empty() && curConfigs) {
            auto &residencies = (*results)[curEntity];
            if (residencies.empty()) {
                residencies.resize(curConfigs->size());
                for (size_t i = 0; i < curConfigs->size(); ++i)
                    residencies[i].id = static_cast<int32_t>(i);
            }
            for (size_t i = 0; i < curConfigs->size(); ++i) {
                const auto &cfg = (*curConfigs)[i];
                uint64_t val = 0;
                if (cfg.entryCountSupported && extractStat(lineBuf, cfg.entryCountPrefix, val))
                    residencies[i].totalStateEntryCount =
                            cfg.entryCountTransform ? cfg.entryCountTransform(val) : val;
                if (cfg.totalTimeSupported && extractStat(lineBuf, cfg.totalTimePrefix, val))
                    residencies[i].totalTimeInStateMs =
                            cfg.totalTimeTransform ? cfg.totalTimeTransform(val) : val;
                if (cfg.lastEntrySupported && extractStat(lineBuf, cfg.lastEntryPrefix, val))
                    residencies[i].lastEntryTimestampMs =
                            cfg.lastEntryTransform ? cfg.lastEntryTransform(val) : val;
            }
            continue;
        }

        // --- Extract stats for the active FORMAT B state ---
        if (!curHdrEntity.empty() && curHdrStateIdx >= 0 && curHdrConfigs) {
            auto &residencies = (*results)[curHdrEntity];
            if (curHdrStateIdx >= static_cast<int32_t>(residencies.size())) continue;

            const auto &cfg = (*curHdrConfigs)[curHdrStateIdx];
            uint64_t val = 0;
            if (cfg.entryCountSupported && extractStat(lineBuf, cfg.entryCountPrefix, val))
                residencies[curHdrStateIdx].totalStateEntryCount =
                        cfg.entryCountTransform ? cfg.entryCountTransform(val) : val;
            if (cfg.totalTimeSupported && extractStat(lineBuf, cfg.totalTimePrefix, val))
                residencies[curHdrStateIdx].totalTimeInStateMs =
                        cfg.totalTimeTransform ? cfg.totalTimeTransform(val) : val;
            if (cfg.lastEntrySupported && extractStat(lineBuf, cfg.lastEntryPrefix, val))
                residencies[curHdrStateIdx].lastEntryTimestampMs =
                        cfg.lastEntryTransform ? cfg.lastEntryTransform(val) : val;
        }
    }

    free(lineBuf);
    return true;
}

std::unordered_map<std::string, std::vector<State>>
GenericStateResidencyDataProvider::getInfo() {
    std::unordered_map<std::string, std::vector<State>> info;
    for (const auto &entityConfig : mConfigs) {
        std::vector<State> states;
        for (size_t i = 0; i < entityConfig.second.size(); ++i) {
            states.push_back({.id = static_cast<int32_t>(i),
                              .name = entityConfig.second[i].name});
        }
        info[entityConfig.first] = states;
    }
    return info;
}

}  // namespace stats
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
