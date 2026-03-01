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

// PowerStats AIDL V2 HAL service for Xiaomi Raphael (Redmi K20 Pro / Mi 9T Pro)
// Snapdragon 855 (SM8150) — Adreno 640
//
// Migrated from android.hardware.power.stats@1.0 (HIDL) to
// android.hardware.power.stats AIDL V2, removing all dependency on
// hardware/google/pixel/powerstats (libpixelpowerstats).
//
// Entities registered:
//   RPMH:  APSS, MPSS, ADSP, CDSP, SLPI, SLPI_ISLAND  — Sleep state
//   SoC:   AOSD, CXSD                                  — system-sleep stats
//   GPU:   per-frequency bins + Suspend                 — Adreno 640
//   Display: Off, 60Hz, 90Hz                           — DSI/backlight polling
//
// Energy consumers registered:
//   Display (DISPLAY type) — Off/60Hz/90Hz mW coefficients
//   GPU     (OTHER type)   — per-frequency mW coefficients (both SKUs handled
//                            at runtime because GpuStateResidencyDataProvider
//                            reads the actual freq table from sysfs)
//   SoC     (OTHER type)   — AOSD/CXSD mW coefficients

#define LOG_TAG "android.hardware.power.stats-service.raphael"

#include "PowerStats.h"
#include "PowerStatsEnergyConsumer.h"
#include "GenericStateResidencyDataProvider.h"
#include "GpuStateResidencyDataProvider.h"
#include "DisplayStateResidencyDataProvider.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using aidl::android::hardware::power::stats::DisplayStateResidencyDataProvider;
using aidl::android::hardware::power::stats::EnergyConsumerType;
using aidl::android::hardware::power::stats::GenericStateResidencyDataProvider;
using aidl::android::hardware::power::stats::GpuStateResidencyDataProvider;
using aidl::android::hardware::power::stats::PowerStats;
using aidl::android::hardware::power::stats::PowerStatsEnergyConsumer;
using aidl::android::hardware::power::stats::StateResidencyConfig;

// ---------------------------------------------------------------------------
// RPMH subsystem sleep stats
//   Source: /sys/power/rpmh_stats/master_stats
//   Clock:  19.2 MHz — divide raw ticks by 19200 to get milliseconds.
// ---------------------------------------------------------------------------
static void addRpmhStats(std::shared_ptr<PowerStats> service) {
    const uint64_t RPM_CLK = 19200;
    auto toMs = [](uint64_t a) { return a / RPM_CLK; };

    const std::vector<StateResidencyConfig> sleepConfig = {{
        .name                = "Sleep",
        .entryCountSupported = true,
        .entryCountPrefix    = "Sleep Count:",
        .totalTimeSupported  = true,
        .totalTimePrefix     = "Sleep Accumulated Duration:",
        .totalTimeTransform  = toMs,
        .lastEntrySupported  = true,
        .lastEntryPrefix     = "Sleep Last Entered At:",
        .lastEntryTransform  = toMs,
    }};

    auto sdp = std::make_unique<GenericStateResidencyDataProvider>(
            "/sys/power/rpmh_stats/master_stats");

    sdp->addEntity("APSS",        sleepConfig);
    sdp->addEntity("MPSS",        sleepConfig);
    sdp->addEntity("ADSP",        sleepConfig);
    sdp->addEntity("CDSP",        sleepConfig);
    sdp->addEntity("SLPI",        sleepConfig);

    // SLPI_ISLAND uses a different state name ("uImage") but the same prefixes.
    const std::vector<StateResidencyConfig> islandConfig = {{
        .name                = "uImage",
        .entryCountSupported = true,
        .entryCountPrefix    = "Sleep Count:",
        .totalTimeSupported  = true,
        .totalTimePrefix     = "Sleep Accumulated Duration:",
        .totalTimeTransform  = toMs,
        .lastEntrySupported  = true,
        .lastEntryPrefix     = "Sleep Last Entered At:",
        .lastEntryTransform  = toMs,
    }};
    sdp->addEntity("SLPI_ISLAND", islandConfig);

    service->addStateResidencyDataProvider(std::move(sdp));
}

