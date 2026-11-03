# Common stuff for the proof tests, every T*.sh sources this.
# Tests run as root on the MASTER and only ever reach the peer over the c2c
# link (c2c-slave = 10.77.0.2), never the LAN.

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$TESTS_DIR/build"
RESULTS="${C2C_RESULTS:-$TESTS_DIR/results/manual}"
PEER="${C2C_PEER:-c2c-slave}"
PEER_USER="${C2C_PEER_USER:-${SUDO_USER:-$USER}}"
TEST_KEY=/etc/c2c/test_id_ed25519
TEST_KNOWN=/etc/c2c/test_known_hosts
PEER_DIR=".cache/c2c-tests"            # relative to the peer user's home
PEER_BUILD="$PEER_DIR/build"

# give files made under sudo back to the real user so the tree stays editable
fix_owner() { [ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER": "$@" 2>/dev/null; return 0; }

say()  { echo -e "\033[1;36m[$TEST_ID]\033[0m $*" >&2; }
warn() { echo -e "\033[1;33m[$TEST_ID] warning:\033[0m $*" >&2; }

# run a command on the peer over the link.
# Auth = the test key (setup_tests.sh --with-key) if it exists, otherwise the
# peer user's password, typed or from $C2C_PEER_PASS (never hits the disk).
# One multiplexed ssh connection gets reused for the whole run.
C2C_SSH_CTL=/run/c2c/test-ssh-ctl
_c2c_ssh_opts() {
    C2C_SSH_O=(-o IdentityAgent=none -o UserKnownHostsFile="$TEST_KNOWN" -o StrictHostKeyChecking=accept-new
               -o Ciphers=aes128-gcm@openssh.com -o Compression=no -o ConnectTimeout=5 -o LogLevel=ERROR
               -o ControlPath="$C2C_SSH_CTL")
    if [ -f "$TEST_KEY" ]; then C2C_SSH_O+=(-o IdentitiesOnly=yes -i "$TEST_KEY")
    else C2C_SSH_O+=(-o PubkeyAuthentication=no); fi
}
# open the shared connection once, stdio detached so no pipe hangs around
c2c_connect() {
    _c2c_ssh_opts
    ssh "${C2C_SSH_O[@]}" -O check "$PEER_USER@$PEER" 2>/dev/null && return 0
    mkdir -p /run/c2c
    local m=(-o ControlMaster=yes -o ControlPersist=900 -fN "$PEER_USER@$PEER")
    if [ -n "${C2C_PEER_PASS:-}" ] && [ ! -f "$TEST_KEY" ]; then
        local ap; ap="$(mktemp /run/c2c/askpass.XXXXXX)"
        printf '#!/bin/sh\nprintf "%%s\\n" "$C2C_PEER_PASS"\n' > "$ap"; chmod 700 "$ap"
        SSH_ASKPASS="$ap" SSH_ASKPASS_REQUIRE=force DISPLAY="${DISPLAY:-:0}" \
            setsid -w ssh "${C2C_SSH_O[@]}" "${m[@]}" </dev/null >/dev/null 2>&1
        rm -f "$ap"
    else
        ssh "${C2C_SSH_O[@]}" "${m[@]}" >/dev/null 2>/dev/null   # asks on the tty if it has to
    fi
    ssh "${C2C_SSH_O[@]}" -O check "$PEER_USER@$PEER" 2>/dev/null
}
c2crun() {
    c2c_connect || return 255
    ssh -T "${C2C_SSH_O[@]}" -o ControlMaster=no "$PEER_USER@$PEER" "$@"
}
c2crun_close() { ssh -o ControlPath="$C2C_SSH_CTL" -O exit "$PEER_USER@$PEER" 2>/dev/null; }

require_root() { [ "$(id -u)" = 0 ] || { echo "run as root: sudo $0" >&2; exit 2; }; }

link_json() { c2cctl json 2>/dev/null; }

require_link() {
    local st
    st="$(link_json | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["role"], d["state"])' 2>/dev/null)"
    [ "$st" = "master UP" ] || { echo "C2C link not UP on a master node (got: '${st:-no c2cd}')" >&2; exit 2; }
    c2crun true || { echo "cannot reach $PEER_USER@$PEER over c2c (set C2C_PEER_PASS or see tests/README.md)" >&2; exit 2; }
}

# peer telemetry from c2cd (comes over the link's CTRL channel, no ssh)
peer_field() { link_json | python3 -c "import json,sys; print(json.load(sys.stdin)['peer']['$1'])"; }
local_field() { link_json | python3 -c "import json,sys; print(json.load(sys.stdin)['local']['$1'])"; }

now() { date +%s.%N; }

# result <PASS|FAIL> <summary> <metrics-json>
result() {
    local status="$1" summary="$2" metrics="${3:-}"
    [ -n "$metrics" ] || metrics='{}'
    mkdir -p "$RESULTS"
    python3 - "$TEST_ID" "$TEST_NAME" "$status" "$summary" "$metrics" "$RESULTS/$TEST_ID.json" <<'EOF'
import json, sys, time
tid, name, status, summary, metrics, out = sys.argv[1:]
d = {"id": tid, "name": name, "status": status, "summary": summary,
     "time": time.strftime("%Y-%m-%d %H:%M:%S"), "metrics": json.loads(metrics)}
json.dump(d, open(out, "w"), indent=2)
EOF
    fix_owner "$TESTS_DIR/results"
    if [ "$status" = PASS ]; then
        echo -e "\033[1;32m[$TEST_ID] PASS\033[0m $summary" >&2
    else
        echo -e "\033[1;31m[$TEST_ID] FAIL\033[0m $summary" >&2
    fi
    [ "$status" = PASS ]
}

# JSON list of GPUs a program sees through c2c-gpu
gpu_list() { c2c-gpu ls 2>/dev/null; }
require_remote_gpu() {
    command -v c2c-gpu >/dev/null || { result FAIL "c2c-gpu not installed (run setup_master.sh)"; exit 1; }
    gpu_list | python3 -c 'import json,sys; sys.exit(0 if any(g["remote"] for g in json.load(sys.stdin)) else 1)' \
        || { result FAIL "no remote GPU visible (is c2c-gpud running on the slave?)"; exit 1; }
}
