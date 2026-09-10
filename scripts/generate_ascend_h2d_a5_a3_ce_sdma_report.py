#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path

from plot_ascend_h2d_all_host_group import (
    DEVICES,
    IO_MODES,
    STREAMS,
    load_rows,
    plot_bandwidth,
    plot_time,
    row_index,
    validate,
)


REPO_ROOT = Path(__file__).resolve().parent.parent
DATA_DIR = REPO_ROOT / "docs" / "data"
IMAGE_DIR = REPO_ROOT / "docs" / "images"
REPORT_PATH = REPO_ROOT / "docs" / "ascend_h2d_a5_a3_ce_sdma_io_matrix_report_20260912.md"

GROUPS = {
    "a5_ce": {
        "label": "Atlas A5 标准 CE",
        "data": DATA_DIR / "ascend_h2d_all_host_a5_ce_io_matrix_20260910.tsv",
        "run": "20260910_020859",
        "case": "all_host_to_all_device_ce_multi_stream",
        "time_image": IMAGE_DIR / "ascend_h2d_all_host_a5_ce_copy_submit_20260910.png",
        "bandwidth_image": IMAGE_DIR / "ascend_h2d_all_host_a5_ce_bandwidth_20260910.png",
    },
    "a3_ce": {
        "label": "Atlas A3 标准 CE",
        "data": DATA_DIR / "ascend_h2d_all_host_a3_ce_io_matrix_20260912.tsv",
        "run": "20260912_095638",
        "case": "all_host_to_all_device_ce_multi_stream",
        "time_image": IMAGE_DIR / "ascend_h2d_all_host_a3_ce_copy_submit_20260912.png",
        "bandwidth_image": IMAGE_DIR / "ascend_h2d_all_host_a3_ce_bandwidth_20260912.png",
    },
    "a3_sdma": {
        "label": "Atlas A3 SDMA Direct",
        "data": DATA_DIR / "ascend_h2d_all_host_a3_sdma_io_matrix_20260912.tsv",
        "run": "20260912_095638",
        "case": "all_host_to_all_device_ffts_direct_h2d",
        "time_image": IMAGE_DIR / "ascend_h2d_all_host_a3_sdma_copy_submit_20260912.png",
        "bandwidth_image": IMAGE_DIR / "ascend_h2d_all_host_a3_sdma_bandwidth_20260912.png",
    },
}


def select(rows: list[dict[str, object]], io_mode: str) -> list[dict[str, object]]:
    return [row for row in rows if row["io_mode"] == io_mode]


def range_text(values: list[float], digits: int = 2) -> str:
    return f"{min(values):.{digits}f}–{max(values):.{digits}f}"


def submit_shares(rows: list[dict[str, object]], io_mode: str) -> list[float]:
    return [
        int(row["submit_avg_us"]) / int(row["group_wall_avg_us"]) * 100
        for row in select(rows, io_mode)
    ]


def best_bandwidth(rows: list[dict[str, object]], io_mode: str) -> dict[str, object]:
    return max(select(rows, io_mode), key=lambda row: float(row["wall_bw_gib_s"]))


def summary_table(rows_by_group: dict[str, list[dict[str, object]]]) -> str:
    lines = [
        "| 平台与方法 | IO 模式 | 下发时间范围（ms） | 拷贝时间范围（ms） | 下发时间占比 | 峰值带宽（GiB/s） | 峰值配置 |",
        "|---|---|---:|---:|---:|---:|---|",
    ]
    for group_key, group in GROUPS.items():
        rows = rows_by_group[group_key]
        for io_mode in IO_MODES:
            selected = select(rows, io_mode)
            submit_ms = [int(row["submit_avg_us"]) / 1000 for row in selected]
            copy_ms = [int(row["group_wall_avg_us"]) / 1000 for row in selected]
            best = best_bandwidth(rows, io_mode)
            lines.append(
                f"| {group['label']} | {io_mode} | {range_text(submit_ms)} | "
                f"{range_text(copy_ms)} | {range_text(submit_shares(rows, io_mode), 1)}% | "
                f"{float(best['wall_bw_gib_s']):.3f} | {best['devices']} 卡 / {best['streams']} Streams |"
            )
    return "\n".join(lines)


