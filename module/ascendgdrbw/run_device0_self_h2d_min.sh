#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_SCRIPT="${SCRIPT_DIR}/build_device0_self_h2d_min.sh"
BUILD_DIR="${SCRIPT_DIR}/build"
TARGET="ascendgdrbw_device0_self_h2d_min"
SET_ENV_PATH="${CANN_SET_ENV_PATH:-}"
DEVICE_ID=0
NIC_NAME="mlx5_0"
BYTES=$((1024 * 1024))
WARMUP=10
ITERS=100
BUILD_ONLY=0
RUN_ONLY=0
BUILD_ARGS=()

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Run options:
  --device N        Device id, default: ${DEVICE_ID}
  --nic NAME        NIC hint, default: ${NIC_NAME}
  --bytes N         Transfer bytes, default: ${BYTES}
  --warmup N        Warmup loops, default: ${WARMUP}
  --iters N         Timed loops, default: ${ITERS}
  --build-only      Only build
  --run-only        Only run existing binary

Build passthrough:
  --build-dir DIR
  --target NAME
  --set-env PATH
  --ascend-root DIR
  --no-clean

  -h, --help        Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device)
            DEVICE_ID="$2"
            shift 2
            ;;
        --nic)
            NIC_NAME="$2"
            shift 2
            ;;
        --bytes)
            BYTES="$2"
            shift 2
            ;;
        --warmup)
            WARMUP="$2"
            shift 2
            ;;
        --iters)
            ITERS="$2"
            shift 2
            ;;
        --build-only)
            BUILD_ONLY=1
            shift
            ;;
        --run-only)
            RUN_ONLY=1
            shift
            ;;
        --build-dir|--target|--set-env|--ascend-root)
            if [[ "$1" == "--build-dir" ]]; then
                BUILD_DIR="$2"
            elif [[ "$1" == "--target" ]]; then
                TARGET="$2"
            elif [[ "$1" == "--set-env" ]]; then
                SET_ENV_PATH="$2"
            fi
            BUILD_ARGS+=("$1" "$2")
            shift 2
            ;;
        --no-clean)
            BUILD_ARGS+=("$1")
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

if [[ "${BUILD_ONLY}" == "1" && "${RUN_ONLY}" == "1" ]]; then
    echo "--build-only and --run-only cannot be used together" >&2
    exit 1
fi

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

# shellcheck disable=SC1090
source "${SET_ENV_REAL_PATH}"
echo "Using CANN environment: ${SET_ENV_REAL_PATH}"

export ASCEND_RT_VISIBLE_DEVICES="${ASCEND_RT_VISIBLE_DEVICES:-${DEVICE_ID}}"

echo "ASCEND_RT_VISIBLE_DEVICES=${ASCEND_RT_VISIBLE_DEVICES}"

if [[ "${RUN_ONLY}" != "1" ]]; then
    bash "${BUILD_SCRIPT}" "${BUILD_ARGS[@]}"
fi

if [[ "${BUILD_ONLY}" == "1" ]]; then
    echo "Build finished. Skip running because --build-only was specified."
    exit 0
fi

BIN_PATH="${BUILD_DIR}/${TARGET}"
if [[ ! -x "${BIN_PATH}" ]]; then
    echo "Binary not found: ${BIN_PATH}" >&2
    exit 1
fi

exec "${BIN_PATH}" \
    --device="${DEVICE_ID}" \
    --nic="${NIC_NAME}" \
    --bytes="${BYTES}" \
    --warmup="${WARMUP}" \
    --iters="${ITERS}"
