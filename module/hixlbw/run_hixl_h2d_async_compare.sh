#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "${SCRIPT_DIR}/../.." && pwd)

BUILD_SCRIPT=${BUILD_SCRIPT:-"${SCRIPT_DIR}/build_hixlbw.sh"}
BUILD_DIR=${BUILD_DIR:-"${ROOT_DIR}/build-hixlbw"}
BIN_PATH="${BUILD_DIR}/module/hixlbw/hixlbw_h2d_async_compare"

CLIENT_DEVICE=${CLIENT_DEVICE:-0}
SERVER_DEVICE=${SERVER_DEVICE:-1}
CLIENT_ENGINE=${CLIENT_ENGINE:-127.0.0.1}
SERVER_ENGINE=${SERVER_ENGINE:-127.0.0.1:16001}
METADATA_FILE=${METADATA_FILE:-"${BUILD_DIR}/hixlbw_metadata.txt"}

TOTAL_BYTES=${TOTAL_BYTES:-134217728}
MIN_BLOCK_BYTES=${MIN_BLOCK_BYTES:-262144}
MAX_BLOCK_BYTES=${MAX_BLOCK_BYTES:-8388608}
REPEATS=${REPEATS:-5}
CONNECT_TIMEOUT_MS=${CONNECT_TIMEOUT_MS:-10000}
COMPLETION_TIMEOUT_MS=${COMPLETION_TIMEOUT_MS:-60000}
METADATA_TIMEOUT_MS=${METADATA_TIMEOUT_MS:-60000}
POLL_INTERVAL_US=${POLL_INTERVAL_US:-50}
export HCCL_INTRA_ROCE_ENABLE=${HCCL_INTRA_ROCE_ENABLE:-1}

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

HIXL_ROOT=${HIXL_ROOT:-"${ROOT_DIR}/hixl"}
HIXL_BUILD_DIR=${HIXL_BUILD_DIR:-"${HIXL_ROOT}/build"}
HIXL_LIB_DIR="${HIXL_BUILD_DIR}/src/llm_datadist"

export LD_LIBRARY_PATH="${HIXL_LIB_DIR}:${ASCEND_ROOT}/lib64:${ASCEND_ROOT}/runtime/lib64:${ASCEND_ROOT}/lib64/stub:${ASCEND_ROOT}/runtime/lib64/stub:${LD_LIBRARY_PATH:-}"

bash "${BUILD_SCRIPT}"

if [[ ! -x "${BIN_PATH}" ]]; then
    echo "binary not found: ${BIN_PATH}" >&2
    exit 1
fi

mkdir -p "$(dirname "${METADATA_FILE}")"
rm -f "${METADATA_FILE}" "${METADATA_FILE}.tmp" "${METADATA_FILE}.done" "${METADATA_FILE}.done.tmp"

SERVER_PID=
cleanup() {
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
        kill "${SERVER_PID}" >/dev/null 2>&1 || true
        wait "${SERVER_PID}" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

echo "path,total_bytes,block_bytes,transfer_count,repeats,submit_us_avg,total_us_avg,bandwidth_gib_s,poll_count_avg"
echo "[hixlbw-run] CLIENT_DEVICE=${CLIENT_DEVICE} SERVER_DEVICE=${SERVER_DEVICE}" >&2
echo "[hixlbw-run] CLIENT_ENGINE=${CLIENT_ENGINE} SERVER_ENGINE=${SERVER_ENGINE}" >&2
echo "[hixlbw-run] METADATA_FILE=${METADATA_FILE}" >&2

"${BIN_PATH}" \
    --role server \
    --device "${SERVER_DEVICE}" \
    --local-engine "${SERVER_ENGINE}" \
    --metadata-file "${METADATA_FILE}" \
    --total-bytes "${TOTAL_BYTES}" \
    --min-block-bytes "${MIN_BLOCK_BYTES}" \
    --max-block-bytes "${MAX_BLOCK_BYTES}" \
    --repeats "${REPEATS}" \
    --connect-timeout-ms "${CONNECT_TIMEOUT_MS}" \
    --completion-timeout-ms "${COMPLETION_TIMEOUT_MS}" \
    --metadata-timeout-ms "${METADATA_TIMEOUT_MS}" \
    >"${BUILD_DIR}/hixlbw_server.log" 2>&1 &
SERVER_PID=$!
echo "[hixlbw-run] server pid=${SERVER_PID}" >&2

for _ in $(seq 1 600); do
    if [[ -f "${METADATA_FILE}" ]]; then
        break
    fi
    if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
        echo "server exited before creating metadata file: ${METADATA_FILE}" >&2
        if [[ -f "${BUILD_DIR}/hixlbw_server.log" ]]; then
            cat "${BUILD_DIR}/hixlbw_server.log" >&2
        fi
        exit 1
    fi
    sleep 0.1
done

if [[ ! -f "${METADATA_FILE}" ]]; then
    echo "metadata file was not created: ${METADATA_FILE}" >&2
    if [[ -f "${BUILD_DIR}/hixlbw_server.log" ]]; then
        cat "${BUILD_DIR}/hixlbw_server.log" >&2
    fi
    exit 1
fi

"${BIN_PATH}" \
    --role client \
    --device "${CLIENT_DEVICE}" \
    --local-engine "${CLIENT_ENGINE}" \
    --remote-engine "${SERVER_ENGINE}" \
    --metadata-file "${METADATA_FILE}" \
    --total-bytes "${TOTAL_BYTES}" \
    --min-block-bytes "${MIN_BLOCK_BYTES}" \
    --max-block-bytes "${MAX_BLOCK_BYTES}" \
    --repeats "${REPEATS}" \
    --connect-timeout-ms "${CONNECT_TIMEOUT_MS}" \
    --completion-timeout-ms "${COMPLETION_TIMEOUT_MS}" \
    --metadata-timeout-ms "${METADATA_TIMEOUT_MS}" \
    --poll-interval-us "${POLL_INTERVAL_US}"

wait "${SERVER_PID}"
SERVER_PID=

"${BIN_PATH}" \
    --role acl \
    --device "${SERVER_DEVICE}" \
    --total-bytes "${TOTAL_BYTES}" \
    --min-block-bytes "${MIN_BLOCK_BYTES}" \
    --max-block-bytes "${MAX_BLOCK_BYTES}" \
    --repeats "${REPEATS}"
