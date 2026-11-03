#!/bin/bash
# usbc-c2c setup (interactive).
#
#   ./setup.sh                 wizard: looks at this board + the cable, asks a few
#                              things and installs. can do the peer board as well
#   sudo ./setup.sh --role master|slave --key HEX [installer options] --yes
#                              no questions (same options as setup_master.sh / setup_slave.sh)
#
# Any two AGX Xaviers with a USB-C cable between the flashing ports (J512).
# The L4T image is never touched: out-of-tree modules, some services and
# files in /opt/c2c + /etc/c2c. uninstall.sh removes all of it.
#
set -uo pipefail
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"

b()    { echo -e "\033[1m$*\033[0m"; }
say()  { echo -e "\033[1;36m[setup]\033[0m $*"; }
warn() { echo -e "\033[1;33m[setup] warning:\033[0m $*"; }
die()  { echo -e "\033[1;31m[setup] error:\033[0m $*" >&2; exit 1; }

ROLE=""; KEY=""; YES=0; PASS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --role) ROLE="$2"; shift 2 ;;
        --key) KEY="$2"; PASS+=("$1" "$2"); shift 2 ;;
        --yes|-y) YES=1; shift ;;
        -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) PASS+=("$1"); shift ;;
    esac
done

# ------------------------------------------------------------ unattended
if [ "$YES" = 1 ]; then
    [ "$(id -u)" = 0 ] || die "run as root (sudo)"
    case "$ROLE" in master|slave) ;; *) die "--role master|slave is required with --yes" ;; esac
    exec "$HERE/setup_$ROLE.sh" "${PASS[@]}"
fi

# ------------------------------------------------------------ wizard
ask() {        # ask VAR "question" default
    local q="$2" d="$3" a
    read -r -p "$(echo -e "  $q \033[2m[$d]\033[0m: ")" a
    printf -v "$1" '%s' "${a:-$d}"
}
ask_yn() {     # ask_yn VAR "question" y|n
    local a
    while :; do
        ask a "$2 (y/n)" "$3"
        case "$a" in y|Y|yes) printf -v "$1" yes; return ;; n|N|no) printf -v "$1" no; return ;; esac
    done
}

b "usbc-c2c setup"
echo "Joins two Jetson AGX Xaviers over the USB-C cable between their flashing ports:"
echo "the master can use the slave's GPU (as CUDA device 1), RAM (as swap) and CPUs."
echo

# --- this board
model="$(tr '\0' ' ' < /proc/device-tree/model 2>/dev/null)"
ids="$(tr '\0' ' ' < /proc/device-tree/chosen/ids 2>/dev/null)"
l4t="$(head -1 /etc/nv_tegra_release 2>/dev/null | sed 's/^# R\([0-9]*\) (release), REVISION: \([0-9.]*\).*/R\1.\2/')"
memg=$(( $(awk '/MemTotal/ {print $2}' /proc/meminfo) / 1048576 + 1 ))
b "This board"
echo "  $(hostname): ${model:-unknown model}, L4T ${l4t:-?}, ${memg} GB RAM, $(nproc) CPUs"
case "$ids" in *2888-*) ;; *) warn "this is not an AGX Xavier module (P2888): untested" ;; esac
[ -f /etc/nv_tegra_release ] || warn "no /etc/nv_tegra_release: is this a Jetson?"

missing=()
command -v make >/dev/null && command -v g++ >/dev/null || missing+=(build-essential)
[ -d "/lib/modules/$(uname -r)/build" ] || missing+=(nvidia-l4t-kernel-headers)
if [ ${#missing[@]} -gt 0 ]; then
    warn "missing: ${missing[*]}"
    ask_yn inst "install them now with apt" y
    [ "$inst" = yes ] && { sudo apt-get update -qq && sudo apt-get install -y "${missing[@]}" || die "apt install failed"; }
fi
if [ -f /usr/local/cuda/include/cuda.h ]; then echo "  CUDA $(/usr/local/cuda/bin/nvcc --version 2>/dev/null | sed -n 's/.*release \([0-9.]*\).*/\1/p'): remote GPU supported"
else warn "CUDA toolkit not found: link, remote RAM and CPU work; for the remote GPU install JetPack (SDK Manager or: sudo apt install nvidia-jetpack)"; fi

# --- roles come from the cable, master = USB host end
dr="$(grep -o '\[[a-z]*\]' /sys/class/typec/port0/data_role 2>/dev/null | tr -d '[]')"
peer_usb=""
lsusb 2>/dev/null | grep -qiE "1209:0001|0955:7020" && peer_usb="yes"
case "$dr" in
    host)   sug=master; why="this board is the USB host end of the cable" ;;
    device) sug=slave;  why="this board is the USB device end of the cable" ;;
    *)      sug=master; why="cable role unknown (plug the cable in to detect it)" ;;
esac
echo
b "Role"
echo "  Suggested: $sug ($why)."
echo "  The master is the node that USES the other's GPU/RAM/CPUs; it must be the USB host."
ask ROLE "role of this board (master/slave)" "$sug"
case "$ROLE" in master|slave) ;; *) die "role must be master or slave" ;; esac
if { [ "$ROLE" = master ] && [ "$dr" = device ]; } || { [ "$ROLE" = slave ] && [ "$dr" = host ]; }; then
    warn "the cable currently makes this board the USB '$dr': after installing, replug (and flip) the cable at the slave end until 'c2c doctor' shows the right roles"
fi
peer_role=slave; [ "$ROLE" = slave ] && peer_role=master

