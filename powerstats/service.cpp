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

// PowerStats AIDL V2 HAL — Xiaomi Raphael / Redmi K20 Pro (SM8150, Adreno 640)
//
// Entity / consumer map:
//
// State residency entities:
//   RPMH    APSS, MPSS, ADSP, CDSP, SLPI, SLPI_ISLAND  (Sleep)
//   SoC     AOSD, CXSD, DDR  -- HAL-side accumulator (kernel lacks cumulative total)
//   GPU     770/715/615/515/340 MHz + Suspend            — baseline-delta to strip boot history
//   Display Off / 60Hz / 90Hz                            — 200ms timed poll (POLLPRI unsupported)
//
// Energy consumers:
//   display  DISPLAY  — Off/60Hz/90Hz mW coefficients
//   gpu      OTHER    — per-freq mW × delta residency
//   soc      OTHER    — AOSD=21mW / CXSD=21mW / DDR=15mW

#define LOG_TAG "android.hardware.power.stats-service.raphael"

#include "PowerStats.h"
#include "PowerStatsEnergyConsumer.h"
#include "GenericStateResidencyDataProvider.h"
#include "SocStateResidencyDataProvider.h"
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
using aidl::android::hardware::power::stats::SocStateResidencyDataProvider;
using aidl::android::hardware::power::stats::StateResidencyConfig;

// ---------------------------------------------------------------------------
// addRpmhStats
//   Source: /sys/power/rpmh_stats/master_stats
//   Clock: 19.2 MHz XO — divide raw ticks by 19200 to get ms.
// ---------------------------------------------------------------------------
static void addRpmhStats(std::shared_ptr<PowerStats> service) {
    const uint64_t CLK = 19200;
    auto toMs = [CLK](uint64_t v) { return v / CLK; };

    const std::vector<StateResidencyConfig> sleepCfg = {{
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

    sdp->addEntity("APSS",        sleepCfg);
    sdp->addEntity("MPSS",        sleepCfg);
    sdp->addEntity("ADSP",        sleepCfg);
    sdp->addEntity("CDSP",        sleepCfg);
    sdp->addEntity("SLPI",        sleepCfg);

    // SLPI low-power island — same sysfs prefixes, different state name.
    const std::vector<StateResidencyConfig> islandCfg = {{
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
    sdp->addEntity("SLPI_ISLAND", islandCfg);

    service->addStateResidencyDataProvider(std::move(sdp));
}

// ---------------------------------------------------------------------------
// addSocStats
//   Source: /sys/power/system_sleep/stats
//   Uses SocStateResidencyDataProvider (stateful HAL-side accumulator) because
//   the kernel only exposes the duration of the LAST sleep entry, not a running
//   total.  The provider accumulates (delta_count × last_sleep_ms) across calls.
// ---------------------------------------------------------------------------
static void addSocStats(std::shared_ptr<PowerStats> service) {
    service->addStateResidencyDataProvider(
            std::make_unique<SocStateResidencyDataProvider>(
                    "/sys/power/system_sleep/stats"));
}

// ---------------------------------------------------------------------------
// addGpuStats
//   GpuStateResidencyDataProvider reads the actual OPP table at construction
//   (handles SKU-0 770/715/615/515/340 MHz and SKU-1 692/675/615/515/340 MHz).
//   It captures a baseline from gpu_clock_stats + suspend_time at startup so
//   returned values are delta-from-HAL-start rather than monotonic since boot.
// ---------------------------------------------------------------------------
static void addGpuStats(std::shared_ptr<PowerStats> service) {
    service->addStateResidencyDataProvider(
            std::make_unique<GpuStateResidencyDataProvider>());
}

// ---------------------------------------------------------------------------
// addDisplayStats
//   Timed polling on bl_power every 200ms + DSI clock read for 60/90Hz.
//   poll(POLLPRI) is NOT used: the Raphael CAF 4.14 backlight driver does not
//   call sysfs_notify(), so POLLPRI never fires.
//   EPOLLIN (Looper) is NOT used: it is level-triggered on sysfs and causes
//   a 100% CPU spin loop.
// ---------------------------------------------------------------------------
static void addDisplayStats(std::shared_ptr<PowerStats> service) {
    service->addStateResidencyDataProvider(
            std::make_unique<DisplayStateResidencyDataProvider>(
                    "/sys/class/backlight/panel0-backlight/bl_power",
                    "/sys/devices/platform/soc/soc:qcom,dsi-display-primary/"
                    "dynamic_dsi_clock"));
}

// ---------------------------------------------------------------------------
// addEnergyConsumers
// ---------------------------------------------------------------------------
static void addEnergyConsumers(std::shared_ptr<PowerStats> service) {

    // ── Display ──────────────────────────────────────────────────────────
    // Panel power measured at typical brightness:
    //   60Hz ≈ 776 mW  |  90Hz scaled by DSI clock ratio ≈ 916 mW
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

    // ── GPU ──────────────────────────────────────────────────────────────
    // Adreno 640 active power at each OPP corner (both SKUs handled;
    // any freq absent on this SKU simply contributes 0 because
    // GpuStateResidencyDataProvider will not register that state).
    service->addEnergyConsumer(std::make_unique<PowerStatsEnergyConsumer>(
            service,
            EnergyConsumerType::OTHER,
            "gpu",
            "GPU",
            std::map<std::string, int32_t>{
                {"770MHz", 1060},  // SKU-0 only
                {"715MHz",  720},  // SKU-0 only
                {"692MHz",  820},  // SKU-1 only
                {"675MHz",  760},  // SKU-1 only
                {"615MHz",  480},  // both SKUs
                {"515MHz",  270},  // both SKUs
                {"340MHz",   90},  // both SKUs
                {"Suspend",   0},
            }));

    // ── SoC deep sleep ───────────────────────────────────────────────────
    // Static leakage power during SoC CX/MX rail collapse.
    // ~5.4 mA at 3.85V nominal ≈ 21 mW for both AOSD and CXSD.
    service->addEnergyConsumer(std::make_unique<PowerStatsEnergyConsumer>(
            service,
            EnergyConsumerType::OTHER,
            "soc",
            "SoC",
            std::map<std::string, int32_t>{
                {"AOSD", 21},
                {"CXSD", 21},
                {"DDR",  15},
            }));


}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    // One binder thread: handles concurrent getStateResidency calls and
    // linkToDeath correctly.  Without this, the framework logs a warning and
    // death notifications may be missed.
    ABinderProcess_setThreadPoolMaxThreadCount(1);

    auto service = ndk::SharedRefBase::make<PowerStats>();

    addRpmhStats(service);
    addSocStats(service);
    addGpuStats(service);
    addDisplayStats(service);
    addEnergyConsumers(service);

    const std::string instance = std::string(PowerStats::descriptor) + "/default";
    binder_status_t status =
            AServiceManager_addService(service->asBinder().get(), instance.c_str());
    if (status != STATUS_OK) {
        LOG(FATAL) << "Failed to register " << instance << " (status=" << status << ")";
        return EXIT_FAILURE;
    }

    LOG(INFO) << "android.hardware.power.stats-service.raphael is ready.";
    ABinderProcess_joinThreadPool();

    LOG(FATAL) << "Thread pool exited unexpectedly.";
    return EXIT_FAILURE;
}
