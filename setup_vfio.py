#!/usr/bin/env python3

from os import system as do
import os
import pathlib

pci_id = "06:00.1"
iommu_group = pathlib.PurePath(os.readlink(f"/sys/bus/pci/devices/0000:{pci_id}/iommu_group")).name
print(f"iommu_group: {iommu_group}")
do(f"echo 0000:{pci_id} | sudo tee /sys/bus/pci/devices/0000:{pci_id}/driver/unbind")
do(f"echo 8086 10fb | sudo tee /sys/bus/pci/drivers/vfio-pci/new_id");
print("devices in iommu_group:")
do(f"ls -l /sys/bus/pci/devices/0000:{pci_id}/iommu_group/devices")
do(f"sudo chown upamanyu: /dev/vfio/{iommu_group}")
