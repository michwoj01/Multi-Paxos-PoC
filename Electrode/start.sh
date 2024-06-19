#!/bin/bash
mkdir -p /sys/fs/bpf
mount -t bpf bpf /sys/fs/bpf
./xdp-handler/fast eth0 &
exec "$@"