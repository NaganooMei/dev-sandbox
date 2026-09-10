#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import os
import tempfile
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "ucm-plot-cache"))

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.lines import Line2D


IO_MODES = ("glm5.1", "1m")
DEVICES = (1, 4, 8)
STREAMS = (1, 4, 16)
COPY_COLOR = "#0072B2"
SUBMIT_COLOR = "#D55E00"
GLM_COLOR = "#D55E00"
LARGE_IO_COLOR = "#0072B2"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Plot one all-host Ascend H2D benchmark group")
    parser.add_argument("--data", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--title", required=True)
    parser.add_argument("--subtitle", required=True)
    parser.add_argument("--kind", choices=("time", "bandwidth"), default="time")
    return parser.parse_args()


def load_rows(path: Path) -> list[dict[str, object]]:
    with path.open("r", encoding="utf-8-sig", newline="") as file:
        rows = list(csv.DictReader(file, delimiter="\t"))

    required = {
        "io_mode",
        "devices",
        "streams",
        "io_num",
        "bytes_per_device_iteration",
        "iterations",
        "exit_code",
        "submit_avg_us",
        "submit_p90_us",
        "group_wall_avg_us",
    }
    if not rows or not required.issubset(rows[0]):
        raise ValueError(f"missing required columns in {path}")

    bandwidth_field = "wall_bw_gib_s" if "wall_bw_gib_s" in rows[0] else "wall_bw_gb_s"
    for row in rows:
        for field in (
            "devices",
            "streams",
            "io_num",
            "bytes_per_device_iteration",
            "iterations",
            "exit_code",
            "submit_avg_us",
            "submit_p90_us",
            "group_wall_avg_us",
        ):
            row[field] = int(row[field])
        submit_calls = row.get("submit_calls_per_device_iteration") or row.get("io_calls_per_device_iteration")
        if submit_calls is None:
            raise ValueError(f"missing submit call count in row: {row}")
        row["submit_calls_per_device_iteration"] = int(submit_calls)
        row["wall_bw_gib_s"] = float(row[bandwidth_field])
    return rows


def validate(rows: list[dict[str, object]]) -> None:
    expected = {(io, devices, streams) for io in IO_MODES for devices in DEVICES for streams in STREAMS}
    actual = {(str(row["io_mode"]), int(row["devices"]), int(row["streams"])) for row in rows}
    if len(rows) != 18 or actual != expected:
        raise ValueError(f"expected a complete 18-row matrix, got {len(rows)} rows and {len(actual)} keys")
    for row in rows:
        if int(row["io_num"]) != 512 or int(row["iterations"]) != 128 or int(row["exit_code"]) != 0:
            raise ValueError(f"unexpected fixed field in row: {row}")
        calculated = (
            int(row["bytes_per_device_iteration"])
            * int(row["devices"])
            * 1_000_000
            / int(row["group_wall_avg_us"])
            / (1024**3)
        )
        if abs(calculated - float(row["wall_bw_gib_s"])) > 0.002:
            raise ValueError(
                f"WallBW mismatch for {row['io_mode']} d{row['devices']} s{row['streams']}: "
                f"reported={row['wall_bw_gib_s']}, calculated={calculated:.3f}"
            )


def row_index(rows: list[dict[str, object]]) -> dict[tuple[str, int, int], dict[str, object]]:
    return {
        (str(row["io_mode"]), int(row["devices"]), int(row["streams"])): row
        for row in rows
    }


def configure_style() -> None:
    plt.style.use("seaborn-v0_8-whitegrid")
    font_candidates = (
        Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts" / "msyh.ttc",
        Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
        Path("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc"),
    )
    font_family = "DejaVu Sans"
    for candidate in font_candidates:
        if candidate.exists():
            font_manager.fontManager.addfont(str(candidate))
            font_family = font_manager.FontProperties(fname=str(candidate)).get_name()
            break
    plt.rcParams.update(
        {
            "font.family": font_family,
            "font.size": 11,
            "axes.titlesize": 15,
            "axes.titleweight": "bold",
            "axes.labelsize": 12,
            "axes.edgecolor": "#30343B",
            "axes.linewidth": 0.8,
            "grid.color": "#D9DEE7",
            "grid.linewidth": 0.7,
            "grid.alpha": 0.8,
            "axes.unicode_minus": False,
        }
    )


def plot_time(rows: list[dict[str, object]], output: Path, title: str, subtitle: str) -> None:
    configure_style()
    index = row_index(rows)
    configurations = [(io_mode, devices) for io_mode in IO_MODES for devices in DEVICES]
    x = list(range(len(configurations)))

    figure, axes = plt.subplots(1, 3, figsize=(18, 7.8), dpi=180, sharey=True)
    figure.patch.set_facecolor("#F7F8FA")
    figure.suptitle(title, x=0.065, y=0.985, ha="left", fontsize=23, fontweight="bold", color="#172033")
    figure.text(0.065, 0.928, subtitle, ha="left", fontsize=11.5, color="#526071")

    max_group_wall_ms = max(int(row["group_wall_avg_us"]) for row in rows) / 1000
    y_max = max_group_wall_ms * 1.18
    for axis, streams in zip(axes, STREAMS):
        group_wall_values = [
            int(index[(io_mode, devices, streams)]["group_wall_avg_us"]) / 1000
            for io_mode, devices in configurations
        ]
        submit_avg_values = [
            int(index[(io_mode, devices, streams)]["submit_avg_us"]) / 1000
            for io_mode, devices in configurations
        ]

        axis.axvspan(-0.45, 2.5, color="#FFF1E8", alpha=0.70, zorder=0)
        axis.axvspan(2.5, 5.45, color="#EAF5FB", alpha=0.70, zorder=0)
        axis.axvline(2.5, color="#667085", linewidth=1.1, zorder=1)
        axis.plot(
            x,
            group_wall_values,
            color=COPY_COLOR,
            marker="o",
            markersize=7,
            markeredgecolor="white",
            markeredgewidth=1.2,
            linewidth=2.8,
            label="拷贝时间",
            zorder=4,
        )
        axis.plot(
            x,
            submit_avg_values,
            color=SUBMIT_COLOR,
            marker="s",
            markersize=6.5,
            markeredgecolor="white",
            markeredgewidth=1.1,
            linewidth=2.8,
            label="下发时间",
            zorder=5,
        )

        for position, value in zip(x, group_wall_values):
            axis.annotate(
                f"{value:.2f}",
                (position, value),
                xytext=(0, 9),
                textcoords="offset points",
                ha="center",
                va="bottom",
                fontsize=8.2,
                color=COPY_COLOR,
                fontweight="bold",
            )
        for position, value, group_value in zip(x, submit_avg_values, group_wall_values):
            place_above = value < y_max * 0.08 and group_value - value > y_max * 0.08
            axis.annotate(
                f"{value:.2f}",
                (position, value),
                xytext=(0, 9 if place_above else -11),
                textcoords="offset points",
                ha="center",
                va="bottom" if place_above else "top",
                fontsize=8.2,
                color=SUBMIT_COLOR,
                fontweight="bold",
            )

        axis.set_title(f"每卡 {streams} 个 Stream", pad=13)
        axis.set_xlim(-0.45, 5.45)
        axis.set_ylim(0, y_max)
        axis.set_xticks(x)
        axis.set_xticklabels([f"{devices} 卡" for _, devices in configurations], fontsize=10)
        axis.set_xlabel("设备数")
        axis.grid(axis="x", visible=False)
        axis.text(
            1,
            -0.13,
            "GLM5.1 小 IO",
            transform=axis.get_xaxis_transform(),
            ha="center",
            va="top",
            fontsize=10,
            fontweight="bold",
            color="#9A3412",
        )
        axis.text(
            4,
            -0.13,
            "1 MiB 大 IO",
            transform=axis.get_xaxis_transform(),
            ha="center",
            va="top",
            fontsize=10,
            fontweight="bold",
            color="#075985",
        )

    axes[0].set_ylabel("时间（ms）", fontweight="bold")

    legend_handles = [
        Line2D([0], [0], color=COPY_COLOR, marker="o", linewidth=2.8, label="拷贝时间"),
        Line2D([0], [0], color=SUBMIT_COLOR, marker="s", linewidth=2.8, label="下发时间"),
    ]
    figure.legend(
        handles=legend_handles,
        loc="upper right",
        bbox_to_anchor=(0.94, 0.985),
        frameon=False,
        ncol=2,
        fontsize=11,
    )
    figure.text(
        0.065,
        0.018,
        "拷贝时间覆盖同步起点到全部设备完成；下发时间只覆盖 Host 侧拷贝任务提交。图中均为 128 轮平均值。",
        fontsize=9.5,
        color="#667085",
    )
    figure.subplots_adjust(left=0.065, right=0.975, top=0.82, bottom=0.17, wspace=0.10)
    output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(output, bbox_inches="tight", facecolor=figure.get_facecolor())
    plt.close(figure)


def plot_bandwidth(rows: list[dict[str, object]], output: Path, title: str, subtitle: str) -> None:
    configure_style()
    index = row_index(rows)
    x = list(range(len(DEVICES)))

    figure, axes = plt.subplots(1, 3, figsize=(18, 7.8), dpi=180, sharey=True)
    figure.patch.set_facecolor("#F7F8FA")
    figure.suptitle(title, x=0.065, y=0.985, ha="left", fontsize=23, fontweight="bold", color="#172033")
    figure.text(0.065, 0.928, subtitle, ha="left", fontsize=11.5, color="#526071")

    y_max = max(float(row["wall_bw_gib_s"]) for row in rows) * 1.18
    for axis, streams in zip(axes, STREAMS):
        glm_values = [float(index[("glm5.1", devices, streams)]["wall_bw_gib_s"]) for devices in DEVICES]
        large_io_values = [float(index[("1m", devices, streams)]["wall_bw_gib_s"]) for devices in DEVICES]

        axis.plot(
            x,
            glm_values,
            color=GLM_COLOR,
            marker="s",
            markersize=7,
            markeredgecolor="white",
            markeredgewidth=1.1,
            linewidth=2.8,
            label="GLM5.1 小 IO",
            zorder=4,
        )
        axis.plot(
            x,
            large_io_values,
            color=LARGE_IO_COLOR,
            marker="o",
            markersize=7,
            markeredgecolor="white",
            markeredgewidth=1.2,
            linewidth=2.8,
            label="1 MiB 大 IO",
            zorder=4,
        )
        for position, value in zip(x, glm_values):
            axis.annotate(
                f"{value:.1f}",
                (position, value),
                xytext=(0, -12),
                textcoords="offset points",
                ha="center",
                va="top",
                fontsize=8.5,
                color=GLM_COLOR,
                fontweight="bold",
            )
        for position, value in zip(x, large_io_values):
            axis.annotate(
                f"{value:.1f}",
                (position, value),
                xytext=(0, 9),
                textcoords="offset points",
                ha="center",
                va="bottom",
                fontsize=8.5,
                color=LARGE_IO_COLOR,
                fontweight="bold",
            )

        axis.set_title(f"每卡 {streams} 个 Stream", pad=13)
        axis.set_xlim(-0.15, len(DEVICES) - 0.85)
        axis.set_ylim(0, y_max)
        axis.set_xticks(x)
        axis.set_xticklabels([f"{devices} 卡" for devices in DEVICES])
        axis.set_xlabel("设备数")
        axis.grid(axis="x", visible=False)

    axes[0].set_ylabel("聚合带宽（GiB/s）", fontweight="bold")
    legend_handles = [
        Line2D([0], [0], color=GLM_COLOR, marker="s", linewidth=2.8, label="GLM5.1 小 IO"),
        Line2D([0], [0], color=LARGE_IO_COLOR, marker="o", linewidth=2.8, label="1 MiB 大 IO"),
    ]
    figure.legend(
        handles=legend_handles,
        loc="upper right",
        bbox_to_anchor=(0.94, 0.985),
        frameon=False,
        ncol=2,
        fontsize=11,
    )
    figure.text(
        0.065,
        0.018,
        "聚合带宽 = 全部设备每轮总字节数 / 平均拷贝时间；程序按 1024³ 换算，因此单位标为 GiB/s。",
        fontsize=9.5,
        color="#667085",
    )
    figure.subplots_adjust(left=0.065, right=0.975, top=0.82, bottom=0.13, wspace=0.10)
    output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(output, bbox_inches="tight", facecolor=figure.get_facecolor())
    plt.close(figure)


def main() -> None:
    args = parse_args()
    rows = load_rows(args.data)
    validate(rows)
    if args.kind == "time":
        plot_time(rows, args.output, args.title, args.subtitle)
    else:
        plot_bandwidth(rows, args.output, args.title, args.subtitle)
    print(f"validated rows: {len(rows)}")
    print(f"image: {args.output}")


if __name__ == "__main__":
    main()
