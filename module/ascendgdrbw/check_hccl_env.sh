#!/usr/bin/env bash

set -u

ASCEND_CANN_ENV="/usr/local/Ascend/cann/set_env.sh"
ASCEND_TOOLKIT_DEFAULT="/usr/local/Ascend/ascend-toolkit/latest"
DEFAULT_DEMO="./build/module/ascendgdrbw/ascendgdrbw_hccl_h2d_put_demo"
DEMO_PATH="${1:-$DEFAULT_DEMO}"

section() {
    echo
    echo "========== $1 =========="
}

run_cmd() {
    echo "+ $*"
    "$@"
    local status=$?
    if [ "$status" -ne 0 ]; then
        echo "(exit=$status)"
    fi
    return 0
}

run_shell() {
    echo "+ $1"
    bash -lc "$1"
    local status=$?
    if [ "$status" -ne 0 ]; then
        echo "(exit=$status)"
    fi
    return 0
}

show_var() {
    local name="$1"
    local value="${!name-}"
    if [ -n "$value" ]; then
        echo "$name=$value"
    else
        echo "$name=<unset>"
    fi
}

grep_symbol() {
    local lib_path="$1"
    local symbol="$2"
    echo "+ nm -D $lib_path | grep $symbol"
    if nm -D "$lib_path" 2>/dev/null | grep -n "$symbol"; then
        return 0
    fi
    echo "(symbol not found)"
    return 0
}

section "Host"
run_cmd date
run_cmd uname -a
run_cmd whoami
run_shell "pwd"

section "Source CANN Env"
if [ -f "$ASCEND_CANN_ENV" ]; then
    echo "+ source $ASCEND_CANN_ENV"
    # shellcheck disable=SC1090
    source "$ASCEND_CANN_ENV"
else
    echo "missing: $ASCEND_CANN_ENV"
fi

section "Key Env Vars"
show_var ASCEND_HOME_PATH
show_var ASCEND_TOOLKIT_HOME
show_var ASCEND_OPP_PATH
show_var LD_LIBRARY_PATH
show_var PATH
show_var PYTHONPATH
show_var RANK_TABLE_FILE
show_var HCCL_OP_EXPANSION_MODE

section "Ascend Toolkit Files"
run_shell "ls -ld $ASCEND_TOOLKIT_DEFAULT"
run_shell "ls -l $ASCEND_TOOLKIT_DEFAULT/lib64/libascendcl.so*"
run_shell "ls -l $ASCEND_TOOLKIT_DEFAULT/lib64/libhccl.so*"

section "Device State"
run_shell "command -v npu-smi"
run_shell "npu-smi info"

section "RDMA / RoCE"
run_shell "command -v ibv_devices"
run_shell "command -v ibv_devinfo"
run_shell "ibv_devices"
run_shell "ibv_devinfo -l"
run_shell "ls /sys/class/infiniband"
run_shell "lsmod | egrep 'mlx5|ib_uverbs|ib_core|rdma_cm|ib_cm|mlx5_ib|hns|roce'"

section "HCCL Symbols"
HCCL_LIB_PATH="$ASCEND_TOOLKIT_DEFAULT/lib64/libhccl.so"
if [ -f "$HCCL_LIB_PATH" ]; then
    grep_symbol "$HCCL_LIB_PATH" "HcclCommInitRootInfo"
    grep_symbol "$HCCL_LIB_PATH" "HcclRegisterMem"
    grep_symbol "$HCCL_LIB_PATH" "HcclExchangeMemDesc"
    grep_symbol "$HCCL_LIB_PATH" "HcclEnableMemAccess"
    grep_symbol "$HCCL_LIB_PATH" "HcclBatchPut"
else
    echo "missing: $HCCL_LIB_PATH"
fi

section "Demo Binary"
if [ -f "$DEMO_PATH" ]; then
    run_shell "ls -l $DEMO_PATH"
    run_shell "ldd $DEMO_PATH"
else
    echo "demo not found: $DEMO_PATH"
fi

section "Kernel / Driver Logs"
run_shell "dmesg | egrep -i 'ascend|hccl|hccn|roce|rdma|mlx5|ib_core|ib_uverbs' | tail -n 200"

echo
echo "Check finished."
