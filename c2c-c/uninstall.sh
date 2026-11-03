#!/bin/bash
# Takes C2C off this board completely (either role). On a slave the USB port
# goes back to NVIDIA's stock l4t gadget. setup_*.sh puts it back any time.
#   sudo ./uninstall.sh [--keep-key]
set -uo pipefail
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)"; exit 1; }
KEEP_KEY=0; [ "${1:-}" = "--keep-key" ] && KEEP_KEY=1
say() { echo -e "\033[1;36m[c2c]\033[0m $*"; }

# remote RAM first - swapped-out pages have to come home while the link still works
for d in /dev/nbd*; do
    [ -b "$d" ] || continue
    if [ -x /opt/c2c/bin/c2c-rmem ] && /opt/c2c/bin/c2c-rmem status -d "$d" >/dev/null 2>&1; then
        say "detaching remote memory $d"
        /opt/c2c/bin/c2c-rmem detach -d "$d" || { echo "swapoff $d failed; free memory and retry"; exit 1; }
    fi
done
pkill -x c2c-memd 2>/dev/null

for u in c2c-rmem c2c-memd c2c-gpud c2cd c2c-gadget c2c-clocks; do
    if [ -f "/etc/systemd/system/$u.service" ]; then
        say "stopping and disabling $u"
        systemctl disable --now "$u.service" >/dev/null 2>&1 || true
    fi
done
# unit gone but gadget still attached? clean that up too
[ -x /opt/c2c/bin/c2c-gadget.sh ] && /opt/c2c/bin/c2c-gadget.sh detach >/dev/null 2>&1
for m in c2c_host usb_f_c2c c2c_kfix; do lsmod | grep -q "^$m " && rmmod "$m" 2>/dev/null; done

rm -f /etc/systemd/system/c2cd.service /etc/systemd/system/c2c-gadget.service /etc/systemd/system/c2c-gpud.service \
      /etc/systemd/system/c2c-memd.service /etc/systemd/system/c2c-rmem.service /etc/systemd/system/c2c-clocks.service
if [ -f /etc/ld.so.conf.d/00-c2c-gpu.conf ]; then rm -f /etc/ld.so.conf.d/00-c2c-gpu.conf; ldconfig; fi
systemctl daemon-reload
rm -rf /lib/modules/*/updates/c2c
depmod -a
rm -f /etc/modules-load.d/c2c.conf
if [ -f /etc/NetworkManager/conf.d/99-c2c.conf ]; then
    rm -f /etc/NetworkManager/conf.d/99-c2c.conf
    systemctl reload NetworkManager 2>/dev/null || true
fi
sed -i '\|# >>> c2c link|,\|# <<< c2c link <<<|d' /etc/hosts
rm -f /usr/local/bin/c2c /usr/local/bin/c2cctl /usr/local/bin/c2c-rmem /usr/local/bin/c2c-memd /usr/local/bin/c2c-gpu
rm -rf /var/cache/c2c-gpud
rm -rf /opt/c2c /run/c2c
if [ $KEEP_KEY = 1 ]; then
    rm -f /etc/c2c/c2c.conf; say "kept /etc/c2c/c2c.key"
else
    rm -rf /etc/c2c
fi
say "C2C removed"