# --- settings
echo
b "Settings (Enter keeps the default)"
ARGS=()
ask NAME "node name shown to the peer" "$(hostname)"; ARGS+=(--name "$NAME")
ask_yn clocks "pin clocks at boot for the lowest latency (jetson_clocks)" y; ARGS+=(--clocks "$clocks")
ask_yn adv "change link addresses / MTU (advanced)" n
if [ "$adv" = yes ]; then
    ask mip "master link address" 10.77.0.1; ask sip "slave link address" 10.77.0.2; ask mtu "MTU" 65000
    ARGS+=(--master-ip "$mip" --slave-ip "$sip" --mtu "$mtu")
fi
if [ "$ROLE" = slave ]; then
    ask nch "raw USB data channels (programs using the remote GPU at full speed at once; 0 = TCP only)" 4
    ARGS+=(--raw-channels "$nch")
    lend=$(( memg / 2 ))
    ask lgib "RAM to lend to the master at boot, GiB (0 = off; used only on demand)" "$lend"
    ARGS+=(--rmem-lend-gib "$lgib")
else
    ask tr "remote GPU transport: auto (raw USB, else TCP) / raw / tcp" auto; ARGS+=(--gpu-transport "$tr")
    echo "  System-wide GPU: every CUDA program sees the peer's GPU as the last device, without"
    echo "  'c2c gpu run'. Programs that only use device 0 are unaffected."
    ask_yn sw "enable system-wide remote GPU" n; ARGS+=(--gpu-system-wide "$sw")
    ask_yn ra "attach the peer's lent RAM as swap at boot" n; ARGS+=(--rmem-auto "$ra")
fi

# --- the link KEY
echo
b "Link key"
if [ -r /etc/c2c/c2c.key ]; then
    echo "  this board already has a key (/etc/c2c/c2c.key)"; ask_yn keep "keep it" y
    [ "$keep" = yes ] && KEY="$(sudo cat /etc/c2c/c2c.key 2>/dev/null | tr -d ' \n')"
fi
if [ -z "$KEY" ]; then
    if [ "$ROLE" = slave ]; then
        ask KEY "the master's link KEY (shown by its setup; empty = generate one and use it on both)" ""
    fi
    [ -z "$KEY" ] && KEY="$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')" && echo "  generated a new key"
fi

# --- peer board too? (over ssh on the LAN)
echo
b "Peer board"
ask_yn dopeer "also set up the $peer_role board from here over SSH" y
PEER=""
if [ "$dopeer" = yes ]; then
    ask PEER "peer's SSH address (user@ip)" "$(id -un)@"
    [[ "$PEER" == *@?* ]] || die "expected user@ip"
    PEER_ARGS=()
    ask pname "peer's node name" "$(ssh -o ConnectTimeout=5 "$PEER" hostname 2>/dev/null || echo peer)"
    PEER_ARGS+=(--name "$pname" --clocks "$clocks")
    [ "$adv" = yes ] && PEER_ARGS+=(--master-ip "$mip" --slave-ip "$sip" --mtu "$mtu")
    if [ "$peer_role" = slave ]; then
        ask nch "peer: raw USB data channels" 4; PEER_ARGS+=(--raw-channels "$nch")
        pmem=$(ssh -o ConnectTimeout=5 "$PEER" "awk '/MemTotal/ {print int(\$2/2097152)}' /proc/meminfo" 2>/dev/null || echo 0)
        ask lgib "peer: RAM to lend at boot, GiB (0 = off)" "$pmem"; PEER_ARGS+=(--rmem-lend-gib "$lgib")
    else
        ask tr "peer: remote GPU transport (auto/raw/tcp)" auto; PEER_ARGS+=(--gpu-transport "$tr")
        ask_yn sw "peer: system-wide remote GPU" n; PEER_ARGS+=(--gpu-system-wide "$sw")
        ask_yn ra "peer: attach lent RAM as swap at boot" n; PEER_ARGS+=(--rmem-auto "$ra")
    fi
fi

# --- confirm
echo
b "Summary"
echo "  this board : $ROLE  ${ARGS[*]}"
[ -n "$PEER" ] && echo "  $PEER : $peer_role  ${PEER_ARGS[*]}"
echo "  key        : ${KEY:0:8}... (same on both)"
ask_yn go "install now" y
[ "$go" = yes ] || { echo "nothing changed"; exit 0; }

run_local()  { sudo "$HERE/setup_$ROLE.sh" --key "$KEY" "${ARGS[@]}"; }
run_peer() {
    local cm="/tmp/c2c-setup-ssh-%r@%h"
    local o=(-o ControlMaster=auto -o ControlPath="$cm" -o ControlPersist=300)
    say "copying the source to $PEER:~/usbc-c2c"
    tar -C "$HERE" --exclude=.git --exclude=tests/results --exclude=tests/build \
        --exclude='*.o' --exclude='*.ko' -cf - . | ssh "${o[@]}" "$PEER" 'rm -rf ~/usbc-c2c && mkdir -p ~/usbc-c2c && tar -C ~/usbc-c2c -xmf -' \
        || die "copy to $PEER failed"
    say "installing on $PEER as $peer_role (asks for its sudo password)"
    ssh -t "${o[@]}" "$PEER" "cd ~/usbc-c2c && sudo ./setup_$peer_role.sh --key $KEY $(printf '%q ' "${PEER_ARGS[@]}")"
}
# master goes first, it's the USB host side
if [ "$ROLE" = master ]; then run_local || die "local install failed"; [ -n "$PEER" ] && { run_peer || die "peer install failed"; }
else [ -n "$PEER" ] && { run_peer || die "peer install failed"; }; run_local || die "local install failed"; fi

echo
sleep 3
say "checking"
c2c doctor || warn "see above: 'c2c doctor' lists what to fix"
echo
say "done. Try: c2c status | c2c gpu ls | c2c gpu bench | c2c config show"
[ -z "$PEER" ] && say "on the $peer_role board run ./setup.sh with role $peer_role and this key: $KEY"
exit 0
