#!/bin/bash
# c2c-gadget.sh attach|detach|status   (SLAVE node, run as root)
#
# composite: bolt the c2c function onto NVIDIA's stock "l4t" gadget so
#   RNDIS/NCM (192.168.55.1), ACM serial and mass storage keep working.
#   If there's no l4t gadget we fall back to exclusive.
# exclusive (default): our own "c2c" gadget gets the whole port.
#   Why default: composite measured ~45 MiB/s master->slave on tegra-xudc
#   (5 OUT eps active) vs ~450 MiB/s exclusive.
#
# Mode = gadget_mode= in /etc/c2c/c2c.conf (exclusive|composite).
# raw_channels=N (exclusive only, default 4, max 7) adds functions
# c2c.ch0..chN-1 = /dev/c2cchN. Plain byte pipes for the remote GPU data,
# no TCP and no daemon in between.
set -uo pipefail

CFG=/sys/kernel/config/usb_gadget
CONF=/etc/c2c/c2c.conf
NV=$CFG/l4t
OWN=$CFG/c2c
FN=c2c.0
STATE=/run/c2c/gadget.state

log() { echo "c2c-gadget: $*"; }

conf_get() {   # conf_get key default
    local v=""
    [ -r "$CONF" ] && v="$(sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\([^[:space:]#]*\).*/\1/p" "$CONF" | tail -n1)"
    echo "${v:-$2}"
}
mode="$(conf_get gadget_mode exclusive)"
nch="$(conf_get raw_channels 4)"
case "$nch" in [0-7]) ;; *) log "raw_channels=$nch invalid, using 4"; nch=4 ;; esac
want_fns() { echo "$FN"; local i; for ((i = 0; i < nch; i++)); do echo "c2c.ch$i"; done; }

udc_name() { ls /sys/class/udc 2>/dev/null | head -n1; }

rebind() {   # $1 = gadget dir. unbind + bind -> host re-enumerates us
    local g="$1" udc
    udc="$(udc_name)"
    [ -n "$udc" ] || { log "no UDC"; return 1; }
    echo "" > "$g/UDC" 2>/dev/null
    sleep 0.5
    echo "$udc" > "$g/UDC"
}

ensure_superspeed() {   # xudc sometimes comes back as high-speed after a swap, retry
    local g="$1" udc spd try
    udc="$(udc_name)"
    for try in 1 2 3; do
        sleep 3
        spd="$(cat /sys/class/udc/$udc/current_speed 2>/dev/null)"
        [ "$spd" = "high-speed" ] || break
        log "link came up $spd, rebinding (try $try)"
        rebind "$g"
    done
    log "link speed: $(cat /sys/class/udc/$udc/current_speed 2>/dev/null)"
}

attach_composite() {
    # nv-l4t-usb-device-mode.service creates it, give it a moment
    local i
    for i in $(seq 1 60); do
        [ -d "$NV/configs/c.1" ] && [ -n "$(cat $NV/UDC 2>/dev/null)" ] && break
        sleep 0.5
    done
    if [ ! -d "$NV/configs/c.1" ]; then
        log "stock l4t gadget not found, using exclusive mode"
        attach_exclusive
        return
    fi
    if [ -L "$NV/configs/c.1/$FN" ]; then
        log "already attached to l4t gadget"
        return 0
    fi
    [ -d "$NV/functions/$FN" ] || mkdir "$NV/functions/$FN" || return 1
    echo "" > "$NV/UDC"
    ln -s "$NV/functions/$FN" "$NV/configs/c.1/$FN"
    echo "$(udc_name)" > "$NV/UDC"
    echo composite > "$STATE"
    log "attached to l4t composite gadget"
    ensure_superspeed "$NV"
}

