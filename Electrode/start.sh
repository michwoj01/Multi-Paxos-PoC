#!/bin/bash
mount -t bpf none /sys/fs/bpf
./xdp-handler/fast ens1f1np1
exec "$@"