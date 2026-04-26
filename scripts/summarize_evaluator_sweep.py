#!/usr/bin/env python3

import argparse
import csv
import math
import os
from collections import defaultdict
from statistics import mean
from typing import Dict, Iterable, List, Tuple


NUMERIC_FIELDS = {
    "key_count",
    "producer_threads",
    "evaluator_threads",
    "threshold_ops",
    "interval_us",
    "write_latency_ns",
    "seed",
    "total_us",
    "op_phase_us",
    "checkpoint_count",
    "async_checkpoint_count",
    "checkpoint_time_us",
    "replayed_ops",
    "replayed_nodes",
    "ops_per_sec",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarize evaluator sweep CSV into grouped tables and optional plots."
    )
    parser.add_argument("input_csv", help="path to evaluator_sweep_results.csv")
    parser.add_argument(
        "--output-dir",
        default="docs/evaluator_report",
        help="directory for summary outputs",
    )
    parser.add_argument(
        "--group-by",
        default="workload,mode,key_count,producer_threads,evaluator_threads,threshold_ops,interval_us,write_latency_ns",
        help="comma-separated grouping fields for summary rows",
    )
    parser.add_argument(
        "--plot",
        action="store_true",
        help="attempt to generate PNG plots with matplotlib if available",
    )
    return parser.parse_args()


def load_rows(path: str) -> List[Dict[str, object]]:
    rows: List[Dict[str, object]] = []
    with open(path, newline="", encoding="utf-8") as handle:
      reader = csv.DictReader(handle)
      for row in reader:
          parsed: Dict[str, object] = {}
          for key, value in row.items():
              if key in NUMERIC_FIELDS and value != "":
                  parsed[key] = float(value) if "." in value else int(value)
              else:
                  parsed[key] = value
          rows.append(parsed)
    return rows


def group_rows(
    rows: Iterable[Dict[str, object]], group_fields: List[str]
) -> Dict[Tuple[object, ...], List[Dict[str, object]]]:
    grouped: Dict[Tuple[object, ...], List[Dict[str, object]]] = defaultdict(list)
    for row in rows:
        key = tuple(row[field] for field in group_fields)
        grouped[key].append(row)
    return grouped


def summarize_group(rows: List[Dict[str, object]]) -> Dict[str, object]:
    summary: Dict[str, object] = {}
    summary["runs"] = len(rows)
    for field in [
        "total_us",
        "op_phase_us",
        "checkpoint_count",
        "async_checkpoint_count",
        "checkpoint_time_us",
        "replayed_ops",
        "replayed_nodes",
        "ops_per_sec",
    ]:
        values = [float(row[field]) for row in rows]
        summary[f"{field}_mean"] = mean(values)
        summary[f"{field}_min"] = min(values)
        summary[f"{field}_max"] = max(values)
    found_values = [str(row["found"]) for row in rows]
    summary["found_all"] = ";".join(found_values)
    return summary


def write_summary_csv(
    output_path: str,
    grouped: Dict[Tuple[object, ...], List[Dict[str, object]]],
    group_fields: List[str],
) -> None:
    metric_fields = [
        "runs",
        "total_us_mean",
        "total_us_min",
        "total_us_max",
        "op_phase_us_mean",
        "op_phase_us_min",
        "op_phase_us_max",
        "checkpoint_count_mean",
        "checkpoint_count_min",
        "checkpoint_count_max",
        "async_checkpoint_count_mean",
        "async_checkpoint_count_min",
        "async_checkpoint_count_max",
        "checkpoint_time_us_mean",
        "checkpoint_time_us_min",
        "checkpoint_time_us_max",
        "replayed_ops_mean",
        "replayed_ops_min",
        "replayed_ops_max",
        "replayed_nodes_mean",
        "replayed_nodes_min",
        "replayed_nodes_max",
        "ops_per_sec_mean",
        "ops_per_sec_min",
        "ops_per_sec_max",
        "found_all",
    ]
    with open(output_path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=group_fields + metric_fields)
        writer.writeheader()
        for key in sorted(grouped.keys()):
            row = {field: value for field, value in zip(group_fields, key)}
            row.update(summarize_group(grouped[key]))
            writer.writerow(row)


