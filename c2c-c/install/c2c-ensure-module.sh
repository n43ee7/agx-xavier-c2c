#!/bin/bash
# c2c-ensure-module.sh master|slave
# Runs at boot: makes sure our module is built for the kernel that's actually
# running. After a kernel/L4T update the old .ko won't load, so rebuild it from
# /opt/c2c/src (needs headers). Never fails the boot, worst case C2C is just off.
role="${1:-}"
case "$role" in
    master) mod=c2c_host;  dir=host ;;
    slave)  mod=usb_f_c2c; dir=gadget ;;
    *) echo "usage: $0 master|slave"; exit 0 ;;
esac
kver="$(uname -r)"
rebuild() {   # $1 = module, $2 = dir under /opt/c2c/src/kernel
    local src=/opt/c2c/src/kernel/$2
    if [ ! -d "/lib/modules/$kver/build" ] || [ ! -d "$src" ]; then
        echo "c2c: cannot rebuild $1 (kernel headers or source missing)"; return 1
    fi
    if make -C "$src" clean all >"/tmp/c2c-$1-build.log" 2>&1; then
        install -D -m 0644 "$src/$1.ko" "/lib/modules/$kver/updates/c2c/$1.ko"
        depmod -a "$kver"; echo "c2c: $1 rebuilt for $kver"; return 0
    fi
    echo "c2c: $1 build failed, see /tmp/c2c-$1-build.log"; return 1
}
# master only: c2c_kfix (swap over the network). Only remote RAM depends on it.
if [ "$role" = master ]; then
    modinfo -k "$kver" c2c_kfix >/dev/null 2>&1 || rebuild c2c_kfix kfix
    lsmod | grep -q "^c2c_kfix " || modprobe c2c_kfix 2>/dev/null || echo "c2c: c2c_kfix not loaded - remote-memory swap disabled"
fi
if modinfo -k "$kver" "$mod" >/dev/null 2>&1; then
    [ "$role" = master ] && { lsmod | grep -q "^$mod " || modprobe "$mod"; }
    exit 0
fi
echo "c2c: $mod missing for kernel $kver, rebuilding"
src=/opt/c2c/src/kernel/$dir
if [ ! -d "/lib/modules/$kver/build" ] || [ ! -d "$src" ]; then
    echo "c2c: cannot rebuild (kernel headers or source missing) - C2C inoperable"
    exit 0
fi
if make -C "$src" clean all >/tmp/c2c-module-build.log 2>&1; then
    install -D -m 0644 "$src/$mod.ko" "/lib/modules/$kver/updates/c2c/$mod.ko"
    depmod -a "$kver"
    [ "$role" = master ] && modprobe "$mod"
    echo "c2c: $mod rebuilt for $kver"
else
    echo "c2c: module build failed, see /tmp/c2c-module-build.log - C2C inoperable"
fi
exit 0
