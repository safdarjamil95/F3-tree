#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_PATH="${ROOT_DIR}/docs/evaluator_sweep_results.csv"

KEY_COUNTS="2000"
PRODUCER_COUNTS="2,4"
EVALUATOR_COUNTS="1,2"
THRESHOLD_OPS="0,500"
INTERVAL_US="0"
MODES="f3k,f3n"
WORKLOADS="future,mixed"
WRITE_LATENCY_NS="0"
SEED="1"

usage() {
  cat <<'EOF'
Usage:
  ./scripts/run_evaluator_sweep.sh [options]

Options:
  --output PATH            CSV output path
  --key-counts LIST        Comma-separated key counts
  --producers LIST         Comma-separated producer thread counts
  --evaluators LIST        Comma-separated evaluator thread counts
  --thresholds LIST        Comma-separated checkpoint_threshold_ops values
  --intervals LIST         Comma-separated checkpoint_interval_us values
  --modes LIST             Comma-separated evaluator modes: f3k,f3n (canonical) or key,node (aliases)
  --workloads LIST         Comma-separated workloads: future,mixed
  --write-latency NS       Write latency in ns
  --seed N                 Seed for generated workloads
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --output)
      OUTPUT_PATH="$2"
      shift 2
      ;;
    --key-counts)
      KEY_COUNTS="$2"
      shift 2
      ;;
    --producers)
      PRODUCER_COUNTS="$2"
      shift 2
      ;;
    --evaluators)
      EVALUATOR_COUNTS="$2"
      shift 2
      ;;
    --thresholds)
      THRESHOLD_OPS="$2"
      shift 2
      ;;
    --intervals)
      INTERVAL_US="$2"
      shift 2
      ;;
    --modes)
      MODES="$2"
      shift 2
      ;;
    --workloads)
      WORKLOADS="$2"
      shift 2
      ;;
    --write-latency)
      WRITE_LATENCY_NS="$2"
      shift 2
      ;;
    --seed)
      SEED="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

mkdir -p "$(dirname "${OUTPUT_PATH}")"

make -C "${ROOT_DIR}" bin/future_benchmark bin/mixed_benchmark >/dev/null

echo "workload,mode,key_count,producer_threads,evaluator_threads,threshold_ops,interval_us,write_latency_ns,seed,total_us,op_phase_us,checkpoint_count,async_checkpoint_count,checkpoint_time_us,replayed_ops,replayed_nodes,ops_per_sec,found" > "${OUTPUT_PATH}"

list_to_array() {
  local csv="$1"
  IFS=',' read -r -a values <<< "${csv}"
  printf '%s\n' "${values[@]}"
}

extract_metric() {
  local key="$1"
  local file="$2"
  local value
  value="$(awk -F'=' -v metric="${key}" '$1 == metric { print $2 }' "${file}" | tail -n 1)"
  if [[ -z "${value}" ]]; then
    echo ""
  else
    echo "${value}"
  fi
}

for workload in $(list_to_array "${WORKLOADS}"); do
  case "${workload}" in
    future)
      bench_path="${ROOT_DIR}/bin/future_benchmark"
      total_key="future_total_us"
      phase_key="future_buffer_insert_us"
      checkpoint_count_key="future_checkpoint_count"
      async_checkpoint_count_key="future_async_checkpoint_count"
      checkpoint_time_key="future_checkpoint_time_us"
      replayed_ops_key="future_replayed_ops"
      replayed_nodes_key="future_replayed_nodes"
      throughput_key="future_ops_per_sec"
      found_key="future_found"
      ;;
    mixed)
      bench_path="${ROOT_DIR}/bin/mixed_benchmark"
      total_key="mixed_total_us"
      phase_key="mixed_ops_us"
      checkpoint_count_key="mixed_checkpoint_count"
      async_checkpoint_count_key="mixed_async_checkpoint_count"
      checkpoint_time_key="mixed_checkpoint_time_us"
      replayed_ops_key="mixed_replayed_ops"
      replayed_nodes_key="mixed_replayed_nodes"
      throughput_key="mixed_ops_per_sec"
      found_key="mixed_found"
      ;;
    *)
      echo "unknown workload: ${workload}" >&2
      exit 1
      ;;
  esac

  for mode in $(list_to_array "${MODES}"); do
    for key_count in $(list_to_array "${KEY_COUNTS}"); do
      for producers in $(list_to_array "${PRODUCER_COUNTS}"); do
        for evaluators in $(list_to_array "${EVALUATOR_COUNTS}"); do
          for threshold in $(list_to_array "${THRESHOLD_OPS}"); do
            for interval in $(list_to_array "${INTERVAL_US}"); do
              run_output="$(mktemp)"
              "${bench_path}" \
                -n "${key_count}" \
                -t "${producers}" \
                -e "${evaluators}" \
                -m "${mode}" \
                -c "${threshold}" \
                -u "${interval}" \
                -w "${WRITE_LATENCY_NS}" \
                -s "${SEED}" > "${run_output}"

              total_us="$(extract_metric "${total_key}" "${run_output}")"
              op_phase_us="$(extract_metric "${phase_key}" "${run_output}")"
              checkpoint_count="$(extract_metric "${checkpoint_count_key}" "${run_output}")"
              async_checkpoint_count="$(extract_metric "${async_checkpoint_count_key}" "${run_output}")"
              checkpoint_time_us="$(extract_metric "${checkpoint_time_key}" "${run_output}")"
              replayed_ops="$(extract_metric "${replayed_ops_key}" "${run_output}")"
              replayed_nodes="$(extract_metric "${replayed_nodes_key}" "${run_output}")"
              ops_per_sec="$(extract_metric "${throughput_key}" "${run_output}")"
              found="$(extract_metric "${found_key}" "${run_output}")"

              echo "${workload},${mode},${key_count},${producers},${evaluators},${threshold},${interval},${WRITE_LATENCY_NS},${SEED},${total_us},${op_phase_us},${checkpoint_count},${async_checkpoint_count},${checkpoint_time_us},${replayed_ops},${replayed_nodes},${ops_per_sec},${found}" >> "${OUTPUT_PATH}"
              rm -f "${run_output}"
            done
          done
        done
      done
    done
  done
done

echo "wrote ${OUTPUT_PATH}"
