#!/bin/bash
KERNEL="/sel4-image/kernel-x86_64-pc99"
INITRD="/sel4-image/capdl-loader-image-x86_64-pc99"
TIMEOUT="${TIMEOUT:-30}"

timeout "$TIMEOUT" qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64,+rdrand,+fsgsbase,+xsave,+xsaveopt \
    -m 512 \
    -nographic \
    -serial mon:stdio \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    "$@"

EXIT_CODE=$?
if [ $EXIT_CODE -eq 124 ]; then
    echo "QEMU_TIMEOUT: Exited after ${TIMEOUT}s"
fi
exit $EXIT_CODE
