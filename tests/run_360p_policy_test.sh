#!/bin/bash
set -Eeuo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_bin=$(mktemp /tmp/test-max9296-360p-policy.XXXXXX)
qualification_bin=$(mktemp /tmp/test-max9296-360p-policy-qualification.XXXXXX)

cleanup()
{
    rm -f "$test_bin" "$qualification_bin"
}
trap cleanup EXIT

cc -std=c11 -Wall -Wextra -Werror \
    "$repo_dir/tests/max9296_360p_policy_test.c" \
    -o "$test_bin"

"$test_bin"

cc -std=c11 -Wall -Wextra -Werror \
    -DMAX9296_360P_MAX_FPS=120U \
    -DMAX9296_360P_EXPECTED_MAX_FPS=120U \
    "$repo_dir/tests/max9296_360p_policy_test.c" \
    -o "$qualification_bin"

"$qualification_bin"
