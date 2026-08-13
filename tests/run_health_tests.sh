#!/bin/bash
set -eu

cd "$(dirname "$0")/.."
python3 tests/max9296_health_export_test.py
python3 tests/max9296_probe_cleanup_test.py
