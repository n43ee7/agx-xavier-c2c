#!/bin/bash
# Install or upgrade C2C on this Jetson as the SLAVE (USB device end of the cable).
# Stays installed across reboots and re-handshakes on its own. uninstall.sh to remove.
#   sudo ./setup_slave.sh --key HEX     (KEY = the one the master printed)
#
. "$(cd "$(dirname "$0")" && pwd)/install/c2c-install-lib.sh"
do_install slave "$@"
