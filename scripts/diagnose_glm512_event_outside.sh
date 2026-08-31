#!/usr/bin/env bash

set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)

COPY_BIN=${COPY_BIN:-"${REPO_ROOT}/build/module/copy/copy"}
LOG_ROOT=${LOG_ROOT:-"${REPO_ROOT}/logs/glm512_event_outside"}
RUN_ID=${RUN_ID:-$(date +%Y%m%d_%H%M%S)}
OUTPUT_DIR="${LOG_ROOT}/${RUN_ID}"

REPEATS=${REPEATS:-3}
ITERATIONS=${ITERATIONS:-128}
DEVICES=${DEVICES:-16}
STREAMS=${STREAMS:-"16"}
HOST_MODES=${HOST_MODES:-"all"}
METHODS=${METHODS:-"ce ffts"}
SUBMIT_MODES=${SUBMIT_MODES:-${SUBMIT_MODE:-"stream-major round-robin"}}
STREAM_SYNC_MODES=${STREAM_SYNC_MODES:-"event stream"}

readonly BLOCKS=512
readonly FFTS_LANES=3
readonly PROCESS_SYNC=barrier
readonly STREAM_START_GATE=off
readonly GLM51_BLOCK_BYTES=$((176 * 1024))

read -r -a streams_list <<<"${STREAMS}"
read -r -a host_modes <<<"${HOST_MODES}"
read -r -a methods <<<"${METHODS}"
read -r -a submit_modes <<<"${SUBMIT_MODES}"
read -r -a stream_sync_modes <<<"${STREAM_SYNC_MODES}"

if ((REPEATS <= 0 || ITERATIONS <= 0 || DEVICES <= 0)); then
    echo "REPEATS, ITERATIONS, and DEVICES must be positive" >&2
    exit 1
fi

if [[ ! -x "${COPY_BIN}" ]]; then
    echo "copy binary is missing or not executable: ${COPY_BIN}" >&2
    echo "build it first with: cmake --build ${REPO_ROOT}/build -j" >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"

SUMMARY_FILE="${OUTPUT_DIR}/summary.tsv"
AGGREGATE_FILE="${OUTPUT_DIR}/aggregate.tsv"
ALL_LOG="${OUTPUT_DIR}/all.log"

printf 'repeat\tmethod\thost_mode\tcase_name\tdevices\tstreams\tsubmit_mode\tstream_sync\texit_code\tdev_bw_gbps\tgroup_wall_avg_us\twall_bw_gbps\tdevice_event_avg_us\toutside_event_avg_us\tphase_devices\tset_device_avg_us\trecord_start_avg_us\tarm_start_avg_us\tsubmit_avg_us\tfan_in_avg_us\tsynchronize_avg_us\tsynchronize_p90_us\trelease_avg_us\telapsed_query_avg_us\ttotal_host_avg_us\tsynchronize_minus_device_event_us\tgroup_wall_minus_total_host_us\tsynchronize_share_of_outside_pct\tlog\n' \
    >"${SUMMARY_FILE}"

extract_last()
{
    local pattern=$1
    local file=$2
    local value
    value=$(sed -nE "${pattern}" "${file}" | tail -n 1)
    printf '%s' "${value:-NA}"
}

extract_phase_max()
{
    local field=$1
    local file=$2
    awk -v key="${field}=" '
        $1 == "ASCEND_PHASE" {
            for (i = 1; i <= NF; ++i) {
                if (index($i, key) == 1) {
                    split($i, parts, "=")
                    value = parts[2] + 0
                    if (!found || value > maximum) maximum = value
                    found = 1
                }
            }
        }
        END {
            if (found) printf "%.0f", maximum
            else printf "NA"
        }
    ' "${file}"
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
        echo "unsupported entry: method=${method} host_mode=${host_mode}" >&2
        return 1
    fi
}

