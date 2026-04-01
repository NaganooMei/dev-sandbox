#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "${SCRIPT_DIR}/../.." && pwd)

HIXL_ROOT=${HIXL_ROOT:-"${ROOT_DIR}/hixl"}
HIXL_BUILD_DIR=${HIXL_BUILD_DIR:-"${HIXL_ROOT}/build"}
BUILD_DIR=${BUILD_DIR:-"${ROOT_DIR}/build-hixlbw"}

find_hixl_lib() {
    local candidates=(
        "${HIXL_BUILD_DIR}/src/llm_datadist/libcann_hixl.so"
        "${HIXL_BUILD_DIR}/build/src/llm_datadist/libcann_hixl.so"
        "${HIXL_BUILD_DIR}/src/hixl/libcann_hixl.so"
        "${HIXL_ROOT}/build/src/llm_datadist/libcann_hixl.so"
        "${HIXL_ROOT}/build/src/hixl/libcann_hixl.so"
        "${HIXL_ROOT}/build_out/lib/libcann_hixl.so"
        "${HIXL_ROOT}/build_out/lib64/libcann_hixl.so"
        "${HIXL_ROOT}/build_out/hixl/lib/libcann_hixl.so"
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

source_ascend_env() {
    local candidates=(
        "/usr/local/Ascend/cann/set_env.sh"
        "/usr/local/Ascend/ascend-toolkit/set_env.sh"
        "/usr/local/Ascend/ascend-toolkit/latest/set_env.sh"
        "/usr/local/Ascend/latest/bin/setenv.bash"
    )
    for candidate in "${candidates[@]}"; do
        if [[ -f "${candidate}" ]]; then
            # shellcheck disable=SC1090
            source "${candidate}"
            return 0
        fi
    done
    return 1
}

if [[ -z "${ASCEND_HOME_PATH:-}" && -z "${ASCEND_TOOLKIT_HOME:-}" ]]; then
    source_ascend_env || true
fi

ASCEND_ROOT=${ASCEND_ROOT:-${ASCEND_HOME_PATH:-${ASCEND_TOOLKIT_HOME:-}}}
if [[ -z "${ASCEND_ROOT}" || ! -d "${ASCEND_ROOT}" ]]; then
    echo "failed to resolve ASCEND_ROOT; please source CANN set_env.sh or export ASCEND_ROOT" >&2
    exit 1
fi

if [[ ! -d "${HIXL_ROOT}" ]]; then
    echo "HIXL_ROOT does not exist: ${HIXL_ROOT}" >&2
    exit 1
fi

HIXL_LIB_PATH="$(find_hixl_lib || true)"
if [[ -z "${HIXL_LIB_PATH}" ]]; then
    echo "building HIXL library first..." >&2
    (
        cd "${HIXL_ROOT}"
        bash build.sh --build-type=Release
    )
    HIXL_LIB_PATH="$(find_hixl_lib || true)"
fi

if [[ -z "${HIXL_LIB_PATH}" ]]; then
    echo "failed to locate libcann_hixl.so under HIXL_ROOT=${HIXL_ROOT} HIXL_BUILD_DIR=${HIXL_BUILD_DIR}" >&2
    exit 1
fi

echo "using HIXL lib: ${HIXL_LIB_PATH}" >&2

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DASCEND_ROOT="${ASCEND_ROOT}" \
    -DHIXL_ROOT="${HIXL_ROOT}" \
    -DHIXL_BUILD_DIR="${HIXL_BUILD_DIR}"

if command -v nproc >/dev/null 2>&1; then
    JOBS=$(nproc)
else
    JOBS=8
fi

cmake --build "${BUILD_DIR}" --target hixlbw_h2d_async_compare -j"${JOBS}"

echo "binary: ${BUILD_DIR}/module/hixlbw/hixlbw_h2d_async_compare"
