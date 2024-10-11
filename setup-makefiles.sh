#!/bin/bash
#
# SPDX-FileCopyrightText: 2016 The CyanogenMod Project
# SPDX-FileCopyrightText: 2017-2024 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

set -e

DEVICE=raphael
VENDOR=xiaomi

# Load extract_utils and do some sanity checks
MY_DIR="${BASH_SOURCE%/*}"
if [[ ! -d "${MY_DIR}" ]]; then MY_DIR="${PWD}"; fi

ANDROID_ROOT="${MY_DIR}/../../.."

export TARGET_ENABLE_CHECKELF=true

HELPER="${ANDROID_ROOT}/tools/extract-utils/extract_utils.sh"
if [ ! -f "${HELPER}" ]; then
    echo "Unable to find helper script at ${HELPER}"
    exit 1
fi
source "${HELPER}"


function vendor_imports() {
    cat <<EOF >>"$1"
	"device/xiaomi/raphael",
	"device/qcom/common/vendor/media-legacy",
	"vendor/qcom/common/vendor/media-legacy",
        "hardware/google/interfaces",
        "hardware/google/pixel",
        "hardware/xiaomi",
        "vendor/qcom/common/system/av",
        "vendor/google/pixel",
        "hardware/qcom-caf/sm8150",
	"hardware/qcom-caf/sm8250",
        "vendor/qcom/opensource/commonsys/display",
        "vendor/qcom/opensource/commonsys-intf/display",
        "vendor/qcom/opensource/display",
        "vendor/qcom/opensource/data-ipa-cfg-mgr-legacy-um",
        "vendor/qcom/opensource/dataservices",
        "hardware/qcom-caf/wlan",
EOF
}

# Initialize the helper
setup_vendor "${DEVICE}" "${VENDOR}" "${ANDROID_ROOT}"

# Warning headers and guards
write_headers

write_makefiles "${MY_DIR}/proprietary-files.txt" true

# Finish
write_footers