// ---------------------------------------------------------------------------
// SoC-level sleep stats (AOSD / CXSD)
//   Source: /sys/power/system_sleep/stats
//
//   Kernel output format (per RPM mode):
//     RPM Mode:aosd
//     count: <N>
//     actual last sleep(msec): <T>
//     RPM Mode:cxsd
//     ...
//
//   totalTimePrefix matches "actual last sleep(msec):" which is cumulative
//   sleep time in milliseconds (already in ms — no transform needed).
//   lastEntry is not available in this node.
// ---------------------------------------------------------------------------
static void addSocStats(std::shared_ptr<PowerStats> service) {
    const std::vector<StateResidencyConfig> socConfigs = {
        {
            .name                = "AOSD",
            .header              = "RPM Mode:aosd",
            .entryCountSupported = true,
            .entryCountPrefix    = "count:",
            .totalTimeSupported  = true,
            .totalTimePrefix     = "actual last sleep(msec):",
            .lastEntrySupported  = false,
        },
        {
            .name                = "CXSD",
            .header              = "RPM Mode:cxsd",
            .entryCountSupported = true,
            .entryCountPrefix    = "count:",
            .totalTimeSupported  = true,
            .totalTimePrefix     = "actual last sleep(msec):",
            .lastEntrySupported  = false,
        },
    };

    auto sdp = std::make_unique<GenericStateResidencyDataProvider>(
            "/sys/power/system_sleep/stats");
    sdp->addEntity("SoC", socConfigs);

    service->addStateResidencyDataProvider(std::move(sdp));
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    // Use at least 1 binder thread so the service can handle concurrent calls
    // and linkToDeath works correctly.  0 threads causes a framework warning
    // and prevents proper client death notification.
    ABinderProcess_setThreadPoolMaxThreadCount(1);

    auto service = ndk::SharedRefBase::make<PowerStats>();

    // ---- State residency data providers ----
    addRpmhStats(service);
    addSocStats(service);

    // GPU — GpuStateResidencyDataProvider reads the actual frequency table
    // from sysfs at construction, so it handles both SKUs automatically:
    //   SKU 0 (speed-bin=0): 770, 715, 615, 515, 340 MHz
    //   SKU 1 (speed-bin=1): 692, 675, 615, 515, 340 MHz
    service->addStateResidencyDataProvider(
            std::make_unique<GpuStateResidencyDataProvider>());

    // Display — monitors bl_power via Looper + reads DSI clock for 60/90Hz.
    service->addStateResidencyDataProvider(
            std::make_unique<DisplayStateResidencyDataProvider>(
                    "/sys/class/backlight/panel0-backlight/bl_power",
                    "/sys/devices/platform/soc/soc:qcom,dsi-display-primary/"
                    "dynamic_dsi_clock"));

    // ---- Energy consumers ----
    //
    // Display (DISPLAY type)
    //   Measured panel power at 60 Hz ≈ 776 mW.
    //   90 Hz scaled by DSI clock ratio: 776 × (1300/1100) ≈ 916 mW.
    //   (Conservative estimate; exact value depends on content brightness.)
    service->addEnergyConsumer(std::make_unique<PowerStatsEnergyConsumer>(
            service,
            EnergyConsumerType::DISPLAY,
            "display",
            "Display",
            std::map<std::string, int32_t>{
                {"Off",   0},
                {"60Hz",  776},
                {"90Hz",  916},
            }));

    // GPU (OTHER type)
    //   Power coefficients from Adreno 640 vendor corner data (4.14 kernel).
    //   States whose names match GpuStateResidencyDataProvider output are used;
    //   any freq bin absent on a given SKU simply contributes 0 energy.
    //   Both SKU tables share 615/515/340 MHz, so those entries always match.
    service->addEnergyConsumer(std::make_unique<PowerStatsEnergyConsumer>(
            service,
            EnergyConsumerType::OTHER,
            "gpu",
            "GPU",
            std::map<std::string, int32_t>{
                // SKU 0 exclusive
                {"770MHz",   1060},
                {"715MHz",    720},
                // SKU 1 exclusive
                {"692MHz",    820},
                {"675MHz",    760},
                // Shared across both SKUs
                {"615MHz",    480},
                {"515MHz",    270},
                {"340MHz",     90},
                {"Suspend",     0},
            }));

    // SoC (OTHER type)
    //   Based on ~5.4 mA idle current at 3.85 V nominal ≈ 21 mW during deep sleep.
    //   AOSD and CXSD represent different SoC idle rail states but share a similar
    //   static power floor at this granularity.
    service->addEnergyConsumer(std::make_unique<PowerStatsEnergyConsumer>(
            service,
            EnergyConsumerType::OTHER,
            "soc",
            "SoC",
            std::map<std::string, int32_t>{
                {"AOSD", 21},
                {"CXSD", 21},
            }));

    // ---- Register with servicemanager and join thread pool ----
    const std::string instance =
            std::string(PowerStats::descriptor) + "/default";

    binder_status_t status =
            AServiceManager_addService(service->asBinder().get(), instance.c_str());
    if (status != STATUS_OK) {
        LOG(FATAL) << "Failed to register " << instance << " (status=" << status << ")";
        return EXIT_FAILURE;
    }

    LOG(INFO) << "android.hardware.power.stats-service.raphael is ready.";

    ABinderProcess_joinThreadPool();

    // Should never be reached.
    LOG(FATAL) << "android.hardware.power.stats-service.raphael thread pool exited.";
    return EXIT_FAILURE;
}
