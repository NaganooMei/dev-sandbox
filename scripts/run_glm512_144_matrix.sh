#!/usr/bin/env bash

set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)

COPY_BIN=${COPY_BIN:-"${REPO_ROOT}/build/module/copy/copy"}
LOG_ROOT=${LOG_ROOT:-"${REPO_ROOT}/logs/glm512_144_matrix"}
RUN_ID=${RUN_ID:-$(date +%Y%m%d_%H%M%S)}
OUTPUT_DIR="${LOG_ROOT}/${RUN_ID}"

readonly ITERATIONS=128
readonly BLOCKS=512
readonly FFTS_LANES=3
readonly PROCESS_SYNC=barrier
readonly STREAM_START_GATE=off

methods=(ce ffts)
host_modes=(one_share all)
devices_list=(1 8 16)
streams_list=(1 4 16)
submit_modes=(stream-major round-robin)
stream_sync_modes=(event stream)

total_runs=$((${#methods[@]} * ${#host_modes[@]} * ${#devices_list[@]} *
             ${#streams_list[@]} * ${#submit_modes[@]} *
             ${#stream_sync_modes[@]}))
readonly EXPECTED_RUNS=144

if ((total_runs != EXPECTED_RUNS)); then
    echo "matrix definition error: expected ${EXPECTED_RUNS} runs, got ${total_runs}" >&2
    exit 1
fi

if [[ ! -x "${COPY_BIN}" ]]; then
    echo "copy binary is missing or not executable: ${COPY_BIN}" >&2
    echo "build it first with: cmake --build ${REPO_ROOT}/build -j" >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"

SUMMARY_FILE="${OUTPUT_DIR}/summary.tsv"
ALL_LOG="${OUTPUT_DIR}/all.log"

printf 'method\thost_mode\tcase_name\tdevices\tstreams\tsubmit_mode\tstream_sync\tprocess_sync\tstream_start_gate\tblocks\titerations\texit_code\tdev_bw_gbps\tstart_skew_avg_us\tgroup_wall_avg_us\twall_bw_gbps\tlog\n' \
    >"${SUMMARY_FILE}"

extract_last()
{
    local pattern=$1
    local file=$2
    local value
    value=$(sed -nE "${pattern}" "${file}" | tail -n 1)
    printf '%s' "${value:-NA}"
}

case_name_for()
{
    local method=$1
    local host_mode=$2

    if [[ "${method}" == "ce" && "${host_mode}" == "one_share" ]]; then
        printf '%s' one_share_host_to_all_device_ce_multi_stream
    elif [[ "${method}" == "ce" && "${host_mode}" == "all" ]]; then
        printf '%s' all_host_to_all_device_ce_multi_stream
    elif [[ "${method}" == "ffts" && "${host_mode}" == "one_share" ]]; then
        printf '%s' one_share_host_to_all_device_ffts_direct_h2d
    elif [[ "${method}" == "ffts" && "${host_mode}" == "all" ]]; then
        printf '%s' all_host_to_all_device_ffts_direct_h2d
    else
        echo "unsupported matrix entry: method=${method} host_mode=${host_mode}" >&2
        return 1
    fi
}

run_index=0
failed_runs=0

printf 'matrix_start total=%d blocks=%d iterations=%d process_sync=%s stream_start_gate=%s\n' \
    "${total_runs}" "${BLOCKS}" "${ITERATIONS}" "${PROCESS_SYNC}" \
    "${STREAM_START_GATE}" | tee "${ALL_LOG}"

# Keep event/stream pairs adjacent, then compare CE/FFTS and host layouts at the same shape.
for devices in "${devices_list[@]}"; do
    for streams in "${streams_list[@]}"; do
        for submit_mode in "${submit_modes[@]}"; do
            for host_mode in "${host_modes[@]}"; do
                for method in "${methods[@]}"; do
                    case_name=$(case_name_for "${method}" "${host_mode}") || exit 1

                    for stream_sync in "${stream_sync_modes[@]}"; do
                        run_index=$((run_index + 1))
                        safe_submit=${submit_mode//-/_}
                        log_file="${OUTPUT_DIR}/${method}_${host_mode}_d${devices}_s${streams}_${safe_submit}_${stream_sync}.log"

                        command=(
                            "${COPY_BIN}"
                            -t "${case_name}"
                            --io-mode glm5.1
                            -f 3
                            -n "${BLOCKS}"
                            -S "${streams}"
                            --submit-mode "${submit_mode}"
                            --process-sync "${PROCESS_SYNC}"
                            --stream-start-gate "${STREAM_START_GATE}"
                            --stream-sync "${stream_sync}"
                            -i "${ITERATIONS}"
                            -d "${devices}"
                        )

                        printf -v command_text '%q ' "${command[@]}"
                        if [[ "${method}" == "ffts" ]]; then
                            command_text="FFTS_MAX_READY_LANES=${FFTS_LANES} ${command_text}"
                        fi

                        {
                            printf '\n[%d/%d] method=%s host=%s devices=%s streams=%s submit=%s stream_sync=%s\n' \
                                "${run_index}" "${total_runs}" "${method}" "${host_mode}" \
                                "${devices}" "${streams}" "${submit_mode}" "${stream_sync}"
                            printf 'command: %s\n' "${command_text}"
                        } | tee "${log_file}" | tee -a "${ALL_LOG}"

                        if [[ "${method}" == "ffts" ]]; then
                            FFTS_MAX_READY_LANES="${FFTS_LANES}" "${command[@]}" 2>&1 \
                                | tee -a "${log_file}" | tee -a "${ALL_LOG}"
                        else
                            "${command[@]}" 2>&1 \
                                | tee -a "${log_file}" | tee -a "${ALL_LOG}"
                        fi
                        rc=${PIPESTATUS[0]}

                        dev_bw=$(awk \
                            '/acl::device::all/ && $0 !~ /ProcessSync/ {value=$NF} END {print value}' \
                            "${log_file}")
                        dev_bw=${dev_bw:-NA}
                        start_skew_avg=$(extract_last \
                            's/.*StartSkew\(us\)-\(Min\/Max\/Avg\/P50\/P90\)=[^ ]+ \/ [^ ]+ \/ ([^ ]+).*/\1/p' \
                            "${log_file}")
                        group_wall_avg=$(extract_last \
                            's/.*GroupWall\(us\)-\(Min\/Max\/Avg\/P50\/P90\)=[^ ]+ \/ [^ ]+ \/ ([^ ]+).*/\1/p' \
                            "${log_file}")
                        wall_bw=$(extract_last \
                            's/.*WallBW\(GB\/s\)=([0-9.]+).*/\1/p' "${log_file}")

                        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                            "${method}" "${host_mode}" "${case_name}" "${devices}" \
                            "${streams}" "${submit_mode}" "${stream_sync}" \
                            "${PROCESS_SYNC}" "${STREAM_START_GATE}" "${BLOCKS}" \
                            "${ITERATIONS}" "${rc}" "${dev_bw}" "${start_skew_avg}" \
                            "${group_wall_avg}" "${wall_bw}" "${log_file}" \
                            >>"${SUMMARY_FILE}"

                        if ((rc != 0)); then
                            failed_runs=$((failed_runs + 1))
                            echo "FAILED: ${command_text}" | tee -a "${ALL_LOG}" >&2
                        fi
                    done
                done
            done
        done
    done
done

echo
echo "matrix_complete total=${total_runs} failed=${failed_runs}"
echo "summary=${SUMMARY_FILE}"
echo "all_log=${ALL_LOG}"

if ((failed_runs != 0)); then
    exit 1
fi
