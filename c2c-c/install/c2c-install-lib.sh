#!/bin/bash
# Installer guts shared by setup_master.sh / setup_slave.sh. Don't run it directly.
#
# What it puts on the box (uninstall.sh takes all of it back out):
#   /opt/c2c/src                  copy of this tree (so modules can be rebuilt later)
#   /opt/c2c/bin                  c2c (the CLI), c2cd, c2cctl, c2c-memd, c2c-rmem, c2c-svc,
#                                 c2c-gadget.sh, c2c-ensure-module.sh, GPU tools
#   /usr/local/bin/c2c, c2cctl, c2c-rmem, c2c-memd, c2c-gpu   symlinks
#   /etc/c2c/c2c.conf, c2c.key    config + the shared link KEY (0600)
#   /lib/modules/<kver>/updates/c2c/*.ko
#   /etc/modules-load.d/c2c.conf  (master: c2c_kfix + c2c_host)
#   /etc/systemd/system/c2cd.service, c2c-clocks.service, c2c-gadget.service +
#       c2c-gpud.service + c2c-memd.service (slave), c2c-rmem.service (master)
#   /etc/ld.so.conf.d/00-c2c-gpu.conf   master, only if gpu_system_wide=yes
#   /etc/NetworkManager/conf.d/99-c2c.conf   so NM leaves c2c0 alone
#   /etc/hosts                    c2c-master / c2c-slave entries

set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX=/opt/c2c
ETC=/etc/c2c
UNIT_DIR=/etc/systemd/system
HOSTS_BEGIN="# >>> c2c link (managed by C2C, removed by uninstall.sh) >>>"
HOSTS_END="# <<< c2c link <<<"

say()  { echo -e "\033[1;36m[c2c]\033[0m $*"; }
warn() { echo -e "\033[1;33m[c2c] warning:\033[0m $*"; }
die()  { echo -e "\033[1;31m[c2c] error:\033[0m $*" >&2; exit 1; }

usage() {
    cat <<EOF
usage: sudo $0 [options]
  --key HEX          shared link KEY (64 hex chars); must match on both nodes
  --key-file FILE    read the KEY from FILE
  --name NAME        node name shown to the peer (default: hostname)
  --master-ip IP     link address of the master (default 10.77.0.1)
  --slave-ip IP      link address of the slave  (default 10.77.0.2)
  --mtu N            link MTU (default 65000)
  --gadget-mode M    slave only: exclusive (default, full speed) | composite
  --raw-channels N   slave only: raw USB data channels, 0..7 (default 4)
  --gpu-transport T  auto (default) | raw | tcp
  --gpu-system-wide yes|no   master: every CUDA program sees the peer's GPU
  --rmem-lend-gib N  slave: RAM lent to the master at boot (default 0 = off)
  --rmem-auto yes|no master: attach the peer's lent RAM as swap at boot
  --clocks yes|no    pin clocks at boot for low latency (default yes)
Settings already in /etc/c2c/c2c.conf are kept unless given here.
EOF
}

old_conf() {   # value from the existing config if there is one, else $2
    local v=""
    [ -r "$ETC/c2c.conf" ] && v="$(sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\([^#[:space:]]*\).*/\1/p" "$ETC/c2c.conf" | tail -n1)"
    echo "${v:-$2}"
}

