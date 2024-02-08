#!/usr/bin/env python3

import os
from os import system as do

do("sudo apt update")
do("sudo apt upgrade")
do("sudo apt install cpufrequtils liburing-dev")
os.chdir("/tmp")
do("wget https://go.dev/dl/go1.21.0.linux-amd64.tar.gz -O go.tar.gz")
do("sudo tar -C /usr/local -xzvf go.tar.gz")


do("""
sudo sed -i s/GRUB_CMDLINE_LINUX_DEFAULT=.*/GRUB_CMDLINE_LINUX_DEFAULT="\\"intel_pstate=passive intel_idle.max_cstate=0 systemd.unified_cgroup_hierarchy=1\\""/g  /etc/default/grub
""")
do("sudo update-grub")
do("echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo")

# do("sudo reboot")
