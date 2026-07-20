#!/usr/bin/env bash
set -euo pipefail

step0_script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
step0_repo_root=$(cd -- "${step0_script_dir}/../../.." && pwd)
step0_vcs=vcs
step0_outdir=
step0_dry_run=0

usage() {
    printf '%s\n' \
        "Usage: $0 --outdir PATH [--vcs PATH] [--dry-run]" \
        "" \
        "Compiles and runs the original and patched Mikui SA_ENGINE Step 0 matrix."
}

while (($#)); do
    case "$1" in
        --outdir)
            step0_outdir=$2
            shift 2
            ;;
        --vcs)
            step0_vcs=$2
            shift 2
            ;;
        --dry-run)
            step0_dry_run=1
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

if [[ -z "${step0_outdir}" ]]; then
    printf '%s\n' "--outdir is required" >&2
    exit 2
fi

if [[ "${step0_outdir}" != /* ]]; then
    step0_outdir="${PWD}/${step0_outdir}"
fi

step0_cases=(
    tail_r1_c1
    tail_r15_c15
    tail_r16_c16
    mapping_k9
    control_k9_bp
    sat_pos_k567
    sat_neg_k567
)

run_command() {
    printf 'COMMAND'
    printf ' %q' "$@"
    printf '\n'
    if ((step0_dry_run == 0)); then
        "$@"
    fi
}

if ((step0_dry_run == 0)); then
    if [[ -e "${step0_outdir}" ]]; then
        printf 'Refusing to overwrite existing output path: %s\n' \
            "${step0_outdir}" >&2
        exit 2
    fi
    mkdir -p "${step0_outdir}"
fi

cd "${step0_repo_root}"
run_command python3 util/conv_pipeline/step0/verify_step0_sources.py

for step0_variant in original integration; do
    step0_variant_dir="${step0_outdir}/${step0_variant}"
    step0_simv="${step0_variant_dir}/simv"
    step0_expect_patched=0
    if [[ "${step0_variant}" == integration ]]; then
        step0_expect_patched=1
    fi

    if ((step0_dry_run == 0)); then
        mkdir -p "${step0_variant_dir}/csrc"
    fi

    run_command "${step0_vcs}" \
        -full64 \
        -sverilog \
        -timescale=1ns/1ps \
        -top tb_mikui_sau_engine_step0 \
        -f "src/sau_n/rtl/mikui/filelists/${step0_variant}.f" \
        -Mdir="${step0_variant_dir}/csrc" \
        -o "${step0_simv}" \
        -l "${step0_variant_dir}/compile.log"

    for step0_case in "${step0_cases[@]}"; do
        step0_trace="${step0_variant_dir}/${step0_case}.csv"
        step0_log="${step0_variant_dir}/${step0_case}.log"
        run_command "${step0_simv}" \
            "+CASE=${step0_case}" \
            "+EXPECT_PATCHED=${step0_expect_patched}" \
            "+TRACE=${step0_trace}" \
            -l "${step0_log}"
        run_command python3 \
            util/conv_pipeline/step0/verify_step0_trace.py \
            --trace "${step0_trace}" \
            --case "${step0_case}" \
            --variant "${step0_variant}"
    done
done

if ((step0_dry_run)); then
    printf 'DRY RUN COMPLETE Step 0 VCS matrix outdir=%s\n' "${step0_outdir}"
else
    run_command python3 \
        util/conv_pipeline/step0/collect_step0_results.py \
        --outdir "${step0_outdir}" \
        --vcs "${step0_vcs}"
    printf 'PASS Step 0 VCS matrix outdir=%s\n' "${step0_outdir}"
fi