parse_args() {
    KEY=""; KEY_FILE=""
    NAME="$(old_conf name '')"; MASTER_IP="$(old_conf master_ip 10.77.0.1)"; SLAVE_IP="$(old_conf slave_ip 10.77.0.2)"
    MTU="$(old_conf mtu 65000)"; GADGET_MODE="$(old_conf gadget_mode exclusive)"
    RAW_CHANNELS="$(old_conf raw_channels 4)"; GPU_TRANSPORT="$(old_conf gpu_transport auto)"
    GPU_SYSTEM_WIDE="$(old_conf gpu_system_wide no)"; RMEM_LEND_GIB="$(old_conf rmem_lend_gib 0)"
    RMEM_AUTO="$(old_conf rmem_auto no)"; CLOCKS="$(old_conf clocks_at_boot yes)"
    while [ $# -gt 0 ]; do
        case "$1" in
            --key) KEY="$2"; shift 2 ;;
            --key-file) KEY_FILE="$2"; shift 2 ;;
            --name) NAME="$2"; shift 2 ;;
            --master-ip) MASTER_IP="$2"; shift 2 ;;
            --slave-ip) SLAVE_IP="$2"; shift 2 ;;
            --mtu) MTU="$2"; shift 2 ;;
            --gadget-mode) GADGET_MODE="$2"; shift 2 ;;
            --raw-channels) RAW_CHANNELS="$2"; shift 2 ;;
            --gpu-transport) GPU_TRANSPORT="$2"; shift 2 ;;
            --gpu-system-wide) GPU_SYSTEM_WIDE="$2"; shift 2 ;;
            --rmem-lend-gib) RMEM_LEND_GIB="$2"; shift 2 ;;
            --rmem-auto) RMEM_AUTO="$2"; shift 2 ;;
            --clocks) CLOCKS="$2"; shift 2 ;;
            -h|--help) usage; exit 0 ;;
            *) usage; die "unknown option $1" ;;
        esac
    done
    [ -n "$KEY_FILE" ] && KEY="$(tr -d ' \n\r\t' < "$KEY_FILE")"
    case "$GADGET_MODE" in exclusive|composite) ;; *) die "--gadget-mode exclusive|composite" ;; esac
    [[ "$RAW_CHANNELS" =~ ^[0-7]$ ]] || die "--raw-channels 0..7"
    case "$GPU_TRANSPORT" in auto|raw|tcp) ;; *) die "--gpu-transport auto|raw|tcp" ;; esac
    [[ "$RMEM_LEND_GIB" =~ ^[0-9]+$ ]] || die "--rmem-lend-gib N"
    for v in "$GPU_SYSTEM_WIDE" "$RMEM_AUTO" "$CLOCKS"; do case "$v" in yes|no) ;; *) die "yes|no expected, got '$v'" ;; esac; done
}

preflight() {
    [ "$(id -u)" = 0 ] || die "run as root (sudo)"
    [ -f /etc/nv_tegra_release ] || warn "this does not look like a Jetson (no /etc/nv_tegra_release)"
    command -v make >/dev/null || die "make not installed (apt install build-essential)"
    command -v g++ >/dev/null  || die "g++ not installed (apt install build-essential)"
    [ -d "/lib/modules/$(uname -r)/build" ] || die "kernel headers for $(uname -r) missing (nvidia-l4t-kernel-headers)"
}

