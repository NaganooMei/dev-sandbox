#!/usr/bin/env bash

set -euo pipefail

pick_root_from_config() {
    local config_path="$1"
    local cmake_dir
    local root

    cmake_dir="$(cd "$(dirname "$config_path")" && pwd)"
    root="$(cd "${cmake_dir}/../.." && pwd)"

    if [ -f "${cmake_dir}/ASCConfig.cmake" ] && [ -f "${cmake_dir}/AICPUConfig.cmake" ]; then
        echo "${root}"
        return 0
    fi

    return 1
}

try_candidate_root() {
    local root="$1"
    if [ -z "${root}" ] || [ ! -d "${root}" ]; then
        return 1
    fi

    if [ -f "${root}/lib64/cmake/ASCConfig.cmake" ] && [ -f "${root}/lib64/cmake/AICPUConfig.cmake" ]; then
        echo "${root}"
        return 0
    fi

    return 1
}

if [ -n "${ASCEND_ROOT_OVERRIDE:-}" ]; then
    if try_candidate_root "${ASCEND_ROOT_OVERRIDE}"; then
        exit 0
    fi
fi

if [ -n "${ASCEND_HOME_PATH:-}" ]; then
    if try_candidate_root "${ASCEND_HOME_PATH}"; then
        exit 0
    fi
fi

if [ -n "${ASCEND_TOOLKIT_HOME:-}" ]; then
    if try_candidate_root "${ASCEND_TOOLKIT_HOME}"; then
        exit 0
    fi
fi

while IFS= read -r config_path; do
    if pick_root_from_config "${config_path}"; then
        exit 0
    fi
done < <(find /usr/local/Ascend -path '*/lib64/cmake/ASCConfig.cmake' 2>/dev/null | sort)

echo "failed to find an Ascend root that contains both ASCConfig.cmake and AICPUConfig.cmake" >&2
exit 1
