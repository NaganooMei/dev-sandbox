#!/usr/bin/env bash

set -euo pipefail

ASCEND_ROOT="/usr/local/Ascend/ascend-toolkit/latest"
CUSTOM_P2P_ROOT="${ASCEND_ROOT}/opp/vendors/cust"
CONF_PATH="/usr/local/Ascend/cann/conf/ascend_package_load.ini"

section() {
    echo
    echo "========== $1 =========="
}

show_path() {
    local path="$1"
    if [ -e "$path" ]; then
        echo "[OK] $path"
        ls -ld "$path"
    else
        echo "[MISSING] $path"
    fi
}

section "Base Paths"
show_path "${ASCEND_ROOT}"
show_path "${CUSTOM_P2P_ROOT}"

section "Custom P2P Headers And Libraries"
show_path "${CUSTOM_P2P_ROOT}/include"
show_path "${CUSTOM_P2P_ROOT}/include/hccl_custom_p2p.h"
show_path "${CUSTOM_P2P_ROOT}/lib64"
show_path "${CUSTOM_P2P_ROOT}/lib64/libhccl_custom_p2p.so"

section "AICPU Custom Op Package"
show_path "${CUSTOM_P2P_ROOT}/aicpu/config/libp2p_aicpu_kernel.json"
show_path "${CUSTOM_P2P_ROOT}/aicpu/kernel/aicpu_hccl_custom_p2p.tar.gz"
show_path "${CUSTOM_P2P_ROOT}/scripts/install.sh"

section "Whitelist Config"
if [ -f "${CONF_PATH}" ]; then
    echo "[OK] ${CONF_PATH}"
    grep -n "aicpu_hccl_custom_p2p.tar.gz" "${CONF_PATH}" || echo "(entry not found)"
else
    echo "[MISSING] ${CONF_PATH}"
fi

section "Linked Symbol"
if [ -f "${CUSTOM_P2P_ROOT}/lib64/libhccl_custom_p2p.so" ]; then
    nm -D "${CUSTOM_P2P_ROOT}/lib64/libhccl_custom_p2p.so" | egrep "HcclSendCustom|HcclRecvCustom" || true
fi

echo
echo "Check finished."