# sync the exclusive gadget's functions with want_fns (needs the UDC unbound)
sync_functions() {
    local f have want
    want="$(want_fns | sort)"
    have="$(ls -1 "$OWN/functions" 2>/dev/null | sort)"
    [ "$want" = "$have" ] && return 0
    echo "" > "$OWN/UDC" 2>/dev/null
    for f in $have; do
        echo "$want" | grep -qx "$f" && continue
        rm -f "$OWN/configs/c.1/$f"; rmdir "$OWN/functions/$f" 2>/dev/null
    done
    for f in $want; do      # link goes first so it's interface 0
        [ -d "$OWN/functions/$f" ] || mkdir "$OWN/functions/$f" || { log "cannot create function $f"; return 1; }
        [ -L "$OWN/configs/c.1/$f" ] || ln -s "$OWN/functions/$f" "$OWN/configs/c.1/$f"
    done
    log "functions: $(echo $want)"
}

attach_exclusive() {
    if [ ! -d "$OWN" ]; then
        mkdir "$OWN"
        echo 0x1209 > "$OWN/idVendor"
        echo 0x0001 > "$OWN/idProduct"
        echo 0x0320 > "$OWN/bcdUSB"
        echo 0x0100 > "$OWN/bcdDevice"
        mkdir "$OWN/strings/0x409"
        echo "C2C"            > "$OWN/strings/0x409/manufacturer"
        echo "C2C link"       > "$OWN/strings/0x409/product"
        hostname              > "$OWN/strings/0x409/serialnumber"
        mkdir "$OWN/configs/c.1"
        echo 100 > "$OWN/configs/c.1/MaxPower"
    fi
    sync_functions || return 1
    # steal the UDC from whoever has it
    local g udc
    udc="$(udc_name)"
    for g in "$CFG"/*; do
        [ "$g" = "$OWN" ] && continue
        [ "$(cat "$g/UDC" 2>/dev/null)" = "$udc" ] && echo "" > "$g/UDC"
    done
    [ "$(cat "$OWN/UDC")" = "$udc" ] || echo "$udc" > "$OWN/UDC"
    echo exclusive > "$STATE"
    log "exclusive c2c gadget bound to $udc"
    ensure_superspeed "$OWN"
}

detach() {
    local udc
    udc="$(udc_name)"
    if [ -L "$NV/configs/c.1/$FN" ] || [ -d "$NV/functions/$FN" ]; then
        echo "" > "$NV/UDC" 2>/dev/null
        rm -f "$NV/configs/c.1/$FN"
        rmdir "$NV/functions/$FN" 2>/dev/null
        echo "$udc" > "$NV/UDC" 2>/dev/null
        log "detached from l4t gadget"
        ensure_superspeed "$NV"
    fi
    if [ -d "$OWN" ]; then
        local f
        echo "" > "$OWN/UDC" 2>/dev/null
        for f in $(ls -1 "$OWN/functions" 2>/dev/null); do
            rm -f "$OWN/configs/c.1/$f"; rmdir "$OWN/functions/$f" 2>/dev/null
        done
        rmdir "$OWN/configs/c.1/strings/0x409" 2>/dev/null
        rmdir "$OWN/configs/c.1" "$OWN/strings/0x409" "$OWN" 2>/dev/null
        if [ -d "$NV" ] && [ -z "$(cat "$NV/UDC")" ]; then
            echo "$udc" > "$NV/UDC" && ensure_superspeed "$NV"
        fi
        log "exclusive gadget removed"
    fi
    rm -f "$STATE"
}

mkdir -p /run/c2c
case "${1:-}" in
    attach)
        lsmod | grep -q '^usb_f_c2c ' || modprobe usb_f_c2c 2>/dev/null || { log "usb_f_c2c module not available"; exit 1; }
        if [ "$mode" = "exclusive" ]; then attach_exclusive; else attach_composite; fi ;;
    detach)
        detach
        lsmod | grep -q '^usb_f_c2c ' && rmmod usb_f_c2c 2>/dev/null
        exit 0 ;;
    status)
        echo "mode: $(cat $STATE 2>/dev/null || echo detached)"
        echo "functions: $(ls -1 "$OWN/functions" 2>/dev/null | tr '\n' ' ')"
        echo "udc speed: $(cat /sys/class/udc/$(udc_name)/current_speed 2>/dev/null)" ;;
    *)  echo "usage: $0 attach|detach|status"; exit 2 ;;
esac
