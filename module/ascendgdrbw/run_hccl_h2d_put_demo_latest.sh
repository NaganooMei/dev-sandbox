#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ASCEND_ROOT="/usr/local/Ascend/ascend-toolkit/latest"
BUILD_DIR="${REPO_ROOT}/build-latest"
TARGET="ascendgdrbw_hccl_h2d_put_demo"
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

section "Override Env For latest"
export ASCEND_HOME_PATH="${ASCEND_ROOT}"
export ASCEND_TOOLKIT_HOME="${ASCEND_ROOT}"
export ASCEND_OPP_PATH="${ASCEND_ROOT}/opp"

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

echo "ASCEND_HOME_PATH=${ASCEND_HOME_PATH}"
echo "ASCEND_TOOLKIT_HOME=${ASCEND_TOOLKIT_HOME}"
echo "ASCEND_OPP_PATH=${ASCEND_OPP_PATH}"

section "Configure"
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