resolve_key() {   # $1 = role
    local existing=""
    [ -r "$ETC/c2c.key" ] && existing="$(tr -d ' \n\r\t' < "$ETC/c2c.key")"
    if [ -z "$KEY" ]; then
        if [ -n "$existing" ]; then
            KEY="$existing"; say "keeping existing link KEY from $ETC/c2c.key"
        elif [ "$1" = master ]; then
            KEY="$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')"
            say "generated a new link KEY"
        else
            die "the slave needs the master's KEY: --key <hex> or --key-file <file>"
        fi
    fi
    [[ "$KEY" =~ ^[0-9a-fA-F]{32,128}$ ]] && [ $(( ${#KEY} % 2 )) = 0 ] \
        || die "KEY must be 32..128 hex characters"
}

stop_existing() {
    for u in c2c-rmem c2c-memd c2c-gpud c2cd c2c-gadget; do
        if systemctl list-unit-files "$u.service" >/dev/null 2>&1 &&
           systemctl is-active --quiet "$u"; then
            say "stopping running $u (upgrade)"
            systemctl stop "$u" || true
        fi
    done
}

build_and_install() {   # $1 = role
    local role="$1" kdir mod kver
    kver="$(uname -r)"
    if [ "$role" = master ]; then kdir=host; mod=c2c_host; else kdir=gadget; mod=usb_f_c2c; fi

    say "copying sources to $PREFIX/src"
    rm -rf "$PREFIX/src"
    mkdir -p "$PREFIX/src" "$PREFIX/bin"
    tar -C "$SRC_DIR" --exclude='*.o' --exclude='*.ko' --exclude='*.cmd' \
        --exclude='*.mod*' --exclude=Module.symvers --exclude=modules.order \
        --exclude='.git' -cf - . | tar -C "$PREFIX/src" -xf -
    chown -R root:root "$PREFIX/src"

    say "building kernel module $mod for $kver"
    make -C "$PREFIX/src/kernel/$kdir" clean all >"$PREFIX/build.log" 2>&1 \
        || { tail -20 "$PREFIX/build.log"; die "module build failed ($PREFIX/build.log)"; }
    say "building c2cd / c2cctl"
    make -C "$PREFIX/src/daemon" clean all >>"$PREFIX/build.log" 2>&1 \
        || { tail -20 "$PREFIX/build.log"; die "daemon build failed ($PREFIX/build.log)"; }
    say "building remote memory tools (c2c-memd / c2c-rmem)"
    make -C "$PREFIX/src/rmem" clean all >>"$PREFIX/build.log" 2>&1 \
        || { tail -20 "$PREFIX/build.log"; die "rmem build failed ($PREFIX/build.log)"; }
    # remote GPU needs the CUDA toolkit. optional, the link works fine without it
    local gpu_ok=0
    if [ -x /usr/local/cuda/bin/nvcc ] || [ -f /usr/local/cuda/include/cuda.h ]; then
        say "building remote GPU support (libc2ccuda / c2c-gpud)"
        if make -C "$PREFIX/src/gpu" clean all >>"$PREFIX/build.log" 2>&1; then gpu_ok=1
        else warn "remote GPU build failed (see $PREFIX/build.log): remote GPU disabled"; fi
    else
        warn "CUDA toolkit not found: remote GPU disabled"
    fi
    if [ "$role" = master ]; then
        make -C "$PREFIX/src/tools/test_bw_spd" >>"$PREFIX/build.log" 2>&1 \
            && install -m 0755 "$PREFIX/src/tools/test_bw_spd/test_bw_spd" "$PREFIX/bin/" \
            || warn "test_bw_spd build failed (optional)"
    fi

    install -D -m 0644 "$PREFIX/src/kernel/$kdir/$mod.ko" "/lib/modules/$kver/updates/c2c/$mod.ko"
    if [ "$role" = master ]; then
        # c2c_kfix: without it swapping over c2c can panic the kernel
        say "building kernel fix module c2c_kfix"
        make -C "$PREFIX/src/kernel/kfix" clean all >>"$PREFIX/build.log" 2>&1 \
            || { tail -20 "$PREFIX/build.log"; die "c2c_kfix build failed ($PREFIX/build.log)"; }
        install -D -m 0644 "$PREFIX/src/kernel/kfix/c2c_kfix.ko" "/lib/modules/$kver/updates/c2c/c2c_kfix.ko"
    fi
    depmod -a "$kver"
    install -m 0755 "$PREFIX/src/daemon/c2cd" "$PREFIX/src/daemon/c2cctl" "$PREFIX/bin/"
    install -m 0755 "$PREFIX/src/rmem/c2c-memd" "$PREFIX/src/rmem/c2c-rmem" "$PREFIX/bin/"
    ln -sf "$PREFIX/bin/c2c-rmem" /usr/local/bin/c2c-rmem
    ln -sf "$PREFIX/bin/c2c-memd" /usr/local/bin/c2c-memd
    if [ "$gpu_ok" = 1 ]; then
        if [ "$role" = master ]; then
            # our libcuda.so.1 front; only programs started via c2c-gpu load it
            install -D -m 0755 "$PREFIX/src/gpu/libc2ccuda.so" "$PREFIX/lib/cuda/libcuda.so.1"
            install -m 0755 "$PREFIX/src/gpu/c2c-gpu" "$PREFIX/src/gpu/c2c-gpu-ls" \
                "$PREFIX/src/gpu/c2c-gpu-bench" "$PREFIX/bin/"
            ln -sf "$PREFIX/bin/c2c-gpu" /usr/local/bin/c2c-gpu
        else
            install -m 0755 "$PREFIX/src/gpu/c2c-gpud" "$PREFIX/bin/"
        fi
    fi
    install -m 0755 "$PREFIX/src/install/c2c-ensure-module.sh" "$PREFIX/src/install/c2c" \
        "$PREFIX/src/install/c2c-svc" "$PREFIX/bin/"
    ln -sf "$PREFIX/bin/c2c" /usr/local/bin/c2c
    [ "$role" = slave ] && install -m 0755 "$PREFIX/src/install/c2c-gadget.sh" "$PREFIX/bin/"
    ln -sf "$PREFIX/bin/c2cctl" /usr/local/bin/c2cctl
}

write_config() {   # $1 = role
    mkdir -p "$ETC"
    chmod 0755 "$ETC"
    umask 077
    printf '%s\n' "$KEY" > "$ETC/c2c.key"
    chmod 0600 "$ETC/c2c.key"
    umask 022
    cat > "$ETC/c2c.conf" <<EOF
# C2C link configuration (written by setup_$1.sh)
role=$1
key_file=$ETC/c2c.key
device=/dev/c2c0
ifname=c2c0
master_ip=$MASTER_IP
slave_ip=$SLAVE_IP
prefix=30
mtu=$MTU
${NAME:+name=$NAME}
# heartbeat: link declared lost after dead_ms without traffic
ping_ms=500
dead_ms=3000
# after traffic, poll without sleeping this long (lower round-trip latency)
busy_poll_us=$(old_conf busy_poll_us 200)
hello_ms=1000
reject_backoff_ms=5000
txq_bytes=4194304
gadget_mode=$GADGET_MODE
# raw USB data channels (/dev/c2cchN) next to the link: the remote GPU's
# data path without TCP/IP or c2cd (see: c2c config show)
raw_channels=$RAW_CHANNELS
gpu_transport=$GPU_TRANSPORT
gpu_spin_us=$(old_conf gpu_spin_us 1000)
gpud_spin_us=$(old_conf gpud_spin_us 1000)
gpud_download_buffer_mib=$(old_conf gpud_download_buffer_mib 512)
gpud_keep_power_gating=$(old_conf gpud_keep_power_gating no)
gpu_system_wide=$GPU_SYSTEM_WIDE
rmem_lend_gib=$RMEM_LEND_GIB
rmem_auto=$RMEM_AUTO
rmem_page_cluster=$(old_conf rmem_page_cluster 7)
clocks_at_boot=$CLOCKS
$( [ -n "$(old_conf cpu_offload_user '')" ] && echo "cpu_offload_user=$(old_conf cpu_offload_user '')" )
EOF
    chmod 0644 "$ETC/c2c.conf"
}

system_glue() {   # $1 = role
    # names for the link ends in /etc/hosts
    if grep -qF "$HOSTS_BEGIN" /etc/hosts; then
        sed -i "\|$HOSTS_BEGIN|,\|$HOSTS_END|d" /etc/hosts
    fi
    printf '%s\n%s c2c-master\n%s c2c-slave\n%s\n' "$HOSTS_BEGIN" "$MASTER_IP" \
        "$SLAVE_IP" "$HOSTS_END" >> /etc/hosts

    # keep NetworkManager's hands off c2c0
    if [ -d /etc/NetworkManager/conf.d ]; then
        printf '[keyfile]\nunmanaged-devices=interface-name:c2c0\n' \
            > /etc/NetworkManager/conf.d/99-c2c.conf
        systemctl reload NetworkManager 2>/dev/null || true
    fi
    if [ "$1" = master ]; then
        printf 'c2c_kfix\nc2c_host\n' > /etc/modules-load.d/c2c.conf
        modprobe c2c_kfix || warn "c2c_kfix did not load: remote-memory swap stays disabled"
    fi
    return 0
}

install_units() {   # $1 = role
    local after wants
    if [ "$1" = slave ]; then
        after="c2c-gadget.service"; wants="c2c-gadget.service"
        install -m 0644 "$PREFIX/src/install/systemd/c2c-gadget.service" "$UNIT_DIR/"
    else
        after=""; wants=""
    fi
    install -m 0644 "$PREFIX/src/install/systemd/c2c-clocks.service" "$UNIT_DIR/"
    sed -e "s|@ROLE@|$1|g" -e "s|@AFTER@|$after|" -e "s|@WANTS@|$wants|" \
        "$PREFIX/src/install/systemd/c2cd.service" > "$UNIT_DIR/c2cd.service"
    [ -z "$wants" ] && sed -i '/^Wants=$/d' "$UNIT_DIR/c2cd.service"
    systemctl daemon-reload
    if [ "$CLOCKS" = yes ]; then
        systemctl enable c2c-clocks.service >/dev/null 2>&1; systemctl start c2c-clocks.service || true
    else
        systemctl disable c2c-clocks.service >/dev/null 2>&1 || true
    fi
    if [ "$1" = slave ]; then
        systemctl enable c2c-gadget.service >/dev/null 2>&1
        systemctl start c2c-gadget.service
    fi
    systemctl enable c2cd.service >/dev/null 2>&1
    systemctl restart c2cd.service
    if [ "$1" = slave ] && [ -x "$PREFIX/bin/c2c-gpud" ]; then
        sed -e "s|@SLAVE_IP@|$SLAVE_IP|g" -e "s|@MASTER_IP@|$MASTER_IP|g" \
            "$PREFIX/src/install/systemd/c2c-gpud.service" > "$UNIT_DIR/c2c-gpud.service"
        systemctl daemon-reload
        systemctl enable c2c-gpud.service >/dev/null 2>&1
        systemctl restart c2c-gpud.service
    fi
    if [ "$1" = slave ]; then
        install -m 0644 "$PREFIX/src/install/systemd/c2c-memd.service" "$UNIT_DIR/"
        systemctl daemon-reload
        if [ "$RMEM_LEND_GIB" -gt 0 ]; then
            systemctl enable c2c-memd.service >/dev/null 2>&1; systemctl restart c2c-memd.service
        else
            systemctl disable c2c-memd.service >/dev/null 2>&1 || true
        fi
    else
        install -m 0644 "$PREFIX/src/install/systemd/c2c-rmem.service" "$UNIT_DIR/"
        systemctl daemon-reload
        if [ "$RMEM_AUTO" = yes ]; then
            systemctl enable c2c-rmem.service >/dev/null 2>&1   # attaches on next boot/start
        else
            systemctl disable c2c-rmem.service >/dev/null 2>&1 || true
        fi
        # system-wide GPU: put our libcuda ahead of NVIDIA's in the ld.so search order
        if [ "$GPU_SYSTEM_WIDE" = yes ] && [ -e "$PREFIX/lib/cuda/libcuda.so.1" ]; then
            echo "$PREFIX/lib/cuda" > /etc/ld.so.conf.d/00-c2c-gpu.conf
        else
            rm -f /etc/ld.so.conf.d/00-c2c-gpu.conf
        fi
        ldconfig
    fi
}

do_install() {   # $1 = role, rest = args
    local role="$1"; shift
    parse_args "$@"
    preflight
    resolve_key "$role"
    stop_existing
    build_and_install "$role"
    write_config "$role"
    system_glue "$role"
    install_units "$role"
    sleep 3
    say "installed as $role. Status:"
    /usr/local/bin/c2cctl status | head -8 || true
    echo
    if [ "$role" = master ]; then
        say "link KEY (give it to the slave):"
        echo "    $KEY"
        say "on the slave run:  sudo ./setup_slave.sh --key $KEY"
    fi
    say "check anytime with: c2cctl status   (logs: journalctl -u c2cd)"
}
