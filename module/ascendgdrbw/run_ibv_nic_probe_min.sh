#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_SCRIPT="${SCRIPT_DIR}/build_ibv_nic_probe_min.sh"
BUILD_DIR="${SCRIPT_DIR}/build"
TARGET="ascendgdrbw_ibv_nic_probe_min"
NIC_NAME="mlx5_0"
TARGET_IP=""
BUILD_ONLY=0
RUN_ONLY=0
BUILD_ARGS=()

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Run options:
  --nic NAME        NIC hint, default: ${NIC_NAME}
  --ip A.B.C.D      Explicit device IP to match against ibv GIDs
  --build-only      Only build
  --run-only        Only run existing binary

Build passthrough:
  --build-dir DIR
  --target NAME
  --ascend-root DIR
  --no-clean

  -h, --help        Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --nic)
            NIC_NAME="$2"
            shift 2
            ;;
        --ip)
            TARGET_IP="$2"
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
        --build-dir|--target|--ascend-root)
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
    --nic="${NIC_NAME}" \
    $([[ -n "${TARGET_IP}" ]] && printf '%s' "--ip=${TARGET_IP}")
