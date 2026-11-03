#!/bin/bash
# One-time prep for the proof tests (run it on the MASTER):
#   sudo C2C_PEER_PASS=<slave password> tests/setup_tests.sh   (or omit it and type it)
#   sudo tests/setup_tests.sh --with-key    also install a dedicated test SSH key
# - copies tests/workloads to the slave (~/.cache/c2c-tests) over the link and
#   builds them on both boards
# - --with-key: makes /etc/c2c/test_id_ed25519 and authorizes it on the slave
#   (tagged "c2c-test", only accepted from the master's link IP) so later runs
#   don't ask for a password
# Undo with tests/cleanup_tests.sh.
set -euo pipefail
TEST_ID=setup
source "$(dirname "$0")/lib/common.sh"
require_root
if [ "${1:-}" = --with-key ]; then
    MASTER_IP="$(link_json | python3 -c 'import json,sys; print(json.load(sys.stdin)["local_ip"])')"
    [ -f "$TEST_KEY" ] || { say "creating test key $TEST_KEY"; ssh-keygen -q -t ed25519 -N '' -C c2c-test -f "$TEST_KEY"; }
    line="from=\"$MASTER_IP\",no-agent-forwarding,no-X11-forwarding $(cat "$TEST_KEY.pub")"
    say "authorising the test key on $PEER_USER@$PEER"
    mv "$TEST_KEY" "$TEST_KEY.new"   # password login just this once
    c2crun "umask 077; mkdir -p ~/.ssh; grep -qF '$(cut -d' ' -f2 "$TEST_KEY.pub")' ~/.ssh/authorized_keys 2>/dev/null || echo '$line' >> ~/.ssh/authorized_keys"
    mv "$TEST_KEY.new" "$TEST_KEY"; c2crun_close
fi
c2crun true && say "login to $PEER over c2c works"

say "building workloads on master"
make -s -C "$TESTS_DIR/workloads" OUT="$BUILD"
fix_owner "$BUILD"
say "copying workloads to $PEER:~/$PEER_DIR and building there"
c2crun "rm -rf ~/$PEER_DIR && mkdir -p ~/$PEER_DIR"
tar -C "$TESTS_DIR" -cf - workloads | c2crun "tar -C ~/$PEER_DIR -xf - && make -s -C ~/$PEER_DIR/workloads OUT=../build"
c2crun "ls ~/$PEER_BUILD" >/dev/null
# stock NVIDIA CUDA samples for T09/T10, master only
if [ -d /usr/local/cuda/samples ]; then
    say "building NVIDIA CUDA samples for T09/T10 (a few minutes)"
    source "$TESTS_DIR/lib/cuda_samples.sh"
    mkdir -p "$SAMPLES_DIR"
    cp -a /usr/local/cuda/samples/. "$SAMPLES_DIR/"
    nb=0
    for entry in $SAMPLES_LIST; do
        n="${entry%%|*}"
        d="$(find "$SAMPLES_DIR" -mindepth 2 -maxdepth 2 -type d -name "$n" | head -1)"
        [ -n "$d" ] || { warn "sample $n not found"; continue; }
        make -s -C "$d" SMS=72 -j"$(nproc)" >"$d/build.log" 2>&1 && nb=$((nb + 1)) || warn "sample $n failed to build"
    done
    fix_owner "$SAMPLES_DIR"
    say "built $nb samples"
else
    warn "CUDA samples not installed (/usr/local/cuda/samples): T09/T10 limited"
fi
say "ready. run: sudo tests/run_all.sh"
