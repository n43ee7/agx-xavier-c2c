#!/bin/bash
# slave (USB device end): load usb_f_c2c and steal the UDC from the stock L4T
# gadget (RNDIS/NCM/ACM/storage) for ours. gadget_down.sh puts it all back.
# needs sudo. extra args go straight to insmod, e.g. buflen=131072 qlen=32
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
CFG=/sys/kernel/config/usb_gadget
G=$CFG/c2c
UDC="$(ls /sys/class/udc | head -n1)"
STATE=/run/c2c_prev_gadget

lsmod | grep -q '^usb_f_c2c ' || insmod "$DIR/../kernel/gadget/usb_f_c2c.ko" "$@"

if [ ! -d "$G" ]; then
    mkdir "$G"
    echo 0x1209 > "$G/idVendor"
    echo 0x0001 > "$G/idProduct"
    echo 0x0320 > "$G/bcdUSB"
    echo 0x0100 > "$G/bcdDevice"
    mkdir "$G/strings/0x409"
    echo "C2C-thunderbolt"  > "$G/strings/0x409/manufacturer"
    echo "C2C bulk link"    > "$G/strings/0x409/product"
    hostname                > "$G/strings/0x409/serialnumber"
    mkdir "$G/configs/c.1"
    echo 100 > "$G/configs/c.1/MaxPower"
    mkdir "$G/functions/c2c.0"
    ln -s "$G/functions/c2c.0" "$G/configs/c.1/"
fi

# remember who has the UDC right now and let go of it
for g in "$CFG"/*; do
    [ "$g" = "$G" ] && continue
    if [ "$(cat "$g/UDC" 2>/dev/null)" = "$UDC" ]; then
        basename "$g" > "$STATE"
        echo "" > "$g/UDC"
        echo "released $UDC from gadget '$(basename "$g")'"
    fi
done
[ -f "$STATE" ] || echo l4t > "$STATE"

[ "$(cat "$G/UDC")" = "$UDC" ] || echo "$UDC" > "$G/UDC"
echo "c2c gadget bound to $UDC"
