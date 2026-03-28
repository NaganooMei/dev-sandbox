#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [ -f "/usr/local/Ascend/cann/set_env.sh" ]; then
    set +u
    source /usr/local/Ascend/cann/set_env.sh
    set -u
fi

ASCEND_ROOT="$(bash "${REPO_ROOT}/module/ascendgdrbw/resolve_ascend_root.sh")"
HCCL_REPO="$(bash "${REPO_ROOT}/module/ascendgdrbw/resolve_hccl_custom_p2p_source.sh")"
EXAMPLE_DIR="${HCCL_REPO}/examples/04_custom_ops_p2p/testcase"
CUSTOM_P2P_LIB_DIR="${ASCEND_ROOT}/opp/vendors/cust/lib64"

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

need_file() {
    local path="$1"
    if [ ! -e "$path" ]; then
        echo "missing required path: $path"
        exit 1
    fi
}

section "Select Sources"
echo "ASCEND_ROOT=${ASCEND_ROOT}"
echo "HCCL_REPO=${HCCL_REPO}"

section "Override Env For selected toolkit"
export ASCEND_HOME_PATH="${ASCEND_ROOT}"
export ASCEND_TOOLKIT_HOME="${ASCEND_ROOT}"
export ASCEND_OPP_PATH="${ASCEND_ROOT}/opp"

prepend_path PATH "${ASCEND_ROOT}/bin"
prepend_path PATH "${ASCEND_ROOT}/compiler/ccec_compiler/bin"
prepend_path PATH "${ASCEND_ROOT}/tools/ccec_compiler/bin"
prepend_path LD_LIBRARY_PATH "${ASCEND_ROOT}/lib64"
prepend_path LD_LIBRARY_PATH "${ASCEND_ROOT}/lib64/plugin/opskernel"
prepend_path LD_LIBRARY_PATH "${ASCEND_ROOT}/lib64/plugin/nnengine"
prepend_path LD_LIBRARY_PATH "${CUSTOM_P2P_LIB_DIR}"
prepend_path LD_LIBRARY_PATH "/usr/local/Ascend/driver/lib64"
prepend_path LD_LIBRARY_PATH "/usr/local/Ascend/driver/lib64/common"
prepend_path LD_LIBRARY_PATH "/usr/local/Ascend/driver/lib64/driver"

echo "ASCEND_HOME_PATH=${ASCEND_HOME_PATH}"
echo "ASCEND_TOOLKIT_HOME=${ASCEND_TOOLKIT_HOME}"
echo "ASCEND_OPP_PATH=${ASCEND_OPP_PATH}"

section "Check Required Files"
need_file "${EXAMPLE_DIR}"
need_file "${EXAMPLE_DIR}/Makefile"
need_file "${ASCEND_ROOT}/opp/vendors/cust/include/hccl_custom_p2p.h"
need_file "${CUSTOM_P2P_LIB_DIR}/libhccl_custom_p2p.so"

section "Build Example"
make -C "${EXAMPLE_DIR}" clean || true
make -C "${EXAMPLE_DIR}"

section "Run Example"
make -C "${EXAMPLE_DIR}" test
