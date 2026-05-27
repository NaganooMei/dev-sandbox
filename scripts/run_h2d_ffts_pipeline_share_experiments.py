#!/usr/bin/env python3
"""Run H2D FFTS pipeline share experiments and collect report tables."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Tuple


DEFAULT_EXP1_SIZES = ["2K", "8K", "32K", "64K", "128K", "256K", "512K"]
DEFAULT_EXP1_COUNTS = [1024, 4096]
DEFAULT_EXP3_SIZES = ["2K", "8K", "32K", "64K"]
DEFAULT_PIPELINE_TARGETS = ["1M", "2M"]
DEFAULT_ONE_SHARE_HOST_PIPELINE_CASE = "one_share_host_to_all_device_ffts_pipeline"
DEFAULT_ONE_SHARE_HOST_MULTISTREAM_CASE = "one_share_host_to_all_device_ce_multi_stream"
DEFAULT_ALL_HOST_PIPELINE_CASE = "all_host_to_all_device_ffts_pipeline"
DEFAULT_ALL_HOST_MULTISTREAM_CASE = "all_host_to_all_device_ce_multi_stream"

STAT_RE = re.compile(
    r"(?P<submit_min>\d+)\s*/\s*"
    r"(?P<submit_max>\d+)\s*/\s*"
    r"(?P<submit_avg>\d+)\s*/\s*"
    r"(?P<submit_p50>\d+)\s*/\s*"
    r"(?P<submit_p90>\d+)\s+"
    r"(?P<copy_min>\d+)\s*/\s*"
    r"(?P<copy_max>\d+)\s*/\s*"
    r"(?P<copy_avg>\d+)\s*/\s*"
    r"(?P<copy_p50>\d+)\s*/\s*"
    r"(?P<copy_p90>\d+)\s+"
    r"(?P<bw_gbs>\d+(?:\.\d+)?)\s*$"
)


@dataclass(frozen=True)
class Variant:
    name: str
    case_name: str
    object_frags_mode: str
    target_object_bytes: Optional[int] = None


@dataclass(frozen=True)
class RunSpec:
    experiment: str
    axis: str
    topology: str
    io_size: str
    io_count: int
    devices: int
    variant: Variant
    object_frags: Optional[int]


SUMMARY_FIELDS = [
    "experiment",
    "axis",
    "topology",
    "io_size",
    "io_count",
    "iterations",
    "devices",
    "variant",
    "copy_case",
    "target_object_bytes",
    "object_frags",
    "row_index",
    "submit_min_us",
    "submit_max_us",
    "submit_avg_us",
    "submit_p50_us",
    "submit_p90_us",
    "copy_min_us",
    "copy_max_us",
    "copy_avg_us",
    "copy_p50_us",
    "copy_p90_us",
    "bw_gbs",
    "log_file",
]


def parse_size_bytes(text: str) -> int:
    normalized = text.strip().upper()
    match = re.fullmatch(r"(\d+)([KMG]?B?|)", normalized)
    if match is None:
        raise ValueError(f"invalid IO size: {text}")

    value = int(match.group(1))
    unit = match.group(2)
    if unit in ("", "B"):
        return value
    if unit in ("K", "KB"):
        return value * 1024
    if unit in ("M", "MB"):
        return value * 1024 * 1024
    if unit in ("G", "GB"):
        return value * 1024 * 1024 * 1024
    raise ValueError(f"invalid IO size unit: {text}")


def target_object_frags(io_size: str, io_count: int, target_bytes: int) -> int:
    size_bytes = parse_size_bytes(io_size)
    rounded = (target_bytes + size_bytes // 2) // size_bytes
    return max(1, min(io_count, rounded))


def object_frags_for(spec: RunSpec) -> Optional[int]:
    if spec.variant.object_frags_mode == "target_bytes":
        assert spec.variant.target_object_bytes is not None
        return target_object_frags(spec.io_size, spec.io_count, spec.variant.target_object_bytes)
    if spec.variant.object_frags_mode == "full_count":
        return spec.io_count
    return None


def format_size_label(size_bytes: int) -> str:
    mib = 1024 * 1024
    kib = 1024
    if size_bytes % mib == 0:
        return f"{size_bytes // mib}m"
    if size_bytes % kib == 0:
        return f"{size_bytes // kib}k"
    return f"{size_bytes}b"


def parse_pipeline_targets(args: argparse.Namespace) -> List[int]:
    if args.target_object_bytes is not None:
        return [args.target_object_bytes]
    targets = []
    for target in args.pipeline_targets:
        targets.append(parse_size_bytes(target))
    return targets


def make_pipeline_variants(targets: Iterable[int], case_name: str) -> List[Variant]:
    return [
        Variant(f"ffts_{format_size_label(target)}_pipeline", case_name, "target_bytes", target)
        for target in targets
    ]


def make_exp1_specs(args: argparse.Namespace) -> List[RunSpec]:
    pipeline_targets = parse_pipeline_targets(args)
    variants = [
        *make_pipeline_variants(pipeline_targets, args.pipeline_case),
        Variant("ffts_full_no_pipeline", args.pipeline_case, "full_count"),
        Variant("ascend_ce_copy", args.ce_case, "none"),
        Variant("ascend_multistream_ce_copy", args.multistream_case, "none"),
    ]

    specs: List[RunSpec] = []
    for count in args.exp1_counts:
        for size in args.exp1_sizes:
            for variant in variants:
                spec = RunSpec(
                    experiment="exp1_io_size_sweep",
                    axis=f"{size}_x{count}",
                    topology="single_device",
                    io_size=size,
                    io_count=count,
                    devices=args.devices,
                    variant=variant,
                    object_frags=None,
                )
                specs.append(
                    RunSpec(
                        spec.experiment,
                        spec.axis,
                        spec.topology,
                        spec.io_size,
                        spec.io_count,
                        spec.devices,
                        spec.variant,
                        object_frags_for(spec),
                    )
                )
    return specs


def make_exp3_specs(args: argparse.Namespace) -> List[RunSpec]:
    pipeline_targets = parse_pipeline_targets(args)
    specs: List[RunSpec] = []
    exp3_topologies = [
        (
            "one_share_host_to_all_devices",
            args.one_share_host_pipeline_case,
            [args.one_share_host_multistream_case],
        ),
        (
            "all_hosts_to_all_devices",
            args.all_host_pipeline_case,
            [args.all_host_multistream_case],
        ),
    ]
    for topology, pipeline_case, multistream_cases in exp3_topologies:
        exp3_variants = [
            *make_pipeline_variants(pipeline_targets, pipeline_case),
            Variant("ffts_full_no_pipeline", pipeline_case, "full_count"),
            *[
                Variant(multistream_case, multistream_case, "none")
                for multistream_case in multistream_cases
            ],
        ]
        for size in args.exp3_sizes:
            for variant in exp3_variants:
                spec = RunSpec(
                    experiment="exp3_eight_device_topology",
                    axis=f"{topology}_x{size}",
                    topology=topology,
                    io_size=size,
                    io_count=args.exp3_count,
                    devices=args.exp3_devices,
                    variant=variant,
                    object_frags=None,
                )
                specs.append(
                    RunSpec(
                        spec.experiment,
                        spec.axis,
                        spec.topology,
                        spec.io_size,
                        spec.io_count,
                        spec.devices,
                        spec.variant,
                        object_frags_for(spec),
                    )
                )
    return specs


def make_specs(args: argparse.Namespace) -> List[RunSpec]:
    specs: List[RunSpec] = []
    if args.suite in ("all", "exp1"):
        specs.extend(make_exp1_specs(args))
    if args.suite in ("all", "exp3"):
        specs.extend(make_exp3_specs(args))
    return specs


def shell_command(args: argparse.Namespace, spec: RunSpec) -> str:
    env_parts = [
        f"COPY_FFTS_VALIDATE={'1' if args.validate else '0'}",
        f"FFTS_MAX_READY_LANES={args.ffts_max_ready_lanes}",
    ]
    if spec.object_frags is not None:
        env_parts.append(f"COPY_FFTS_PIPELINE_OBJECT_FRAGS={spec.object_frags}")
    command = [
        str(args.copy_bin),
        "-t",
        spec.variant.case_name,
        "-s",
        spec.io_size,
        "-n",
        str(spec.io_count),
        "-i",
        str(args.iterations),
        "-d",
        str(spec.devices),
    ]
    return " ".join(env_parts + [shlex.quote(part) for part in command])


def run_command(args: argparse.Namespace, spec: RunSpec, log_file: Path) -> int:
    env = os.environ.copy()
    env["COPY_FFTS_VALIDATE"] = "1" if args.validate else "0"
    env["FFTS_MAX_READY_LANES"] = str(args.ffts_max_ready_lanes)
    if spec.object_frags is None:
        env.pop("COPY_FFTS_PIPELINE_OBJECT_FRAGS", None)
    else:
        env["COPY_FFTS_PIPELINE_OBJECT_FRAGS"] = str(spec.object_frags)

    command = [
        str(args.copy_bin),
        "-t",
        spec.variant.case_name,
        "-s",
        spec.io_size,
        "-n",
        str(spec.io_count),
        "-i",
        str(args.iterations),
        "-d",
        str(spec.devices),
    ]

    print(f"\n[run] {spec.experiment} {spec.axis} {spec.variant.name}")
    print(f"[cmd] {shell_command(args, spec)}")
    with log_file.open("w", encoding="utf-8") as out:
        out.write(f"$ {shell_command(args, spec)}\n\n")
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=env,
        )
        assert process.stdout is not None
        for line in process.stdout:
            print(line, end="")
            out.write(line)
        return process.wait()


def parse_log(log_file: Path) -> List[Dict[str, str]]:
    rows = []
    with log_file.open("r", encoding="utf-8", errors="replace") as source:
        for line in source:
            match = STAT_RE.search(line)
            if match is not None:
                rows.append(match.groupdict())
    return rows


def write_plan(args: argparse.Namespace, specs: Iterable[RunSpec], out_dir: Path) -> None:
    plan_file = out_dir / "planned_commands.sh"
    with plan_file.open("w", encoding="utf-8") as out:
        out.write("#!/usr/bin/env bash\n")
        out.write("set -euo pipefail\n\n")
        specs = list(specs)
        for index, spec in enumerate(specs):
            out.write(
                f"# {spec.experiment} axis={spec.axis} topology={spec.topology} "
                f"variant={spec.variant.name}\n"
            )
            out.write(shell_command(args, spec))
            out.write("\n")
            if index + 1 < len(specs) and args.case_wait_sec > 0:
                out.write(f"sleep {args.case_wait_sec:g}\n")
            out.write("\n")
    print(f"[plan] wrote {plan_file}")


def append_summary_rows(
    writer: csv.DictWriter,
    args: argparse.Namespace,
    spec: RunSpec,
    rows: List[Dict[str, str]],
    log_file: Path,
) -> None:
    for index, parsed in enumerate(rows):
        row = {
            "experiment": spec.experiment,
            "axis": spec.axis,
            "topology": spec.topology,
            "io_size": spec.io_size,
            "io_count": spec.io_count,
            "iterations": args.iterations,
            "devices": spec.devices,
            "variant": spec.variant.name,
            "copy_case": spec.variant.case_name,
            "target_object_bytes": (
                "" if spec.variant.target_object_bytes is None else spec.variant.target_object_bytes
            ),
            "object_frags": "" if spec.object_frags is None else spec.object_frags,
            "row_index": index,
            "log_file": str(log_file),
        }
        row.update({f"{key}_us" if key != "bw_gbs" else key: value for key, value in parsed.items()})
        writer.writerow(row)


def rows_from_tsv(summary_file: Path) -> List[Dict[str, str]]:
    with summary_file.open("r", encoding="utf-8", newline="") as source:
        return list(csv.DictReader(source, delimiter="\t"))


def markdown_table(rows: List[Dict[str, str]], experiment: str, columns: List[Tuple[str, str]]) -> str:
    selected = [row for row in rows if row["experiment"] == experiment]
    lines = []
    lines.append("| " + " | ".join(title for title, _ in columns) + " |")
    lines.append("| " + " | ".join("---" for _ in columns) + " |")
    for row in selected:
        lines.append("| " + " | ".join(str(row[key]) for _, key in columns) + " |")
    return "\n".join(lines)


def write_report(args: argparse.Namespace, out_dir: Path, summary_file: Path) -> None:
    rows = rows_from_tsv(summary_file) if summary_file.exists() else []
    report_file = out_dir / "report.md"
    generated_at = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    pipeline_targets = ", ".join(args.pipeline_targets)
    with report_file.open("w", encoding="utf-8") as out:
        out.write("# H2D FFTS Pipeline 实验数据报告\n\n")
        out.write(f"- 生成时间: {generated_at}\n")
        out.write(f"- copy 可执行文件: `{args.copy_bin}`\n")
        out.write(f"- 单卡实验设备数: {args.devices}\n")
        out.write(f"- 多卡实验设备数: {args.exp3_devices}\n")
        out.write(f"- 迭代次数: {args.iterations}\n")
        out.write(f"- FFTS pipeline 聚合目标: {pipeline_targets}\n")
        out.write(f"- FFTS ready lanes: {args.ffts_max_ready_lanes}\n")
        out.write(f"- 原始日志目录: `{out_dir}`\n\n")
        out.write("## 实验一: 扫 IO 大小\n\n")
        out.write(
            markdown_table(
                rows,
                "exp1_io_size_sweep",
                [
                    ("IO Size", "io_size"),
                    ("IO Count", "io_count"),
                    ("Devices", "devices"),
                    ("Variant", "variant"),
                    ("Target Bytes", "target_object_bytes"),
                    ("Object Frags", "object_frags"),
                    ("Submit Avg(us)", "submit_avg_us"),
                    ("Copy Avg(us)", "copy_avg_us"),
                    ("Copy P50(us)", "copy_p50_us"),
                    ("Copy P90(us)", "copy_p90_us"),
                    ("BW(GB/s)", "bw_gbs"),
                ],
            )
        )
        out.write("\n\n## 实验三: 8 卡同时读拓扑\n\n")
        out.write(
            markdown_table(
                rows,
                "exp3_eight_device_topology",
                [
                    ("Topology", "topology"),
                    ("IO Size", "io_size"),
                    ("IO Count", "io_count"),
                    ("Devices", "devices"),
                    ("Variant", "variant"),
                    ("Copy Case", "copy_case"),
                    ("Target Bytes", "target_object_bytes"),
                    ("Object Frags", "object_frags"),
                    ("Submit Avg(us)", "submit_avg_us"),
                    ("Copy Avg(us)", "copy_avg_us"),
                    ("Copy P50(us)", "copy_p50_us"),
                    ("Copy P90(us)", "copy_p90_us"),
                    ("BW(GB/s)", "bw_gbs"),
                ],
            )
        )
        out.write("\n\n## 字段说明\n\n")
        out.write("- Submit Avg(us): host 侧提交本轮操作的平均耗时。\n")
        out.write("- Copy Avg/P50/P90(us): ACL event 覆盖的设备侧 H2D 完成时间统计。\n")
        out.write("- BW(GB/s): `copy` 程序按 `size * count / Copy Avg` 计算的带宽。\n")
        out.write("- Object Frags: FFTS pipeline 每个 logical object 聚合的 IO fragment 数。\n")
        out.write("- `ffts_full_no_pipeline` 的 Object Frags 等于 IO Count，表示全量聚合为一个 object。\n")
        out.write("- Topology: `one_share_host_to_all_devices` 表示 8 卡同时读一块 shared host buffer；")
        out.write("`all_hosts_to_all_devices` 表示 8 卡同时读 8 块独立 host buffer。\n")
    print(f"[report] wrote {report_file}")


def parse_args(default_suite: str = "all") -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run H2D FFTS pipeline share experiments and collect tables."
    )
    parser.add_argument("--suite", choices=["all", "exp1", "exp3"], default=default_suite)
    parser.add_argument("--copy-bin", default="./build/module/copy/copy")
    parser.add_argument("--output-root", default="logs/h2d_ffts_pipeline_share")
    parser.add_argument("--run-id", default=dt.datetime.now().strftime("%Y%m%d-%H%M%S"))
    parser.add_argument("--devices", type=int, default=1)
    parser.add_argument("--iterations", type=int, default=128)
    parser.add_argument("--exp1-sizes", nargs="+", default=DEFAULT_EXP1_SIZES)
    parser.add_argument("--exp1-counts", nargs="+", type=int, default=DEFAULT_EXP1_COUNTS)
    parser.add_argument("--exp3-sizes", nargs="+", default=DEFAULT_EXP3_SIZES)
    parser.add_argument("--exp3-size", default=None, help=argparse.SUPPRESS)
    parser.add_argument("--exp3-count", type=int, default=1024)
    parser.add_argument("--exp3-devices", type=int, default=8)
    parser.add_argument("--case-wait-sec", type=float, default=0.3)
    parser.add_argument("--pipeline-targets", nargs="+", default=DEFAULT_PIPELINE_TARGETS)
    parser.add_argument("--target-object-bytes", type=int, default=None, help=argparse.SUPPRESS)
    parser.add_argument("--ffts-max-ready-lanes", type=int, default=8)
    parser.add_argument("--pipeline-case", default="host_to_device_ffts_pipeline")
    parser.add_argument("--ce-case", default="host_to_device_ce")
    parser.add_argument("--multistream-case", default="host_to_device_ce_multi_stream")
    parser.add_argument(
        "--one-share-host-pipeline-case",
        "--one-host-pipeline-case",
        dest="one_share_host_pipeline_case",
        default=DEFAULT_ONE_SHARE_HOST_PIPELINE_CASE,
        help=(
            "Pipeline case for the shared-host topology. "
            "--one-host-pipeline-case is kept as a compatibility alias."
        ),
    )
    parser.add_argument(
        "--all-host-pipeline-case", default=DEFAULT_ALL_HOST_PIPELINE_CASE
    )
    parser.add_argument(
        "--one-share-host-multistream-case",
        "--one-malloc-host-multistream-case",
        dest="one_share_host_multistream_case",
        default=DEFAULT_ONE_SHARE_HOST_MULTISTREAM_CASE,
        help=(
            "Multi-stream CE case for the shared-host topology. "
            "--one-malloc-host-multistream-case is kept as a compatibility alias."
        ),
    )
    parser.add_argument(
        "--all-host-multistream-case", default=DEFAULT_ALL_HOST_MULTISTREAM_CASE
    )
    parser.add_argument("--validate", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.target_object_bytes is not None:
        args.pipeline_targets = [str(args.target_object_bytes)]
    if args.exp3_size is not None:
        args.exp3_sizes = [args.exp3_size]
    return args


def main(default_suite: str = "all") -> int:
    args = parse_args(default_suite)
    out_dir = Path(args.output_root) / args.run_id
    out_dir.mkdir(parents=True, exist_ok=True)

    specs = make_specs(args)
    write_plan(args, specs, out_dir)
    if args.dry_run:
        print(f"[dry-run] planned {len(specs)} commands")
        return 0

    if not Path(args.copy_bin).exists():
        print(f"copy binary not found: {args.copy_bin}", file=sys.stderr)
        print("Build first, for example: cmake -B build && cmake --build build -j", file=sys.stderr)
        return 2

    summary_file = out_dir / "summary.tsv"
    with summary_file.open("w", encoding="utf-8", newline="") as target:
        writer = csv.DictWriter(target, SUMMARY_FIELDS, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        for index, spec in enumerate(specs):
            object_suffix = "none" if spec.object_frags is None else str(spec.object_frags)
            log_name = (
                f"{spec.experiment}__{spec.axis}__{spec.variant.name}"
                f"__obj{object_suffix}.log"
            )
            log_file = out_dir / log_name
            rc = run_command(args, spec, log_file)
            if rc != 0:
                print(f"command failed with exit code {rc}; see {log_file}", file=sys.stderr)
                return rc
            rows = parse_log(log_file)
            if not rows:
                print(f"could not parse result row from {log_file}", file=sys.stderr)
                return 3
            append_summary_rows(writer, args, spec, rows, log_file)
            if index + 1 < len(specs) and args.case_wait_sec > 0:
                time.sleep(args.case_wait_sec)

    write_report(args, out_dir, summary_file)
    print(f"[summary] wrote {summary_file}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
