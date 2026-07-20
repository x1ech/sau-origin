#!/usr/bin/env bash
set -euo pipefail

step8_script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
step8_repo_root=$(cd -- "${step8_script_dir}/../../.." && pwd)
step8_vcs=vcs
step8_outdir=
step8_dry_run=0

usage() {
    printf '%s\n' \
        "Usage: $0 --outdir PATH [--vcs PATH] [--dry-run]" \
        "" \
        "Runs legacy Im2Col, six project-array cases, and seven pipelines."
}

while (($#)); do
    case "$1" in
        --outdir)
            step8_outdir=$2
            shift 2
            ;;
        --vcs)
            step8_vcs=$2
            shift 2
            ;;
        --dry-run)
            step8_dry_run=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            printf 'Unknown argument: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "${step8_outdir}" ]]; then
    printf '%s\n' "--outdir is required" >&2
    exit 2
fi
if [[ "${step8_outdir}" != /* ]]; then
    step8_outdir="${PWD}/${step8_outdir}"
fi

run_command() {
    printf 'COMMAND'
    printf ' %q' "$@"
    printf '\n'
    if ((step8_dry_run == 0)); then
        "$@"
    fi
}

if ((step8_dry_run == 0)); then
    if [[ -e "${step8_outdir}" ]]; then
        printf 'Refusing to overwrite existing output path: %s\n' \
            "${step8_outdir}" >&2
        exit 2
    fi
    mkdir -p "${step8_outdir}/legacy/csrc"
    mkdir -p "${step8_outdir}/standalone/csrc"
    mkdir -p "${step8_outdir}/pipeline/csrc"
    "${step8_vcs}" -ID >"${step8_outdir}/vcs_version.txt" 2>&1
    step8_vcs_version=$(head -n 1 "${step8_outdir}/vcs_version.txt")
else
    step8_vcs_version="VCS dry-run"
fi

cd "${step8_repo_root}"
run_command python3 util/conv_pipeline/step8/verify_step8_sources.py

step8_legacy_simv="${step8_outdir}/legacy/simv"
run_command "${step8_vcs}" \
    -full64 -sverilog -timescale=1ns/1ps -debug_access+all \
    -top tb_gemmini_im2col_chw_gather_readable \
    src/sau_n/rtl/gemmini_im2col_chw_gather_readable.sv \
    src/sau_n/rtl/tb_gemmini_im2col_chw_gather_readable.sv \
    -Mdir="${step8_outdir}/legacy/csrc" \
    -o "${step8_legacy_simv}" \
    -l "${step8_outdir}/legacy/compile.log"
run_command "${step8_legacy_simv}" -l "${step8_outdir}/legacy/run.log"

step8_sa_simv="${step8_outdir}/standalone/simv"
run_command "${step8_vcs}" \
    -full64 -sverilog -timescale=1ns/1ps \
    -top tb_sau_array_16x16 \
    -f src/sau_n/rtl/sau_array_16x16.f \
    -Mdir="${step8_outdir}/standalone/csrc" \
    -o "${step8_sa_simv}" \
    -l "${step8_outdir}/standalone/compile.log"
for step8_case in \
    tail_r1_c1_k9 \
    tail_r15_c15_k9 \
    full_r16_c16_k9 \
    backpressure_r3_c3_k9 \
    sat_pos_r1_c1_k567 \
    sat_neg_r1_c1_k567; do
    run_command "${step8_sa_simv}" \
        "+CASE=${step8_case}" \
        "+TRACE=${step8_outdir}/standalone/${step8_case}.csv" \
        -l "${step8_outdir}/standalone/${step8_case}.log"
    run_command python3 \
        util/conv_pipeline/array/verify_sau_array_trace.py \
        --trace "${step8_outdir}/standalone/${step8_case}.csv" \
        --case "${step8_case}"
done

step8_pipeline_simv="${step8_outdir}/pipeline/simv"
run_command "${step8_vcs}" \
    -full64 -sverilog -timescale=1ns/1ps \
    -top tb_im2col_mikui_sau_pipeline \
    -f src/sau_n/rtl/mikui/filelists/pipeline.f \
    -Mdir="${step8_outdir}/pipeline/csrc" \
    -o "${step8_pipeline_simv}" \
    -l "${step8_outdir}/pipeline/compile.log"
step8_matrix_args=(
    --sim-executable "${step8_pipeline_simv}"
    --outdir "${step8_outdir}/pipeline/golden"
    --simulator-name VCS
    --simulator-version "${step8_vcs_version}"
    --timeout 600
)
if ((step8_dry_run)); then
    step8_matrix_args+=(--dry-run)
fi
run_command python3 util/conv_pipeline/step8/run_pipeline_matrix.py \
    "${step8_matrix_args[@]}"

if ((step8_dry_run)); then
    printf 'DRY RUN COMPLETE Step 8 outdir=%s\n' "${step8_outdir}"
else
    run_command python3 \
        util/conv_pipeline/step8/collect_step8_results.py \
        --outdir "${step8_outdir}"
    printf 'PASS Step 8 workstation matrix outdir=%s\n' "${step8_outdir}"
fi
