#!/usr/bin/env python3

from __future__ import annotations

import csv
import math
import os
import tempfile
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "ucm-plot-cache"))

from PIL import Image, ImageDraw, ImageFont


REPO_ROOT = Path(__file__).resolve().parent.parent
DATA_PATH = REPO_ROOT / "docs" / "data" / "ascend_h2d_all_host_ce_io_matrix_20260909.tsv"
REPORT_PATH = REPO_ROOT / "docs" / "ascend_h2d_all_host_ce_io_matrix_report_20260909.md"
IMAGE_DIR = REPO_ROOT / "docs" / "images"
SUBMIT_IMAGE = IMAGE_DIR / "ascend_h2d_all_host_ce_submit_avg_20260909.png"
WALL_BW_IMAGE = IMAGE_DIR / "ascend_h2d_all_host_ce_wallbw_20260909.png"

IO_MODES = ("glm5.1", "1m")
DEVICES = (1, 4, 8)
STREAMS = (1, 4, 16)
COLORS = {1: "#2563EB", 4: "#E76F51", 8: "#159947"}


def load_rows() -> list[dict[str, object]]:
    with DATA_PATH.open("r", encoding="utf-8-sig", newline="") as file:
        rows = list(csv.DictReader(file, delimiter="\t"))

    integer_fields = (
        "devices",
        "streams",
        "io_num",
        "io_calls_per_device_iteration",
        "bytes_per_device_iteration",
        "iterations",
        "exit_code",
        "submit_avg_us",
        "submit_p90_us",
        "start_skew_max_us",
        "start_skew_avg_us",
        "start_skew_p90_us",
        "group_wall_avg_us",
        "group_wall_p90_us",
    )
    for row in rows:
        for field in integer_fields:
            row[field] = int(row[field])
        row["wall_bw_gb_s"] = float(row["wall_bw_gb_s"])
    return rows


