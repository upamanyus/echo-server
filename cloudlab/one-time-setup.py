#!/usr/bin/env python3

import os
from os import system as do

# Set up SSH keys to ssh between nodes
do("/usr/bin/geni-get key > ~/.ssh/id_rsa")
do("chmod 600 ~/.ssh/id_rsa")
do("ssh-keygen -y -f ~/.ssh/id_rsa > ~/.ssh/id_rsa.pub")
do("cat ~/.ssh/id_rsa.pub >> ~/.ssh/authorized_keys")
do("chmod 644 ~/.ssh/authorized_keys")

do("yes | sudo apt update")
do("yes | sudo apt upgrade")
do("yes | sudo apt install redis pip cpufrequtils liburing-dev numactl")
os.chdir("/tmp")
do("wget https://go.dev/dl/go1.22.0.linux-amd64.tar.gz -O go.tar.gz")
do("sudo tar -C /usr/local -xzvf go.tar.gz")

do("""
sudo sed -i s/GRUB_CMDLINE_LINUX_DEFAULT=.*/GRUB_CMDLINE_LINUX_DEFAULT="\\"intel_pstate=passive intel_idle.max_cstate=0 systemd.unified_cgroup_hierarchy=1 intel_iommu=on\\""/g  /etc/default/grub
""")
do("sudo update-grub")
do("sudo chsh -s /usr/bin/zsh upamanyu")

do("sudo reboot")
