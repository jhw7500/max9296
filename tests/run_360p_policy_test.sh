#!/bin/bash
set -Eeuo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_bin=$(mktemp /tmp/test-max9296-360p-policy.XXXXXX)

cleanup()
{
    rm -f "$test_bin"
}
trap cleanup EXIT

cc -std=c11 -Wall -Wextra -Werror \
    "$repo_dir/tests/max9296_360p_policy_test.c" \
    -o "$test_bin"

"$test_bin"