total_runs=$((${#methods[@]} * ${#host_modes[@]} * ${#streams_list[@]} *
             ${#submit_modes[@]} *
             ${#stream_sync_modes[@]} * REPEATS))
run_index=0
failed_runs=0

printf 'diagnosis_start total=%d repeats=%d devices=%d streams="%s" iterations=%d submit_modes="%s" process_sync=%s stream_start_gate=%s\n' \
    "${total_runs}" "${REPEATS}" "${DEVICES}" "${STREAMS}" "${ITERATIONS}" \
    "${SUBMIT_MODES}" "${PROCESS_SYNC}" "${STREAM_START_GATE}" | tee "${ALL_LOG}"

for repeat in $(seq 1 "${REPEATS}"); do
    for streams in "${streams_list[@]}"; do
        for submit_mode in "${submit_modes[@]}"; do
            safe_submit=${submit_mode//-/_}
            for host_mode in "${host_modes[@]}"; do
                for method in "${methods[@]}"; do
                    case_name=$(case_name_for "${method}" "${host_mode}") || exit 1
                    for stream_sync in "${stream_sync_modes[@]}"; do
                    run_index=$((run_index + 1))
                    log_file="${OUTPUT_DIR}/r${repeat}_${method}_${host_mode}_d${DEVICES}_s${streams}_${safe_submit}_${stream_sync}.log"
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
                        -d "${DEVICES}"
                    )

                    printf -v command_text '%q ' "${command[@]}"
                    if [[ "${method}" == "ffts" ]]; then
                        command_text="COPY_ASCEND_PHASE_TRACE=1 FFTS_MAX_READY_LANES=${FFTS_LANES} ${command_text}"
                    else
                        command_text="COPY_ASCEND_PHASE_TRACE=1 ${command_text}"
                    fi

                    {
                        printf '\n[%d/%d] repeat=%d method=%s host=%s devices=%d streams=%s submit=%s sync=%s\n' \
                            "${run_index}" "${total_runs}" "${repeat}" "${method}" \
                            "${host_mode}" "${DEVICES}" "${streams}" "${submit_mode}" \
                            "${stream_sync}"
                        printf 'command: %s\n' "${command_text}"
                    } | tee "${log_file}" | tee -a "${ALL_LOG}"

                    if [[ "${method}" == "ffts" ]]; then
                        COPY_ASCEND_PHASE_TRACE=1 FFTS_MAX_READY_LANES="${FFTS_LANES}" \
                            "${command[@]}" 2>&1 | tee -a "${log_file}" | tee -a "${ALL_LOG}"
                        rc=${PIPESTATUS[0]}
                    else
                        COPY_ASCEND_PHASE_TRACE=1 "${command[@]}" 2>&1 \
                            | tee -a "${log_file}" | tee -a "${ALL_LOG}"
                        rc=${PIPESTATUS[0]}
                    fi

                    dev_bw=$(awk \
                        '/acl::device::all/ && $0 !~ /ProcessSync/ {value=$NF} END {print value}' \
                        "${log_file}")
                    dev_bw=${dev_bw:-NA}
                    group_wall_avg=$(extract_last \
                        's/.*GroupWall\(us\)-\(Min\/Max\/Avg\/P50\/P90\)=[^ ]+ \/ [^ ]+ \/ ([^ ]+).*/\1/p' \
                        "${log_file}")
                    wall_bw=$(extract_last \
                        's/.*WallBW\(GB\/s\)=([0-9.]+).*/\1/p' "${log_file}")

                    phase_devices=$(awk '$1 == "ASCEND_PHASE" {++count} END {print count + 0}' \
                        "${log_file}")
                    set_device_avg=$(extract_phase_max set_device_avg_us "${log_file}")
                    record_start_avg=$(extract_phase_max record_start_avg_us "${log_file}")
                    arm_start_avg=$(extract_phase_max arm_start_avg_us "${log_file}")
                    submit_avg=$(extract_phase_max submit_avg_us "${log_file}")
                    fan_in_avg=$(extract_phase_max fan_in_avg_us "${log_file}")
                    synchronize_avg=$(extract_phase_max synchronize_avg_us "${log_file}")
                    synchronize_p90=$(extract_phase_max synchronize_p90_us "${log_file}")
                    release_avg=$(extract_phase_max release_avg_us "${log_file}")
                    elapsed_query_avg=$(extract_phase_max elapsed_query_avg_us "${log_file}")
                    total_host_avg=$(extract_phase_max total_host_avg_us "${log_file}")

                    device_event_avg=NA
                    outside_event_avg=NA
                    synchronize_minus_event=NA
                    group_wall_minus_total_host=NA
                    synchronize_share=NA
                    if [[ "${group_wall_avg}" != "NA" && "${total_host_avg}" != "NA" ]]; then
                        group_wall_minus_total_host=$(awk -v wall="${group_wall_avg}" \
                            -v total="${total_host_avg}" 'BEGIN {printf "%.0f", wall - total}')
                    fi
                    if [[ "${dev_bw}" != "NA" && "${dev_bw}" != "N/A" &&
                          "${group_wall_avg}" != "NA" ]]; then
                        device_event_avg=$(awk -v blocks="${BLOCKS}" \
                            -v bytes="${GLM51_BLOCK_BYTES}" -v devices="${DEVICES}" \
                            -v bandwidth="${dev_bw}" \
                            'BEGIN {printf "%.0f", blocks * bytes * devices * 1000000 / (1024^3) / bandwidth}')
                        outside_event_avg=$(awk -v wall="${group_wall_avg}" \
                            -v event="${device_event_avg}" 'BEGIN {printf "%.0f", wall - event}')
                        if [[ "${synchronize_avg}" != "NA" ]]; then
                            synchronize_minus_event=$(awk -v sync="${synchronize_avg}" \
                                -v event="${device_event_avg}" 'BEGIN {printf "%.0f", sync - event}')
                            synchronize_share=$(awk -v sync="${synchronize_avg}" \
                                -v outside="${outside_event_avg}" \
                                'BEGIN {if (outside > 0) printf "%.1f", sync * 100 / outside; else print "NA"}')
                        fi
                    fi

                    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                        "${repeat}" "${method}" "${host_mode}" "${case_name}" \
                        "${DEVICES}" "${streams}" "${submit_mode}" "${stream_sync}" \
                        "${rc}" "${dev_bw}" "${group_wall_avg}" "${wall_bw}" \
                        "${device_event_avg}" "${outside_event_avg}" "${phase_devices}" \
                        "${set_device_avg}" "${record_start_avg}" "${arm_start_avg}" \
                        "${submit_avg}" "${fan_in_avg}" "${synchronize_avg}" \
                        "${synchronize_p90}" "${release_avg}" "${elapsed_query_avg}" \
                        "${total_host_avg}" "${synchronize_minus_event}" \
                        "${group_wall_minus_total_host}" "${synchronize_share}" "${log_file}" \
                        >>"${SUMMARY_FILE}"

                    if ((rc != 0)); then
                        failed_runs=$((failed_runs + 1))
                        echo "FAILED: ${command_text}" | tee -a "${ALL_LOG}" >&2
                    elif ((phase_devices != DEVICES)); then
                        failed_runs=$((failed_runs + 1))
                        echo "FAILED: expected ${DEVICES} ASCEND_PHASE lines, got ${phase_devices}" \
                            | tee -a "${ALL_LOG}" >&2
                    fi
                done
            done
        done
    done
done
done

printf 'method\thost_mode\tdevices\tstreams\tsubmit_mode\tstream_sync\truns\tgroup_wall_mean_us\tgroup_wall_min_us\tgroup_wall_max_us\twall_bw_mean_gbps\tdevice_event_mean_us\toutside_event_mean_us\tsubmit_mean_us\tfan_in_mean_us\tsynchronize_mean_us\tsynchronize_p90_max_us\trelease_mean_us\telapsed_query_mean_us\ttotal_host_mean_us\tsynchronize_minus_device_event_mean_us\tgroup_wall_minus_total_host_mean_us\n' \
    >"${AGGREGATE_FILE}"

awk -F '\t' '
    NR == 1 {next}
    $9 == 0 {
        key = $2 SUBSEP $3 SUBSEP $5 SUBSEP $6 SUBSEP $7 SUBSEP $8
        method[key] = $2
        host[key] = $3
        devices[key] = $5
        streams[key] = $6
        submit_mode[key] = $7
        sync_mode[key] = $8
        runs[key]++
        group_sum[key] += $11
        wall_bw_sum[key] += $12
        submit_sum[key] += $19
        fan_in_sum[key] += $20
        synchronize_sum[key] += $21
        release_sum[key] += $23
        elapsed_sum[key] += $24
        total_host_sum[key] += $25
        if (!(key in group_min) || $11 < group_min[key]) group_min[key] = $11
        if (!(key in group_max) || $11 > group_max[key]) group_max[key] = $11
        if ($13 != "NA") {
            event_sum[key] += $13
            outside_sum[key] += $14
            synchronize_minus_event_sum[key] += $26
            event_runs[key]++
        }
        group_minus_total_sum[key] += $27
        if ($22 > synchronize_p90_max[key]) synchronize_p90_max[key] = $22
    }
    END {
        for (key in runs) {
            event_mean = event_runs[key] ? event_sum[key] / event_runs[key] : "NA"
            outside_mean = event_runs[key] ? outside_sum[key] / event_runs[key] : "NA"
            synchronize_minus_event_mean = event_runs[key] ? \
                synchronize_minus_event_sum[key] / event_runs[key] : "NA"
            printf "%s\t%s\t%s\t%s\t%s\t%s\t%d\t%.0f\t%.0f\t%.0f\t%.3f\t%s\t%s\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%s\t%.0f\n", \
                method[key], host[key], devices[key], streams[key], submit_mode[key], \
                sync_mode[key], runs[key], group_sum[key] / runs[key], group_min[key], \
                group_max[key], \
                wall_bw_sum[key] / runs[key], event_mean, outside_mean, \
                submit_sum[key] / runs[key], fan_in_sum[key] / runs[key], \
                synchronize_sum[key] / runs[key], synchronize_p90_max[key], \
                release_sum[key] / runs[key], elapsed_sum[key] / runs[key], \
                total_host_sum[key] / runs[key], synchronize_minus_event_mean, \
                group_minus_total_sum[key] / runs[key]
        }
    }
' "${SUMMARY_FILE}" >>"${AGGREGATE_FILE}"

echo
echo "diagnosis_complete total=${total_runs} failed=${failed_runs}"
echo "summary=${SUMMARY_FILE}"
echo "aggregate=${AGGREGATE_FILE}"
echo "all_log=${ALL_LOG}"
echo "Compare outside_event_mean_us with synchronize_minus_device_event_mean_us in aggregate.tsv."

if ((failed_runs != 0)); then
    exit 1
fi
