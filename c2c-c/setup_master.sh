#!/bin/bash
# Install or upgrade C2C on this Jetson as the MASTER (USB host end of the cable).
# Stays installed across reboots and re-handshakes on its own. uninstall.sh to remove.
#   sudo ./setup_master.sh [--key HEX]   (no KEY yet -> one gets generated)
#
. "$(cd "$(dirname "$0")" && pwd)/install/c2c-install-lib.sh"
do_install master "$@"
