#!/usr/bin/env bash

set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)

COPY_BIN=${COPY_BIN:-"${REPO_ROOT}/build/module/copy/copy"}
LOG_ROOT=${LOG_ROOT:-"${REPO_ROOT}/logs/glm512_anomaly_repeats"}
RUN_ID=${RUN_ID:-$(date +%Y%m%d_%H%M%S)}
REPEATS=${REPEATS:-3}
ITERATIONS=${ITERATIONS:-128}

DIAGNOSE_SCRIPT="${SCRIPT_DIR}/diagnose_glm512_event_outside.sh"
OUTPUT_DIR="${LOG_ROOT}/${RUN_ID}"
PROFILE_FILE="${OUTPUT_DIR}/profiles.tsv"

if [[ ! -x "${COPY_BIN}" ]]; then
    echo "copy binary is missing or not executable: ${COPY_BIN}" >&2
    echo "build it first with: cmake --build ${REPO_ROOT}/build -j" >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"
printf 'profile\treason\tdevices\tstreams\tmethods\thost_modes\tsubmit_modes\tstream_sync_modes\trepeats\tresult\n' \
    >"${PROFILE_FILE}"

failed_profiles=0

run_profile()
{
    local profile=$1
    local reason=$2
    local devices=$3
    local streams=$4
    local methods=$5
    local host_modes=$6
    local submit_modes=$7
    local stream_sync_modes=$8
    local result_dir="${OUTPUT_DIR}/${profile}"

    echo
    echo "profile=${profile}"
    echo "reason=${reason}"
    echo "devices=${devices} streams=${streams} methods=${methods} host_modes=${host_modes} submit_modes=${submit_modes} stream_sync_modes=${stream_sync_modes} repeats=${REPEATS}"

    COPY_BIN="${COPY_BIN}" \
        LOG_ROOT="${OUTPUT_DIR}" \
        RUN_ID="${profile}" \
        REPEATS="${REPEATS}" \
        ITERATIONS="${ITERATIONS}" \
        DEVICES="${devices}" \
        STREAMS="${streams}" \
        METHODS="${methods}" \
        HOST_MODES="${host_modes}" \
        SUBMIT_MODES="${submit_modes}" \
        STREAM_SYNC_MODES="${stream_sync_modes}" \
        bash "${DIAGNOSE_SCRIPT}"
    local rc=$?

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${profile}" "${reason}" "${devices}" "${streams}" "${methods}" \
        "${host_modes}" "${submit_modes}" "${stream_sync_modes}" "${REPEATS}" \
        "${result_dir}/aggregate.tsv" >>"${PROFILE_FILE}"

    if ((rc != 0)); then
        failed_profiles=$((failed_profiles + 1))
        echo "FAILED profile=${profile}" >&2
    fi
}

# streams=1 makes stream-major and round-robin semantically equivalent. Keep the
# two shapes with the largest and most useful submit-mode discrepancies.
run_profile ce_one_share_d8_s1_submit_drift \
    "streams=1 submit modes differed by up to 32 percent" \
    8 "1" "ce" "one_share" "stream-major round-robin" "event stream"

run_profile ffts_all_d16_s1_submit_drift \
    "streams=1 event results differed by 38 percent" \
    16 "1" "ffts" "all" "stream-major round-robin" "event stream"

# FFTS all-host throughput fell by about 23 percent at 16 streams versus 4 streams.
run_profile ffts_all_d16_stream_knee \
    "16 streams regressed versus 4 streams" \
    16 "4 16" "ffts" "all" "stream-major round-robin" "stream"

# FFTS one-share throughput plateaued around 104 GiB/s from 8 to 16 devices.
run_profile ffts_one_share_d8_plateau \
    "one-share scaling plateau baseline at 8 devices" \
    8 "4 16" "ffts" "one_share" "round-robin" "stream"

run_profile ffts_one_share_d16_plateau \
    "one-share scaling plateau check at 16 devices" \
    16 "4 16" "ffts" "one_share" "round-robin" "stream"

echo
echo "anomaly_rerun_complete profiles=5 failed=${failed_profiles}"
echo "profiles=${PROFILE_FILE}"

if ((failed_profiles != 0)); then
    exit 1
fi
