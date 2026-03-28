#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HCCL_REPO="${REPO_ROOT}/hccl"
ASCEND_ROOT="$(bash "${REPO_ROOT}/module/ascendgdrbw/resolve_ascend_root.sh")"
BUILD_OUT_DIR="${HCCL_REPO}/build_out"
CUSTOM_OPS_PATH="./examples/04_custom_ops_p2p"
RUN_PKG_GLOB="cann-hccl_custom_p2p_linux-*.run"
ENABLE_NPU_SMI_TWEAKS="${ENABLE_NPU_SMI_TWEAKS:-1}"

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

append_whitelist_entry_if_missing() {
    local file_path="$1"
    local marker="name:aicpu_hccl_custom_p2p.tar.gz"

    need_file "$file_path"
    if grep -q "${marker}" "$file_path"; then
        echo "whitelist entry already exists in ${file_path}"
        return 0
    fi

    cat >> "$file_path" <<'EOF'
name:aicpu_hccl_custom_p2p.tar.gz
install_path:2
optional:true
package_path:opp/vendors/cust/aicpu/kernel
load_as_per_soc:false
EOF

    echo "appended whitelist entry to ${file_path}"
}

find_whitelist_conf() {
    local candidate
    for candidate in \
        "${ASCEND_ROOT}/conf/ascend_package_load.ini" \
        "/usr/local/Ascend/cann/conf/ascend_package_load.ini"; do
        if [ -f "${candidate}" ]; then
            echo "${candidate}"
            return 0
        fi
    done

    echo "failed to find ascend_package_load.ini" >&2
    exit 1
}

configure_npu_smi_if_needed() {
    if [ "${ENABLE_NPU_SMI_TWEAKS}" != "1" ]; then
        echo "skip npu-smi tweaks because ENABLE_NPU_SMI_TWEAKS=${ENABLE_NPU_SMI_TWEAKS}"
        return 0
    fi

    if ! command -v npu-smi >/dev/null 2>&1; then
        echo "skip npu-smi tweaks because npu-smi is not available"
        return 0
    fi

    section "Configure AICPU Verify"
    for i in 0 1 2 3 4 5 6 7; do
        npu-smi set -t custom-op-secverify-enable -i "$i" -d 1 || true
        npu-smi set -t custom-op-secverify-mode -i "$i" -d 0 || true
    done

    for i in 0 1 2 3 4 5 6 7; do
        npu-smi info -t custom-op-secverify-enable -i "$i" || true
        npu-smi info -t custom-op-secverify-mode -i "$i" || true
    done
}

section "Override Env For latest"
export ASCEND_HOME_PATH="${ASCEND_ROOT}"
export ASCEND_TOOLKIT_HOME="${ASCEND_ROOT}"
export ASCEND_OPP_PATH="${ASCEND_ROOT}/opp"
export ASCEND_CUSTOM_OPP_PATH="${ASCEND_ROOT}/opp"
export ASCEND_CANN_PACKAGE_PATH="${ASCEND_ROOT}"
export CMAKE_PREFIX_PATH="${ASCEND_ROOT}/lib64/cmake"
export ASC_DIR="${ASCEND_ROOT}/lib64/cmake"
export AICPU_DIR="${ASCEND_ROOT}/lib64/cmake"

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

ASCEND_CANN_CONF="$(find_whitelist_conf)"

echo "ASCEND_HOME_PATH=${ASCEND_HOME_PATH}"
echo "ASCEND_TOOLKIT_HOME=${ASCEND_TOOLKIT_HOME}"
echo "ASCEND_OPP_PATH=${ASCEND_OPP_PATH}"
echo "ASCEND_CUSTOM_OPP_PATH=${ASCEND_CUSTOM_OPP_PATH}"
echo "ASCEND_CANN_PACKAGE_PATH=${ASCEND_CANN_PACKAGE_PATH}"
echo "CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
echo "ASC_DIR=${ASC_DIR}"
echo "AICPU_DIR=${AICPU_DIR}"
echo "ASCEND_CANN_CONF=${ASCEND_CANN_CONF}"

section "Check Inputs"
need_file "${HCCL_REPO}"
need_file "${HCCL_REPO}/build.sh"
need_file "${HCCL_REPO}/examples/04_custom_ops_p2p/README.md"

section "Clean Old HCCL Build Dirs"
rm -rf "${HCCL_REPO}/build" "${HCCL_REPO}/build_device" "${HCCL_REPO}/build_out"

section "Build custom_p2p package"
(
    cd "${HCCL_REPO}"
    bash build.sh -p "${ASCEND_ROOT}" --vendor=cust --ops=p2p --custom_ops_path="${CUSTOM_OPS_PATH}"
)

section "Locate run package"
RUN_PKG="$(find "${BUILD_OUT_DIR}" -maxdepth 1 -type f -name "${RUN_PKG_GLOB}" | head -n 1)"
if [ -z "${RUN_PKG}" ]; then
    echo "failed to find ${RUN_PKG_GLOB} under ${BUILD_OUT_DIR}"
    exit 1
fi
echo "RUN_PKG=${RUN_PKG}"

section "Install custom_p2p package"
bash "${RUN_PKG}" --install --quiet

section "Patch whitelist config"
append_whitelist_entry_if_missing "${ASCEND_CANN_CONF}"

configure_npu_smi_if_needed

section "Check custom_p2p installation"
bash "${REPO_ROOT}/module/ascendgdrbw/check_hccl_custom_p2p_latest.sh"

section "Run official custom_p2p example"
bash "${REPO_ROOT}/module/ascendgdrbw/run_hccl_custom_p2p_example_latest.sh"
