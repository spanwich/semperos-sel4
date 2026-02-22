FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    qemu-system-x86 \
    && rm -rf /var/lib/apt/lists/*
COPY run-qemu.sh /run-qemu.sh
RUN chmod +x /run-qemu.sh
ENTRYPOINT ["/run-qemu.sh"]
