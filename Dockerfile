FROM ubuntu:20.04

ENV DEBIAN_FRONTEND noninteractive

# Update and install dependencies
RUN apt-get update && \
    apt-get install -y wget llvm bc g++ clang gpg curl tar xz-utils make gcc flex bison libssl-dev \
    libelf-dev protobuf-compiler pkg-config libunwind-dev libprotobuf-dev libevent-dev libgtest-dev \
    iproute2 ethtool iputils-ping git net-tools linux-headers-generic systemd

# Install kernel 5.8.0
RUN wget https://raw.githubusercontent.com/pimlie/ubuntu-mainline-kernel.sh/master/ubuntu-mainline-kernel.sh && \
    bash ubuntu-mainline-kernel.sh -i 5.8.0 && \
    rm ubuntu-mainline-kernel.sh

# Reboot to load the new kernel
RUN echo "#!/bin/bash\nreboot" > /usr/sbin/reboot && chmod +x /usr/sbin/reboot

# Clone the Electrode repository
COPY ./Electrode /app

WORKDIR /app

USER root

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

# Create an entrypoint script to handle mount and start
RUN echo '#!/bin/bash\nmount -t bpf none /sys/fs/bpf\nexec "$@"' > /usr/local/bin/start.sh && chmod +x /usr/local/bin/start.sh

ENTRYPOINT ["/usr/local/bin/start.sh"]
CMD ["./xdp-handler/fast", "ens1f1np1"]
