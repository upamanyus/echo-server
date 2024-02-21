#!/usr/bin/env python3
from os import system as do
from subprocess import Popen

procs = []
nmachines = 2
for i in range(0, nmachines):
    # this saves the key in known_hosts without asking for the user to
    # interactively type "yes"
    do(f"ssh -o StrictHostKeyChecking=no node{i} 'echo starting node{i}'")

    do(f"scp one-time-setup.py node{i}:")
    do(f"scp .zshrc-cloudlab node{i}:.zshrc")
    c = f"ssh node{i} 'chmod +x one-time-setup.py && ./one-time-setup.py'"
    procs.append(Popen(c, shell=True))

for p in procs:
   p.wait()

# connect between cloudlab nodes so user is not prompted for "yes" later.
for j in range(0,nmachines):
    for i in range(0,nmachines):
        do(f"ssh node{j} \"ssh -o StrictHostKeyChecking=no node{i} 'echo connecting node{j} to node{i}'\"")
