FROM ubuntu:20.04

ENV DEBIAN_FRONTEND noninteractive

# Update and install dependencies
RUN apt-get update && \
    apt-get install -y wget llvm bc g++ clang gpg curl tar xz-utils make gcc flex bison libssl-dev \
    libelf-dev protobuf-compiler pkg-config libunwind-dev libprotobuf-dev libevent-dev libgtest-dev \
    iproute2 ethtool iputils-ping git net-tools linux-headers-generic systemd bpftrace gdb kmod

# Clone the Electrode repository
COPY ./Electrode /app
COPY ./scenario7/fast_common.h /app/xdp-handler
COPY ./scenario7/fast_user.c /app/xdp-handler
COPY ./scenario7/config.txt /app

WORKDIR /app

USER root

# Create an entrypoint script to handle mount and start
RUN mv ./start.sh /usr/local/bin/start.sh
RUN chmod +x /usr/local/bin/start.sh

# Build the xdp modules and replica code
RUN bash kernel-src-download.sh && \
    bash kernel-src-prepare.sh && \
    cd xdp-handler && make clean && make EXTRA_CFLAGS="-DTC_BROADCAST" && \
    cd .. && make clean && make CXXFLAGS="-DTC_BROADCAST" PARANOID=0

# Configure NIC and irqbalance
RUN ip link show ens1f1np1 && ip link set ens1f1np1 mtu 3000 up || echo "Interface ens1f1np1 not found, skipping configuration" && \
    ethtool -C ens1f1np1 adaptive-rx off adaptive-tx off rx-frames 1 rx-usecs 0 tx-frames 1 tx-usecs 0 && \
    ethtool -L ens1f1np1 combined 1 && \
    service irqbalance stop && \
    (let CPU=0; cd /sys/class/net/ens1f1np1/device/msi_irqs/; for IRQ in *; do echo $CPU | tee /proc/irq/$IRQ/smp_affinity_list; done) || echo "Skipping IRQ configuration"

ENTRYPOINT ["/usr/local/bin/start.sh"]
