#!/bin/bash
set -eu

cd "$(dirname "$0")/.."
bash tests/run_360p_policy_test.sh
bash tests/run_exposure_policy_test.sh
python3 tests/max9296_exposure_replay_binding_test.py
python3 tests/max9296_exposure_failure_test.py
python3 tests/max9296_cold_init_epoch_test.py
python3 tests/max9296_ci_contract_test.py
bash tests/run_pair_health_test.sh
python3 tests/max9296_pair_health_source_test.py
bash tests/build_360p_candidates_test.sh
bash tests/run_360p_readout_compare_test.sh
python3 tests/max9296_health_export_test.py
python3 tests/max9296_probe_cleanup_test.py
python3 tests/max9296_prepare_test.py
python3 tests/max9296_360p_zoom_exposure_test.py
bash tests/cam_fps_stack_mode_test.sh
bash tests/cam_360p_resource_test.sh
python3 tests/uyvy_frame_check_test.py
python3 tests/rgb565_frame_check_test.py
