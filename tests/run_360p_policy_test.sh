#!/bin/bash
set -Eeuo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_bin=$(mktemp /tmp/test-max9296-360p-policy.XXXXXX)
restricted_bin=$(mktemp /tmp/test-max9296-360p-policy-restricted.XXXXXX)

cleanup()
{
    rm -f "$test_bin" "$restricted_bin"
}
trap cleanup EXIT

cc -std=c11 -Wall -Wextra -Werror \
    "$repo_dir/tests/max9296_360p_policy_test.c" \
    -o "$test_bin"

"$test_bin"

cc -std=c11 -Wall -Wextra -Werror \
    -DMAX9296_360P_MAX_FPS=30U \
    -DMAX9296_360P_EXPECTED_MAX_FPS=30U \
    -DMAX9296_HD_MAX_FPS=30U \
    -DMAX9296_HD_EXPECTED_MAX_FPS=30U \
    "$repo_dir/tests/max9296_360p_policy_test.c" \
    -o "$restricted_bin"

"$restricted_bin"
