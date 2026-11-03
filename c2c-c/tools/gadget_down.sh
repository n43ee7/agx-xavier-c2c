#!/bin/bash
# slave: undo gadget_up.sh. unbind our gadget, hand the UDC back to the stock
# L4T one, delete our configfs tree, unload usb_f_c2c. needs sudo
set -uo pipefail
CFG=/sys/kernel/config/usb_gadget
G=$CFG/c2c
UDC="$(ls /sys/class/udc | head -n1)"
STATE=/run/c2c_prev_gadget
PREV="$(cat "$STATE" 2>/dev/null || echo l4t)"

if [ -d "$G" ]; then
    echo "" > "$G/UDC" 2>/dev/null
    rm -f "$G/configs/c.1/c2c.0"
    rmdir "$G/configs/c.1/strings/0x409" 2>/dev/null
    rmdir "$G/configs/c.1" "$G/functions/c2c.0" "$G/strings/0x409" "$G"
fi
if [ -d "$CFG/$PREV" ] && [ -z "$(cat "$CFG/$PREV/UDC")" ]; then
    echo "$UDC" > "$CFG/$PREV/UDC" && echo "UDC $UDC returned to gadget '$PREV'"
    # xudc likes to come back at high-speed after a swap, one more
    # unbind/bind usually gets SuperSpeed back
    for try in 1 2 3; do
        sleep 4
        spd="$(cat /sys/class/udc/$UDC/current_speed 2>/dev/null)"
        [ "$spd" = "high-speed" ] || break
        echo "link came up $spd, rebinding (try $try)"
        echo "" > "$CFG/$PREV/UDC"; sleep 2; echo "$UDC" > "$CFG/$PREV/UDC"
    done
    echo "link speed: $(cat /sys/class/udc/$UDC/current_speed 2>/dev/null)"
fi
rm -f "$STATE"
lsmod | grep -q '^usb_f_c2c ' && rmmod usb_f_c2c
echo "c2c gadget removed"