def markdown_table(
    grouped: Dict[Tuple[object, ...], List[Dict[str, object]]], group_fields: List[str]
) -> str:
    header = [
        "Workload",
        "Mode",
        "Keys",
        "Prod",
        "Eval",
        "Threshold",
        "Interval",
        "Runs",
        "Ops/sec mean",
        "Checkpoint us mean",
        "Replayed ops",
        "Replayed nodes",
    ]
    lines = [
        "# Evaluator Sweep Summary",
        "",
        "| " + " | ".join(header) + " |",
        "| " + " | ".join(["---"] * len(header)) + " |",
    ]
    for key in sorted(grouped.keys()):
        row = {field: value for field, value in zip(group_fields, key)}
        summary = summarize_group(grouped[key])
        lines.append(
            "| {workload} | {mode} | {keys} | {prod} | {evals} | {threshold} | {interval} | {runs} | {ops:.2f} | {checkpoint:.2f} | {replayed_ops:.2f} | {replayed_nodes:.2f} |".format(
                workload=row.get("workload", ""),
                mode=row.get("mode", ""),
                keys=row.get("key_count", ""),
                prod=row.get("producer_threads", ""),
                evals=row.get("evaluator_threads", ""),
                threshold=row.get("threshold_ops", ""),
                interval=row.get("interval_us", ""),
                runs=summary["runs"],
                ops=summary["ops_per_sec_mean"],
                checkpoint=summary["checkpoint_time_us_mean"],
                replayed_ops=summary["replayed_ops_mean"],
                replayed_nodes=summary["replayed_nodes_mean"],
            )
        )
    lines.append("")
    return "\n".join(lines)


def write_markdown(
    output_path: str,
    grouped: Dict[Tuple[object, ...], List[Dict[str, object]]],
    group_fields: List[str],
) -> None:
    with open(output_path, "w", encoding="utf-8") as handle:
        handle.write(markdown_table(grouped, group_fields))


def maybe_make_plots(rows: List[Dict[str, object]], output_dir: str) -> str:
    try:
        import matplotlib.pyplot as plt  # type: ignore
    except Exception:
        return "matplotlib not available; skipped plots"

    grouped: Dict[Tuple[str, str], List[Dict[str, object]]] = defaultdict(list)
    for row in rows:
        grouped[(str(row["workload"]), str(row["mode"]))].append(row)

    for workload in sorted({str(row["workload"]) for row in rows}):
        fig, axes = plt.subplots(1, 2, figsize=(12, 4))
        for mode in ["key", "node"]:
            series = sorted(
                [
                    row
                    for row in rows
                    if str(row["workload"]) == workload and str(row["mode"]) == mode
                ],
                key=lambda item: (int(item["key_count"]), int(item["threshold_ops"])),
            )
            if not series:
                continue
            x = list(range(len(series)))
            labels = [
                f"k={row['key_count']},th={row['threshold_ops']}"
                for row in series
            ]
            axes[0].plot(x, [float(row["ops_per_sec"]) for row in series], marker="o", label=mode)
            axes[1].plot(
                x,
                [float(row["checkpoint_time_us"]) for row in series],
                marker="o",
                label=mode,
            )
            axes[0].set_xticks(x)
            axes[0].set_xticklabels(labels, rotation=45, ha="right")
            axes[1].set_xticks(x)
            axes[1].set_xticklabels(labels, rotation=45, ha="right")

        axes[0].set_title(f"{workload} throughput")
        axes[0].set_ylabel("ops/sec")
        axes[1].set_title(f"{workload} checkpoint time")
        axes[1].set_ylabel("checkpoint_time_us")
        axes[0].legend()
        axes[1].legend()
        fig.tight_layout()
        fig.savefig(os.path.join(output_dir, f"{workload}_summary.png"))
        plt.close(fig)

    return "generated plots"


def main() -> None:
    args = parse_args()
    rows = load_rows(args.input_csv)
    group_fields = [field.strip() for field in args.group_by.split(",") if field.strip()]

    os.makedirs(args.output_dir, exist_ok=True)

    grouped = group_rows(rows, group_fields)
    summary_csv = os.path.join(args.output_dir, "evaluator_summary.csv")
    summary_md = os.path.join(args.output_dir, "evaluator_summary.md")

    write_summary_csv(summary_csv, grouped, group_fields)
    write_markdown(summary_md, grouped, group_fields)

    plot_status = "plots not requested"
    if args.plot:
        plot_status = maybe_make_plots(rows, args.output_dir)

    print(f"wrote {summary_csv}")
    print(f"wrote {summary_md}")
    print(plot_status)


if __name__ == "__main__":
    main()
