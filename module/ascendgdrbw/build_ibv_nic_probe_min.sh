#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="${SCRIPT_DIR}"
BUILD_DIR="${MODULE_DIR}/build"
TARGET="ascendgdrbw_ibv_nic_probe_min"
ASCEND_ROOT_OVERRIDE="${ASCEND_ROOT_OVERRIDE:-/usr/local/Ascend/cann-9.0.0/aarch64-linux}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --build-dir DIR   Build directory, default: ${BUILD_DIR}
  --target NAME     CMake target, default: ${TARGET}
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

if [[ "${CLEAN_BUILD}" == "1" ]]; then
    rm -rf "${BUILD_DIR}"
fi

cmake -S "${MODULE_DIR}" -B "${BUILD_DIR}" -DASCEND_ROOT="${ASCEND_ROOT_OVERRIDE}"
cmake --build "${BUILD_DIR}" --target "${TARGET}" -j

BIN_PATH="${BUILD_DIR}/${TARGET}"
echo "Build finished: ${BIN_PATH}"
