#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_SCRIPT="${SCRIPT_DIR}/build_ra_npu_nic_open_min.sh"
BUILD_DIR="${SCRIPT_DIR}/build"
TARGET="ascendgdrbw_ra_npu_nic_open_min"
DEVICE_ID=0
TARGET_IP=""
ENABLE_SOCKET=0
BUILD_ONLY=0
RUN_ONLY=0
BUILD_ARGS=()

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Run options:
  --device N        Device id, default: ${DEVICE_ID}
  --ip A.B.C.D      Explicit device IP, e.g. 192.168.190.130
  --socket          Also test hrtRaSocketInit
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
        --ip)
            TARGET_IP="$2"
            shift 2
            ;;
        --socket)
            ENABLE_SOCKET=1
            shift
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
    $([[ -n "${TARGET_IP}" ]] && printf '%s' "--ip=${TARGET_IP}") \
    $([[ "${ENABLE_SOCKET}" == "1" ]] && printf '%s' "--socket")
