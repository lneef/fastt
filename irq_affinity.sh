#!/bin/bash
# adapted from https://oneuptime.com/blog/post/2026-03-02-how-to-configure-irq-affinity-for-network-performance-on-ubuntu/view
NIC={$1:-ens2}
CPUS=$(nproc)
IRQS=$(grep "$NIC" /proc/interrupts | awk -F: '{print $1}' | tr -d ' ')

CPU=0
for IRQ in $IRQS; do
    echo "$CPU" > /proc/irq/$IRQ/smp_affinity_list
    echo "IRQ $IRQ -> CPU $CPU"
    # Cycle through available CPUs
    CPU=$(( (CPU + 1) % CPUS ))
done
~