def validate(rows: list[dict[str, object]]) -> None:
    expected = {(io, devices, streams) for io in IO_MODES for devices in DEVICES for streams in STREAMS}
    actual = {(str(row["io_mode"]), int(row["devices"]), int(row["streams"])) for row in rows}
    if len(rows) != 18 or actual != expected:
        raise ValueError(f"expected the complete 18-row matrix, got {len(rows)} rows")

    for row in rows:
        if row["io_num"] != 512 or row["iterations"] != 128 or row["exit_code"] != 0:
            raise ValueError(f"unexpected fixed field in row: {row}")
        calculated = (
            int(row["bytes_per_device_iteration"])
            * int(row["devices"])
            * 1_000_000
            / int(row["group_wall_avg_us"])
            / (1024**3)
        )
        if abs(calculated - float(row["wall_bw_gb_s"])) > 0.002:
            raise ValueError(
                f"WallBW mismatch for {row['io_mode']} d{row['devices']} s{row['streams']}: "
                f"reported={row['wall_bw_gb_s']}, calculated={calculated:.3f}"
            )


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = []
    windows = Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts"
    if bold:
        candidates.extend((windows / "arialbd.ttf", windows / "msyhbd.ttc"))
    else:
        candidates.extend((windows / "arial.ttf", windows / "msyh.ttc"))
    candidates.extend(
        (
            Path("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
            Path("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"),
        )
    )
    for candidate in candidates:
        try:
            return ImageFont.truetype(str(candidate), size=size)
        except OSError:
            continue
    return ImageFont.load_default()


def nice_ceiling(value: float) -> float:
    exponent = 10 ** math.floor(math.log10(value))
    scaled = value / exponent
    for ceiling in (1, 1.25, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10):
        if scaled <= ceiling:
            return ceiling * exponent
    return 10 * exponent


def draw_chart(
    rows: list[dict[str, object]],
    output: Path,
    field: str,
    scale: float,
    title: str,
    subtitle: str,
    y_label: str,
) -> None:
    width, height = 1800, 900
    image = Image.new("RGB", (width, height), "#F5F7FB")
    draw = ImageDraw.Draw(image)
    title_font = font(38, bold=True)
    subtitle_font = font(22)
    panel_title_font = font(25, bold=True)
    axis_font = font(20)
    tick_font = font(18)
    value_font = font(17, bold=True)

    draw.text((80, 42), title, fill="#172033", font=title_font)
    draw.text((80, 94), subtitle, fill="#566176", font=subtitle_font)

    legend_y = 140
    legend_x = 80
    for devices in DEVICES:
        draw.line((legend_x, legend_y + 10, legend_x + 42, legend_y + 10), fill=COLORS[devices], width=5)
        draw.ellipse((legend_x + 16, legend_y + 3, legend_x + 30, legend_y + 17), fill=COLORS[devices])
        draw.text((legend_x + 52, legend_y - 2), f"{devices} device{'s' if devices > 1 else ''}", fill="#263247", font=tick_font)
        legend_x += 210

    panel_top, panel_bottom = 225, 790
    panel_lefts = (80, 930)
    panel_width = 790
    plot_pad_left, plot_pad_right = 100, 35
    plot_pad_top, plot_pad_bottom = 75, 70

    for io_mode, panel_left in zip(IO_MODES, panel_lefts):
        draw.rounded_rectangle(
            (panel_left, panel_top, panel_left + panel_width, panel_bottom),
            radius=18,
            fill="#FFFFFF",
            outline="#D8DFEA",
            width=2,
        )
        panel_rows = [row for row in rows if row["io_mode"] == io_mode]
        descriptor = "GLM5.1: 1,536 calls, 88 MiB/device/iteration" if io_mode == "glm5.1" else "1 MiB: 512 calls, 512 MiB/device/iteration"
        draw.text((panel_left + 28, panel_top + 20), descriptor, fill="#172033", font=panel_title_font)

        plot_left = panel_left + plot_pad_left
        plot_right = panel_left + panel_width - plot_pad_right
        plot_top = panel_top + plot_pad_top
        plot_bottom = panel_bottom - plot_pad_bottom
        max_value = max(float(row[field]) * scale for row in panel_rows)
        y_max = nice_ceiling(max_value * 1.16)
        x_positions = {
            stream: plot_left + index * (plot_right - plot_left) / (len(STREAMS) - 1)
            for index, stream in enumerate(STREAMS)
        }

        for tick in range(6):
            y_value = y_max * tick / 5
            y = plot_bottom - (plot_bottom - plot_top) * tick / 5
            draw.line((plot_left, y, plot_right, y), fill="#DDE3EC", width=2)
            label = f"{y_value:.0f}" if y_max >= 20 else f"{y_value:.1f}"
            draw.text((plot_left - 18, y), label, fill="#5D687B", font=tick_font, anchor="rm")

        draw.line((plot_left, plot_top, plot_left, plot_bottom), fill="#8893A5", width=2)
        draw.line((plot_left, plot_bottom, plot_right, plot_bottom), fill="#8893A5", width=2)
        for stream, x in x_positions.items():
            draw.text((x, plot_bottom + 18), str(stream), fill="#4B566A", font=tick_font, anchor="ma")

        draw.text(((plot_left + plot_right) / 2, panel_bottom - 22), "Streams per device", fill="#4B566A", font=axis_font, anchor="mm")
        draw.text(
            (plot_left + 12, plot_top + 10),
            y_label,
            fill="#4B566A",
            font=axis_font,
            anchor="la",
        )

        for devices in DEVICES:
            series = sorted(
                (row for row in panel_rows if row["devices"] == devices),
                key=lambda row: int(row["streams"]),
            )
            points = []
            for row in series:
                value = float(row[field]) * scale
                x = x_positions[int(row["streams"])]
                y = plot_bottom - value / y_max * (plot_bottom - plot_top)
                points.append((x, y, value))
            draw.line([(x, y) for x, y, _ in points], fill=COLORS[devices], width=5)
            for x, y, value in points:
                draw.ellipse((x - 8, y - 8, x + 8, y + 8), fill=COLORS[devices], outline="#FFFFFF", width=2)
                label = f"{value:.3f}" if field == "wall_bw_gb_s" else f"{value:.2f}"
                if field == "submit_avg_us" and devices == 8:
                    draw.text((x, y + 15), label, fill=COLORS[devices], font=value_font, anchor="mt")
                else:
                    draw.text((x, y - 15), label, fill=COLORS[devices], font=value_font, anchor="mb")

    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output, format="PNG", optimize=True)


def get_row(rows: list[dict[str, object]], io_mode: str, devices: int, streams: int) -> dict[str, object]:
    return next(
        row
        for row in rows
        if row["io_mode"] == io_mode and row["devices"] == devices and row["streams"] == streams
    )


def percent_change(new: float, old: float) -> float:
    return (new / old - 1) * 100


