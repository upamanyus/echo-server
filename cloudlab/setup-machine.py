#!/usr/bin/env python3

import os
from os import system as do

do("echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo")
do("echo off | sudo tee /sys/devices/system/cpu/smt/control")
for i in range(os.cpu_count()):
    do(f"cpufreq-set -g performance {i}")

# disable second CPU
for i in [1,3,5,7,9,11,13,15]:
    do(f"echo 0 | sudo tee /sys/devices/system/cpu/cpu{i}/online")