def a3_comparison_table(
    ce_rows: list[dict[str, object]], sdma_rows: list[dict[str, object]]
) -> str:
    ce_index = row_index(ce_rows)
    sdma_index = row_index(sdma_rows)
    lines = [
        "| IO | 卡数 | Streams | CE 下发（ms） | SDMA 下发（ms） | CE 拷贝（ms） | SDMA 拷贝（ms） | CE 带宽 | SDMA 带宽 |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for io_mode in IO_MODES:
        for devices in DEVICES:
            for streams in STREAMS:
                key = (io_mode, devices, streams)
                ce = ce_index[key]
                sdma = sdma_index[key]
                lines.append(
                    f"| {io_mode} | {devices} | {streams} | "
                    f"{int(ce['submit_avg_us']) / 1000:.3f} | {int(sdma['submit_avg_us']) / 1000:.3f} | "
                    f"{int(ce['group_wall_avg_us']) / 1000:.3f} | {int(sdma['group_wall_avg_us']) / 1000:.3f} | "
                    f"{float(ce['wall_bw_gib_s']):.3f} | {float(sdma['wall_bw_gib_s']):.3f} |"
                )
    return "\n".join(lines)


def detail_table(rows: list[dict[str, object]]) -> str:
    lines = [
        "| IO | 卡数 | Streams | 下发 Avg/P90（us） | 拷贝 Avg/P90（us） | 带宽（GiB/s） |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    index = row_index(rows)
    for io_mode in IO_MODES:
        for devices in DEVICES:
            for streams in STREAMS:
                row = index[(io_mode, devices, streams)]
                lines.append(
                    f"| {io_mode} | {devices} | {streams} | "
                    f"{row['submit_avg_us']}/{row['submit_p90_us']} | "
                    f"{row['group_wall_avg_us']}/{row['group_wall_p90_us']} | "
                    f"{float(row['wall_bw_gib_s']):.3f} |"
                )
    return "\n".join(lines)


def build_report(rows_by_group: dict[str, list[dict[str, object]]]) -> str:
    a5 = rows_by_group["a5_ce"]
    a3_ce = rows_by_group["a3_ce"]
    a3_sdma = rows_by_group["a3_sdma"]

    group_sections = []
    detail_sections = []
    for group_key in ("a5_ce", "a3_ce", "a3_sdma"):
        group = GROUPS[group_key]
        group_sections.append(
            f"""### {group['label']}

#### 拷贝时间与下发时间

![{group['label']} 拷贝时间与下发时间](images/{group['time_image'].name})

#### 聚合带宽

![{group['label']} 聚合带宽](images/{group['bandwidth_image'].name})"""
        )
        detail_sections.append(
            f"""<details>
<summary>{group['label']} 完整 18 组结果</summary>

{detail_table(rows_by_group[group_key])}

</details>"""
        )

    group_sections_text = "\n\n".join(group_sections)
    detail_sections_text = "\n\n".join(detail_sections)

    a5_glm_best = best_bandwidth(a5, "glm5.1")
    a5_large_best = best_bandwidth(a5, "1m")
    a3_ce_glm_best = best_bandwidth(a3_ce, "glm5.1")
    a3_ce_large_best = best_bandwidth(a3_ce, "1m")
    a3_sdma_glm_best = best_bandwidth(a3_sdma, "glm5.1")
    a3_sdma_large_best = best_bandwidth(a3_sdma, "1m")

    return f"""# Atlas A5/A3 All-Host H2D 拷贝性能对比

## 1. 测试配置

### 1.1 测试 Case

| 平台与方法 | Case | 运行标识 |
|---|---|---|
| Atlas A5 标准 CE | `{GROUPS['a5_ce']['case']}` | `{GROUPS['a5_ce']['run']}` |
| Atlas A3 标准 CE | `{GROUPS['a3_ce']['case']}` | `{GROUPS['a3_ce']['run']}` |
| Atlas A3 SDMA Direct | `{GROUPS['a3_sdma']['case']}` | `{GROUPS['a3_sdma']['run']}` |

- IO 模式：GLM5.1、1 MiB 大 IO。
- 卡数：1、4、8。
- 每卡 Streams：1、4、16。
- 固定参数：`-n 512 -i 128`。
- A3 SDMA 使用 `FFTS_MAX_READY_LANES=3`。

### 1.2 IO 规模

| IO 模式 | 每卡每轮数据量 | IO 描述数 | 标准 CE 下发次数 | SDMA Direct 下发次数 |
|---|---:|---:|---:|---:|
| GLM5.1 | 88 MiB | 1,536 | 1,536 | 512 |
| 1 MiB | 512 MiB | 512 | 512 | 512 |

GLM5.1 每个 block 包含 128 KiB、16 KiB、32 KiB 三段。标准 CE 分别下发三段，SDMA Direct 将三段组织成一个任务。

### 1.3 指标口径

- **下发时间**：`Submit Avg`。单卡为该卡自身统计值；多卡为各卡 `Submit Avg` 的算术平均。
- **拷贝时间**：`GroupWall Avg`。从同步起点到该轮全部设备进程完成，按 128 轮求平均。
- **聚合带宽**：全部设备每轮总字节数除以平均拷贝时间，单位为 GiB/s。

## 2. 可视化结果

{group_sections_text}

## 3. 测试数据

### 3.1 结果汇总

{summary_table(rows_by_group)}

### 3.2 A3 标准 CE 与 SDMA 对比

带宽单位为 GiB/s。

{a3_comparison_table(a3_ce, a3_sdma)}

### 3.3 完整数据

{detail_sections_text}

原始数据：

- [`{GROUPS['a5_ce']['data'].name}`](data/{GROUPS['a5_ce']['data'].name})
- [`{GROUPS['a3_ce']['data'].name}`](data/{GROUPS['a3_ce']['data'].name})
- [`{GROUPS['a3_sdma']['data'].name}`](data/{GROUPS['a3_sdma']['data'].name})

数据说明：

- “拷贝时间”是完整一轮的端到端时间，不等同于纯 DMA 时间。
- A5 `1 MiB / 8 卡 / 16 Streams` 的下发 Avg/P90 为 8.158/12.077 ms，是本批数据中的明显长尾点。
- A3 标准 CE 部分配置的拷贝 P90 低于 Avg，说明少量样本存在较长尾部。

## 4. 结论

### 4.1 带宽

- A5 标准 CE：GLM5.1 峰值 **{float(a5_glm_best['wall_bw_gib_s']):.3f} GiB/s**，1 MiB 峰值 **{float(a5_large_best['wall_bw_gib_s']):.3f} GiB/s**。
- A3 标准 CE：GLM5.1 峰值 **{float(a3_ce_glm_best['wall_bw_gib_s']):.3f} GiB/s**，1 MiB 峰值 **{float(a3_ce_large_best['wall_bw_gib_s']):.3f} GiB/s**。
- A3 SDMA Direct：GLM5.1 峰值 **{float(a3_sdma_glm_best['wall_bw_gib_s']):.3f} GiB/s**，1 MiB 峰值 **{float(a3_sdma_large_best['wall_bw_gib_s']):.3f} GiB/s**。
- A3 的 GLM5.1 在全部配置中都是 SDMA 带宽更高；1 MiB 有 7/9 个配置是 SDMA 更高。

### 4.2 下发时间占比

- A5 标准 CE：GLM5.1 为 **{range_text(submit_shares(a5, 'glm5.1'), 1)}%**，1 MiB 为 **{range_text(submit_shares(a5, '1m'), 1)}%**。
- A3 标准 CE：GLM5.1 为 **{range_text(submit_shares(a3_ce, 'glm5.1'), 1)}%**，1 MiB 为 **{range_text(submit_shares(a3_ce, '1m'), 1)}%**。
- A3 SDMA Direct：GLM5.1 为 **{range_text(submit_shares(a3_sdma, 'glm5.1'), 1)}%**，1 MiB 为 **{range_text(submit_shares(a3_sdma, '1m'), 1)}%**。
- A5 的 GLM5.1 下发时间占比达到 **90.5%–99.7%**，说明这组小 IO 的拷贝时间基本由下发阶段决定。
"""


def main() -> None:
    rows_by_group: dict[str, list[dict[str, object]]] = {}
    for group_key, group in GROUPS.items():
        rows = load_rows(group["data"])
        validate(rows)
        rows_by_group[group_key] = rows
        plot_time(
            rows,
            group["time_image"],
            f"{group['label']} H2D：拷贝时间与下发时间",
            "每个面板固定 Stream 数；两条线越接近，说明下发阶段在整轮拷贝中的占比越高",
        )
        plot_bandwidth(
            rows,
            group["bandwidth_image"],
            f"{group['label']} H2D：聚合带宽",
            "分别展示每卡 1、4、16 个 Stream；横轴为参与拷贝的设备数",
        )

    REPORT_PATH.write_text(build_report(rows_by_group), encoding="utf-8", newline="\n")
    print(f"validated groups: {len(rows_by_group)}")
    print(f"validated rows: {sum(len(rows) for rows in rows_by_group.values())}")
    print(f"report: {REPORT_PATH.relative_to(REPO_ROOT)}")


if __name__ == "__main__":
    main()
