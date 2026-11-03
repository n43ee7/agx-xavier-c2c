#!/bin/bash
# master: unload c2c_host.ko (sudo)
lsmod | grep -q '^c2c_host ' && rmmod c2c_host
echo "c2c_host unloaded"
