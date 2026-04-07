#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [ -f "/usr/local/Ascend/cann/set_env.sh" ]; then
    set +u
    source /usr/local/Ascend/cann/set_env.sh
    set -u
fi

ASCEND_ROOT="$(bash "${REPO_ROOT}/module/ascendgdrbw/resolve_ascend_root.sh")"
BUILD_DIR="${REPO_ROOT}/build-hccl-channel"
TARGET="ascendgdrbw_hccl_channel_demo"
BIN_PATH="${BUILD_DIR}/module/ascendgdrbw/${TARGET}"

section() {
    echo
    echo "========== $1 =========="
}

prepend_path() {
    local var_name="$1"
    local new_value="$2"
    local old_value="${!var_name-}"
    if [ -n "$old_value" ]; then
        export "${var_name}=${new_value}:${old_value}"
    else
        export "${var_name}=${new_value}"
    fi
}

strip_ascend_paths() {
    local var_name="$1"
    local old_value="${!var_name-}"
    local filtered=""
    local part

    IFS=':' read -r -a parts <<< "$old_value"
    for part in "${parts[@]}"; do
        if [ -z "$part" ]; then
            continue
        fi
        case "$part" in
            /usr/local/Ascend/*)
                continue
                ;;
        esac
        if [ -n "$filtered" ]; then
            filtered="${filtered}:$part"
        else
            filtered="$part"
        fi
    done

    export "${var_name}=${filtered}"
}

section "Select Toolkit"
echo "ASCEND_ROOT=${ASCEND_ROOT}"

section "Override Env For selected toolkit"
export ASCEND_HOME_PATH="${ASCEND_ROOT}"
export ASCEND_TOOLKIT_HOME="${ASCEND_ROOT}"
export ASCEND_OPP_PATH="${ASCEND_ROOT}/opp"

strip_ascend_paths PATH
strip_ascend_paths LD_LIBRARY_PATH
strip_ascend_paths PYTHONPATH

prepend_path PATH "${ASCEND_ROOT}/bin"
prepend_path PATH "${ASCEND_ROOT}/compiler/ccec_compiler/bin"
prepend_path PATH "${ASCEND_ROOT}/tools/ccec_compiler/bin"
prepend_path LD_LIBRARY_PATH "${ASCEND_ROOT}/lib64"
prepend_path LD_LIBRARY_PATH "${ASCEND_ROOT}/lib64/plugin/opskernel"
prepend_path LD_LIBRARY_PATH "${ASCEND_ROOT}/lib64/plugin/nnengine"
prepend_path LD_LIBRARY_PATH "/usr/local/Ascend/driver/lib64"
prepend_path LD_LIBRARY_PATH "/usr/local/Ascend/driver/lib64/common"
prepend_path LD_LIBRARY_PATH "/usr/local/Ascend/driver/lib64/driver"
prepend_path PYTHONPATH "${ASCEND_ROOT}/python/site-packages"
prepend_path PYTHONPATH "${ASCEND_ROOT}/opp/built-in/op_impl/ai_core/tbe"

unset HCCL_INTRA_PCIE_ENABLE || true
export HCCL_INTRA_ROCE_ENABLE=1

echo "ASCEND_HOME_PATH=${ASCEND_HOME_PATH}"
echo "ASCEND_TOOLKIT_HOME=${ASCEND_TOOLKIT_HOME}"
echo "ASCEND_OPP_PATH=${ASCEND_OPP_PATH}"
echo "HCCL_INTRA_ROCE_ENABLE=${HCCL_INTRA_ROCE_ENABLE}"

section "Configure"
rm -rf "${BUILD_DIR}"
cmake -S "${REPO_ROOT}" \
      -B "${BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DASCEND_ROOT="${ASCEND_ROOT}"

section "Build"
cmake --build "${BUILD_DIR}" --target "${TARGET}" -j

section "Linked Libraries"
ldd "${BIN_PATH}" | egrep 'libhccl|libascendcl' || true

section "Run"
"${BIN_PATH}"
