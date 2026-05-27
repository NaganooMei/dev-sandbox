#!/usr/bin/env python3
"""Run only the eight-device H2D FFTS pipeline topology experiment."""

from run_h2d_ffts_pipeline_share_experiments import main


if __name__ == "__main__":
    raise SystemExit(main(default_suite="exp3"))
