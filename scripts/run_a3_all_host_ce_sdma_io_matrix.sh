#!/usr/bin/env bash

set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)

COPY_BIN=${COPY_BIN:-"${REPO_ROOT}/build/module/copy/copy"}
LOG_ROOT=${LOG_ROOT:-"${REPO_ROOT}/logs/a3_all_host_ce_sdma_io_matrix"}
RUN_ID=${RUN_ID:-$(date +%Y%m%d_%H%M%S)}
OUTPUT_DIR="${LOG_ROOT}/${RUN_ID}"
DRY_RUN=${DRY_RUN:-0}

readonly CE_CASE=all_host_to_all_device_ce_multi_stream
readonly SDMA_CASE=all_host_to_all_device_ffts_direct_h2d
readonly IO_NUM=512
readonly ITERATIONS=128
readonly SDMA_READY_LANES=3
readonly SUBMIT_MODE=round-robin
readonly PROCESS_SYNC=barrier
readonly STREAM_START_GATE=off
readonly STREAM_SYNC_MODE=stream

# Run the A5-compatible CE matrix first, then the A3 SDMA matrix.
methods=(ce sdma)
io_modes=(glm5.1 1m)
devices_list=(1 4 8)
streams_list=(1 4 16)

total_runs=$((${#methods[@]} * ${#io_modes[@]} * ${#devices_list[@]} *
             ${#streams_list[@]}))
readonly EXPECTED_RUNS=36

if ((total_runs != EXPECTED_RUNS)); then
    echo "matrix definition error: expected ${EXPECTED_RUNS} runs, got ${total_runs}" >&2
    exit 1
fi

if [[ "${DRY_RUN}" != "1" && ! -x "${COPY_BIN}" ]]; then
    echo "copy binary is missing or not executable: ${COPY_BIN}" >&2
    echo "build it first with: cmake --build ${REPO_ROOT}/build -j" >&2
    exit 1
fi

if [[ "${DRY_RUN}" != "1" ]]; then
    # A non-empty unmatched -t makes copy print every registered case.
    available_cases=$("${COPY_BIN}" -t __case_registration_probe__ 2>&1 || true)
    if ! grep -Fq "${SDMA_CASE}" <<<"${available_cases}"; then
        echo "SDMA case is missing from ${COPY_BIN}: ${SDMA_CASE}" >&2
        echo "reconfigure the existing build with the A3 CANN FFTS headers/runtime, then rebuild" >&2
        exit 1
    fi
fi

mkdir -p "${OUTPUT_DIR}"

SUMMARY_FILE="${OUTPUT_DIR}/summary.tsv"
ALL_LOG="${OUTPUT_DIR}/all.log"

printf 'method\tcase_name\tio_mode\tdevices\tstreams\tio_num\tio_specs_per_device_iteration\tsubmit_calls_per_device_iteration\tbytes_per_device_iteration\titerations\texit_code\tsubmit_avg_us\tsubmit_p90_us\tstart_skew_max_us\tstart_skew_avg_us\tstart_skew_p90_us\tgroup_wall_avg_us\tgroup_wall_p90_us\twall_bw_gib_s\tlog\n' \
    >"${SUMMARY_FILE}"

extract_last()
{
    local pattern=$1
    local file=$2
    local value
    value=$(sed -nE "${pattern}" "${file}" | tail -n 1)
    printf '%s' "${value:-NA}"
}

run_index=0
failed_runs=0

printf 'matrix_start total=%d methods=ce,sdma io_num=%d iterations=%d devices=1,4,8 streams=1,4,16 submit=%s process_sync=%s stream_start_gate=%s stream_sync=%s phase_trace=off dry_run=%s\n' \
    "${total_runs}" "${IO_NUM}" "${ITERATIONS}" "${SUBMIT_MODE}" \
    "${PROCESS_SYNC}" "${STREAM_START_GATE}" "${STREAM_SYNC_MODE}" \
    "${DRY_RUN}" | tee "${ALL_LOG}"

for method in "${methods[@]}"; do
    if [[ "${method}" == "ce" ]]; then
        case_name=${CE_CASE}
    else
        case_name=${SDMA_CASE}
    fi

    for io_mode in "${io_modes[@]}"; do
        if [[ "${io_mode}" == "glm5.1" ]]; then
            io_label=glm51
            io_specs_per_block=3
            bytes_per_block=$((128 * 1024 + 16 * 1024 + 32 * 1024))
            io_args=(--io-mode glm5.1 -f 3)
        else
            io_label=1m
            io_specs_per_block=1
            bytes_per_block=$((1024 * 1024))
            io_args=(--io-mode uniform -s 1M)
            if [[ "${method}" == "sdma" ]]; then
                # Keep 512 independent 1 MiB SDMA tasks, matching CE's 512 IOs.
                io_args+=(-f 1)
            fi
        fi

        io_specs_per_device_iteration=$((IO_NUM * io_specs_per_block))
        bytes_per_device_iteration=$((IO_NUM * bytes_per_block))
        if [[ "${method}" == "ce" ]]; then
            submit_calls_per_device_iteration=${io_specs_per_device_iteration}
        else
            # GLM5.1 puts three SDMA contexts in one FFTS launch per block.
            submit_calls_per_device_iteration=${IO_NUM}
        fi

        for devices in "${devices_list[@]}"; do
            for streams in "${streams_list[@]}"; do
                run_index=$((run_index + 1))
                log_file="${OUTPUT_DIR}/${method}_${io_label}_d${devices}_s${streams}.log"

                command=(
                    "${COPY_BIN}"
                    -t "${case_name}"
                    "${io_args[@]}"
                    -n "${IO_NUM}"
                    -S "${streams}"
                    --submit-mode "${SUBMIT_MODE}"
                    --process-sync "${PROCESS_SYNC}"
                    --stream-start-gate "${STREAM_START_GATE}"
                    --stream-sync "${STREAM_SYNC_MODE}"
                    -i "${ITERATIONS}"
                    -d "${devices}"
                )

                env_args=(-u COPY_ASCEND_PHASE_TRACE)
                if [[ "${method}" == "sdma" ]]; then
                    env_args+=("FFTS_MAX_READY_LANES=${SDMA_READY_LANES}")
                fi
                printf -v command_text '%q ' env "${env_args[@]}" "${command[@]}"

                {
                    printf '\n[%d/%d] method=%s io=%s devices=%s streams=%s io_specs_per_device_iteration=%s submit_calls_per_device_iteration=%s bytes_per_device_iteration=%s\n' \
                        "${run_index}" "${total_runs}" "${method}" "${io_mode}" \
                        "${devices}" "${streams}" \
                        "${io_specs_per_device_iteration}" \
                        "${submit_calls_per_device_iteration}" \
                        "${bytes_per_device_iteration}"
                    printf 'command: %s\n' "${command_text}"
                } | tee "${log_file}" | tee -a "${ALL_LOG}"

                if [[ "${DRY_RUN}" == "1" ]]; then
                    rc=0
                else
                    env "${env_args[@]}" "${command[@]}" 2>&1 \
                        | tee -a "${log_file}" | tee -a "${ALL_LOG}"
                    rc=${PIPESTATUS[0]}
                fi

                submit_avg=$(extract_last \
                    's/.*acl::device::all.*[[:space:]]([0-9]+)[[:space:]]*\/[[:space:]]*([0-9]+)[[:space:]]*\/[[:space:]]*([0-9]+)[[:space:]]*\/[[:space:]]*([0-9]+)[[:space:]]*\/[[:space:]]*([0-9]+)[[:space:]]+N\/A[[:space:]]+N\/A[[:space:]]*$/\3/p' \
                    "${log_file}")
                submit_p90=$(extract_last \
                    's/.*acl::device::all.*[[:space:]]([0-9]+)[[:space:]]*\/[[:space:]]*([0-9]+)[[:space:]]*\/[[:space:]]*([0-9]+)[[:space:]]*\/[[:space:]]*([0-9]+)[[:space:]]*\/[[:space:]]*([0-9]+)[[:space:]]+N\/A[[:space:]]+N\/A[[:space:]]*$/\5/p' \
                    "${log_file}")
                start_skew_max=$(extract_last \
                    's/.*StartSkew\(us\)-\(Min\/Max\/Avg\/P50\/P90\)=[^ ]+ \/ ([^ ]+).*/\1/p' \
                    "${log_file}")
                start_skew_avg=$(extract_last \
                    's/.*StartSkew\(us\)-\(Min\/Max\/Avg\/P50\/P90\)=[^ ]+ \/ [^ ]+ \/ ([^ ]+).*/\1/p' \
                    "${log_file}")
                start_skew_p90=$(extract_last \
                    's/.*StartSkew\(us\)-\(Min\/Max\/Avg\/P50\/P90\)=[^ ]+ \/ [^ ]+ \/ [^ ]+ \/ [^ ]+ \/ ([^ ]+).*/\1/p' \
                    "${log_file}")
                group_wall_avg=$(extract_last \
                    's/.*GroupWall\(us\)-\(Min\/Max\/Avg\/P50\/P90\)=[^ ]+ \/ [^ ]+ \/ ([^ ]+).*/\1/p' \
                    "${log_file}")
                group_wall_p90=$(extract_last \
                    's/.*GroupWall\(us\)-\(Min\/Max\/Avg\/P50\/P90\)=[^ ]+ \/ [^ ]+ \/ [^ ]+ \/ [^ ]+ \/ ([^ ]+).*/\1/p' \
                    "${log_file}")
                wall_bw=$(extract_last \
                    's/.*WallBW\(GB\/s\)=([0-9.]+).*/\1/p' "${log_file}")

                printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                    "${method}" "${case_name}" "${io_mode}" "${devices}" \
                    "${streams}" "${IO_NUM}" \
                    "${io_specs_per_device_iteration}" \
                    "${submit_calls_per_device_iteration}" \
                    "${bytes_per_device_iteration}" "${ITERATIONS}" "${rc}" \
                    "${submit_avg}" "${submit_p90}" "${start_skew_max}" \
                    "${start_skew_avg}" "${start_skew_p90}" \
                    "${group_wall_avg}" "${group_wall_p90}" "${wall_bw}" \
                    "${log_file}" >>"${SUMMARY_FILE}"

                if ((rc != 0)); then
                    failed_runs=$((failed_runs + 1))
                    echo "FAILED: ${command_text}" | tee -a "${ALL_LOG}" >&2
                fi
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