def build_table(rows: list[dict[str, object]]) -> str:
    lines = [
        "| IO 模式 | 卡数 | Streams | Submit Avg / P90 (us) | GroupWall Avg / P90 (us) | WallBW (GiB/s) |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for io_mode in IO_MODES:
        for devices in DEVICES:
            for streams in STREAMS:
                row = get_row(rows, io_mode, devices, streams)
                lines.append(
                    f"| {io_mode} | {devices} | {streams} | "
                    f"{row['submit_avg_us']} / {row['submit_p90_us']} | "
                    f"{row['group_wall_avg_us']} / {row['group_wall_p90_us']} | "
                    f"{float(row['wall_bw_gb_s']):.3f} |"
                )
    return "\n".join(lines)


def build_report(rows: list[dict[str, object]]) -> str:
    glm_d4_s4 = get_row(rows, "glm5.1", 4, 4)
    glm_d4_s16 = get_row(rows, "glm5.1", 4, 16)
    glm_d8_s4 = get_row(rows, "glm5.1", 8, 4)
    glm_d8_s16 = get_row(rows, "glm5.1", 8, 16)
    large_d4_s4 = get_row(rows, "1m", 4, 4)
    large_d4_s16 = get_row(rows, "1m", 4, 16)
    large_d8_s4 = get_row(rows, "1m", 8, 4)
    large_d8_s16 = get_row(rows, "1m", 8, 16)

    glm_submit_share = [float(row["submit_avg_us"]) / float(row["group_wall_avg_us"]) * 100 for row in rows if row["io_mode"] == "glm5.1"]
    large_submit_share = [float(row["submit_avg_us"]) / float(row["group_wall_avg_us"]) * 100 for row in rows if row["io_mode"] == "1m"]

    return f"""# Atlas A5 All-Host CE H2D IO 矩阵报告

## 1. 测试范围与统计口径

本报告整理运行标识 `20260909_100102` 的 18 组 Atlas A5 H2D 实测结果。所有 case 均执行成功，`exit_code=0`，且未开启 `COPY_ASCEND_PHASE_TRACE`。

- Case：`all_host_to_all_device_ce_multi_stream`
- IO 模式：`glm5.1`、1 MiB uniform IO
- Devices：1、4、8
- Streams：1、4、16
- 固定参数：`-n 512 -i 128 --submit-mode round-robin --process-sync barrier --stream-start-gate off --stream-sync stream`
- GLM5.1：每卡每轮 512 blocks，每 block 为 128 KiB + 16 KiB + 32 KiB，共 1,536 次 `aclrtMemcpyAsync`、88 MiB
- 1 MiB：每卡每轮 512 次 `aclrtMemcpyAsync`、512 MiB

### 1.1 Submit 口径

这批结果生成于 commit `8c1a12b` 修改跨卡汇总方式之前。多卡 `Submit Avg` 的计算方式是：

```text
第 i 轮合并值 = max(设备 0 第 i 轮 Submit, ..., 设备 N-1 第 i 轮 Submit)
Submit Avg     = 128 个“每轮最大值”的算术平均
Submit P90     = 128 个“每轮最大值”的 P90
```

单卡时没有跨卡合并，因此就是该卡自身 128 轮的 Submit Avg/P90。这里的 Submit 只覆盖 Host 调用异步拷贝 API 的下发阶段，API 返回不代表 H2D 已经完成。

### 1.2 墙钟带宽口径

本报告只采用 `WallBW` 判断带宽，不使用 device Event 带宽。`GroupWall` 和 `WallBW` 定义为：

```text
StartSkew = max(device_start) - min(device_start)
GroupWall = max(device_end)   - min(device_start)
WallBW    = 所有设备每轮总字节数 / GroupWall Avg
```

因此，WallBW 覆盖 barrier 释放后的 Host 侧起点偏差、异步提交、设备执行以及逐 stream 同步完成，是整个多卡 group 的端到端聚合墙钟带宽。运行程序和 TSV 列名写作 `GB/s`，但实现按 `1024^3` 换算，本报告统一标为 **GiB/s**。

## 2. 核心结论

1. **4 streams 是这组矩阵最稳定的选择。** 1 MiB 模式在 1、4、8 卡下的最佳 WallBW 都出现在 4 streams，分别为 **46.755、118.106、270.289 GiB/s**。GLM5.1 在 4、8 卡下也由 4 streams 最优，分别为 **60.075、115.566 GiB/s**；单卡 16 streams 的 **17.288 GiB/s** 只比 4 streams 高约 **2.4%**。

2. **16 streams 在多卡上出现回退。** 相比 4 streams，GLM5.1 的 4 卡和 8 卡 WallBW 分别变化 **{percent_change(float(glm_d4_s16['wall_bw_gb_s']), float(glm_d4_s4['wall_bw_gb_s'])):.1f}%**、**{percent_change(float(glm_d8_s16['wall_bw_gb_s']), float(glm_d8_s4['wall_bw_gb_s'])):.1f}%**；1 MiB 分别变化 **{percent_change(float(large_d4_s16['wall_bw_gb_s']), float(large_d4_s4['wall_bw_gb_s'])):.1f}%**、**{percent_change(float(large_d8_s16['wall_bw_gb_s']), float(large_d8_s4['wall_bw_gb_s'])):.1f}%**。当前数据能证明 16 streams 没有收益，但不能仅凭汇总表区分 CE 调度、Runtime 队列、Host 内存或拓扑中的具体限制。

3. **GLM5.1 的墙钟时间几乎全部落在 Submit 区间。** 各配置 `Submit Avg / GroupWall Avg` 为 **{min(glm_submit_share):.1f}%–{max(glm_submit_share):.1f}%**。每卡每轮需要发起 1,536 次小拷贝，设备执行又会与 Host 后续下发重叠，因此这说明下发路径和队列背压占据关键路径，不表示 `aclrtMemcpyAsync` 变成同步接口。

4. **1 MiB 模式呈现清晰的异步排队。** `Submit Avg / GroupWall Avg` 只有 **{min(large_submit_share):.1f}%–{max(large_submit_share):.1f}%**，大量时间位于提交结束之后的设备执行和 stream 同步阶段。最快 Submit 不必然对应最高带宽，例如 4 卡 1 stream 的 Submit 为 **2.310 ms**，低于 4 streams 的 **3.027 ms**，但 WallBW 只有 **68.918 GiB/s**，明显低于 4 streams 的 **118.106 GiB/s**。

5. **本轮峰值均来自 8 卡 4 streams。** GLM5.1 为 **{float(glm_d8_s4['wall_bw_gb_s']):.3f} GiB/s**，1 MiB 为 **{float(large_d8_s4['wall_bw_gb_s']):.3f} GiB/s**。两种 IO 的单卡数据量和 API 次数不同，不能把二者的绝对带宽差直接归因于 IO 大小这一项。

## 3. Python 可视化

### 3.1 Host Submit Avg

![Host Submit Avg](images/{SUBMIT_IMAGE.name})

纵轴是毫秒。多卡曲线使用本批旧口径，即每轮先取各卡 Submit 最大值，再对 128 轮求平均。

### 3.2 基于 GroupWall 的聚合带宽

![Wall-clock aggregate bandwidth](images/{WALL_BW_IMAGE.name})

纵轴是 GiB/s。每个点均使用所有设备总字节数除以对应的 `GroupWall Avg`。

## 4. 完整结果

{build_table(rows)}

完整原始 TSV：[`docs/data/{DATA_PATH.name}`](data/{DATA_PATH.name})。

## 5. 结果边界

- `--stream-sync stream` 不生成 device Event Copy 时间，因此本批结果不能拆出纯 DMA 时间；`GroupWall` 是端到端墙钟时间。
- StartSkew 的 P90 在全部多卡配置中均为 1 us，但少数组合出现 577–3290 us 的 Max。由于 WallBW 基于 GroupWall，这些罕见进程调度偏差已经进入端到端结果，没有被剔除。
- `1m / 8 卡 / 4 streams` 的 Submit Avg 为 1727 us，而 P90 为 1705 us。算术平均高于 P90 是允许的，表示最高 10% 内可能有长尾；当前 TSV 未保存 Submit Max，无法从 summary 精确还原长尾幅度。
- commit `8c1a12b` 已把后续多卡 Submit/Copy 输出改成“各卡同名统计值的平均”。新旧口径必须分开标注，不能直接把两批 Submit 数据拼接比较。
"""


def main() -> None:
    rows = load_rows()
    validate(rows)
    draw_chart(
        rows,
        SUBMIT_IMAGE,
        "submit_avg_us",
        0.001,
        "Host submit time",
        "Old aggregation: mean across iterations of the per-iteration maximum across devices",
        "Submit avg (ms)",
    )
    draw_chart(
        rows,
        WALL_BW_IMAGE,
        "wall_bw_gb_s",
        1.0,
        "Aggregate H2D wall-clock bandwidth",
        "Total bytes across devices divided by GroupWall average",
        "WallBW (GiB/s)",
    )
    REPORT_PATH.write_text(build_report(rows), encoding="utf-8", newline="\n")
    print(f"validated rows: {len(rows)}")
    print(f"report: {REPORT_PATH.relative_to(REPO_ROOT)}")
    print(f"images: {SUBMIT_IMAGE.relative_to(REPO_ROOT)}, {WALL_BW_IMAGE.relative_to(REPO_ROOT)}")


if __name__ == "__main__":
    main()
