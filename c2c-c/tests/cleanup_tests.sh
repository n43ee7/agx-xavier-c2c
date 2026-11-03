#!/bin/bash
# Undoes tests/setup_tests.sh: detaches leftover test remote RAM, stops c2c-memd
# on the slave, deletes the copied workloads (+ the test key if there is one).
#   sudo tests/cleanup_tests.sh
set -uo pipefail
TEST_ID=cleanup
source "$(dirname "$0")/lib/common.sh"
require_root
if c2c-rmem status >/dev/null 2>&1; then say "detaching remote memory"; c2c-rmem detach; fi
if c2crun true 2>/dev/null; then
    c2crun "pkill -x c2c-memd; rm -rf ~/$PEER_DIR; sed -i '/ c2c-test\$/d' ~/.ssh/authorized_keys" || true
    say "removed test files and key from $PEER"
fi
c2crun_close
rm -f "$TEST_KEY" "$TEST_KEY.pub" "$TEST_KNOWN"
rm -rf "$BUILD"
say "clean (results in tests/results/ are kept)"
