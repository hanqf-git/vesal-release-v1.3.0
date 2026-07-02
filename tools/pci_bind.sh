#!/bin/bash

# Copyright (c) 2025 ByteDance Inc.
#
# This file is part of veSAL.
#
# veSAL is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# veSAL is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with veSAL. If not, see <https://www.gnu.org/licenses/>.

# bind all DSA devices to uio_pci_generic
# usage:
# 	bash pci_bind.sh

assign_dev() {
	device=`cat /sys/bus/pci/devices/$dev/device`
	vendor=`cat /sys/bus/pci/devices/$dev/vendor`
	echo Working on device: $device, vendor: $vendor

	if [ -d /sys/bus/pci/devices/$dev/driver/ ]; then
		if echo "$dev" > /sys/bus/pci/devices/$dev/driver/unbind 2>/dev/null; then
			echo "Unbind successful for $dev"
		else
			echo "Unbind failed for $dev"
		fi
	else
		echo "No driver bound to $dev, skipping unbind"
	fi

	output=$( { echo "$vendor $device" > /sys/bus/pci/drivers/$driver/new_id; } 2>&1 )
	if [[ "$output" =~ "File exists" ]]; then
		echo "Device ID already exists. Skipping."
	elif [[ -n "$output" ]]; then
		echo "new_id write error: $output"
	else
		echo "new_id write succeeded"
	fi

	if echo "$dev" > /sys/bus/pci/drivers/$driver/bind 2>/dev/null; then
		echo "Bind successful for $dev to driver $driver"
	else
		echo "Bind failed for $dev to driver $driver"
	fi

	echo $dev bound to $(basename $(readlink /sys/bus/pci/devices/$dev/driver))
}

driver="uio_pci_generic"
if modinfo "$driver" &>/dev/null; then
  modprobe "$driver"
else
  echo "Driver $driver not found"
  exit -1
fi

# grep all DSA device info which contains "0b25"
result=$(lspci | grep 0b25)

if [[ -n "$result" ]]; then
    echo "Found DSA devices:"
    echo "$result"
else
    echo "No DSA device found"
fi

# get PCI device address
addrs=($(echo "$result" | awk '{print $1}'))

for addr in "${addrs[@]}"; do
	dev=$(echo 0000:$addr)
	assign_dev
done