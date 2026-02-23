#!/bin/bash

export XDG_RUNTIME_DIR=/tmp/xdg-runtime/
mkdir -p $XDG_RUNTIME_DIR
chmod -R o-w $XDG_RUNTIME_DIR
export $(dbus-launch)

#rename the interface for

ip link set eth0 down
ip link set eth0 name enp1s0
ip link set enp1s0 up

pipewire  > /tmp/pipewire.log 2>&1 &
pipewire-avb > /tmp/pipewire-avb.log 2>&1 &
bash
