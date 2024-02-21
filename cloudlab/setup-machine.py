#!/usr/bin/env python3

import os
from os import system as do

def no_hyper():
    do("echo off | sudo tee /sys/devices/system/cpu/smt/control")

def fix_freq():
    # do("echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo")
    for i in range(os.cpu_count()):
        do(f"cpufreq-set -g performance {i}")

# Returns a list of lists, where the first list is the CPUs in numa node 1,
# second list is numa node 2, etc.
def get_cpus():
    c = []
    o = dict()

    # read "/proc/cpuinfo"
    with open("/proc/cpuinfo", "r") as f:
        for l in f.readlines():
            y = l.find(":")
            if y < 0:
                # new object
                c = c + [o]
                o = dict()
            else:
                k = l[0:y].strip()
                v = l[y+1:].strip()
                o[k] = v
    return c

print(get_cpus())
