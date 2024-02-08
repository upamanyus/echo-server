#!/usr/bin/env python3

from os import system as do

do("make")
print("Running client")
for i in [1, 2, 4, 8, 16, 32, 64, 128, 256, 512]:
    do(f"./bin/client {i} node0")
