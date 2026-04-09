#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="${SCRIPT_DIR}"
BUILD_DIR="${MODULE_DIR}/build"
TARGET="ascendgdrbw_device0_self_h2d_min"
SET_ENV_PATH="${CANN_SET_ENV_PATH:-}"
ASCEND_ROOT_OVERRIDE="${ASCEND_ROOT_OVERRIDE:-}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --build-dir DIR   Build directory, default: ${BUILD_DIR}
  --target NAME     CMake target, default: ${TARGET}
  --set-env PATH    Explicit CANN set_env.sh path
  --ascend-root DIR Explicit Ascend root for CMake
  --no-clean        Reuse existing build directory
  -h, --help        Show this help
EOF
}

CLEAN_BUILD=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --target)
            TARGET="$2"
            shift 2
            ;;
        --set-env)
            SET_ENV_PATH="$2"
            shift 2
            ;;
        --ascend-root)
            ASCEND_ROOT_OVERRIDE="$2"
            shift 2
            ;;
        --no-clean)
            CLEAN_BUILD=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

find_set_env() {
    local candidates=()
    if [[ -n "${SET_ENV_PATH}" ]]; then
        candidates+=("${SET_ENV_PATH}")
    fi
    if [[ -n "${ASCEND_HOME_PATH:-}" ]]; then
        candidates+=("${ASCEND_HOME_PATH}/set_env.sh")
        candidates+=("${ASCEND_HOME_PATH}/aarch64-linux/set_env.sh")
    fi
    if [[ -n "${ASCEND_TOOLKIT_HOME:-}" ]]; then
        candidates+=("${ASCEND_TOOLKIT_HOME}/set_env.sh")
    fi
    if [[ -n "${ASCEND_CANN_PACKAGE_PATH:-}" ]]; then
        candidates+=("${ASCEND_CANN_PACKAGE_PATH}/set_env.sh")
    fi
    candidates+=(
        "/usr/local/Ascend/cann-9.0.0/set_env.sh"
        "/usr/local/Ascend/cann-9.0.0/aarch64-linux/set_env.sh"
        "/usr/local/Ascend/ascend-toolkit/latest/set_env.sh"
        "/usr/local/Ascend/ascend-toolkit/set_env.sh"
        "/usr/local/Ascend/cann/set_env.sh"
        "${HOME}/Ascend/ascend-toolkit/set_env.sh"
        "${HOME}/Ascend/ascend-toolkit/latest/set_env.sh"
        "${HOME}/Ascend/cann/set_env.sh"
    )

    local candidate
    for candidate in "${candidates[@]}"; do
        if [[ -f "${candidate}" ]]; then
            echo "${candidate}"
            return 0
        fi
    done
    return 1
}

SET_ENV_REAL_PATH="$(find_set_env)" || {
    echo "Failed to locate CANN set_env.sh. Pass it with --set-env PATH" >&2
    exit 1
}

resolve_root_from_set_env_dir() {
    local base_dir="$1"
    local candidates=(
        "${base_dir}/aarch64-linux"
        "${base_dir}"
    )

    local candidate
    for candidate in "${candidates[@]}"; do
        if [[ -f "${candidate}/include/acl/acl.h" && -f "${candidate}/lib64/libascendcl.so" ]]; then
            echo "${candidate}"
            return 0
        fi
    done

    return 1
}

# shellcheck disable=SC1090
source "${SET_ENV_REAL_PATH}"
echo "Using CANN environment: ${SET_ENV_REAL_PATH}"

if [[ -z "${ASCEND_ROOT_OVERRIDE}" ]]; then
    SET_ENV_DIR="$(cd "$(dirname "${SET_ENV_REAL_PATH}")" && pwd)"
    if ASCEND_ROOT_OVERRIDE="$(resolve_root_from_set_env_dir "${SET_ENV_DIR}")"; then
        :
    else
        ASCEND_ROOT_OVERRIDE="$("${SCRIPT_DIR}/resolve_ascend_root.sh")"
    fi
fi
echo "Using ASCEND_ROOT: ${ASCEND_ROOT_OVERRIDE}"

if [[ "${CLEAN_BUILD}" == "1" ]]; then
    rm -rf "${BUILD_DIR}"
fi

cmake -S "${MODULE_DIR}" -B "${BUILD_DIR}" -DASCEND_ROOT="${ASCEND_ROOT_OVERRIDE}"
cmake --build "${BUILD_DIR}" --target "${TARGET}" -j

BIN_PATH="${BUILD_DIR}/${TARGET}"
echo "Build finished: ${BIN_PATH}"
