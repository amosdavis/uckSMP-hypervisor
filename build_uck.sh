#!/bin/bash
# build_uck.sh - Build and install UCK module into node images
#
# Run from WSL2:
#   bash build_uck.sh
#
# Builds the kernel module against the VM's kernel headers via chroot,
# installs module + tools on both node images.

set -ex

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UCK_DIR="${SCRIPT_DIR}"
MOSIX_DIR=/root/mosix

NODE1_MNT=/mnt/node1
NODE2_MNT=/mnt/node2

echo "===================================="
echo "Building UCK kernel module"
echo "===================================="

# Copy sources to staging area
mkdir -p /root/dsm_src
cp "${UCK_DIR}"/uck*.c "${UCK_DIR}"/uck*.h "${UCK_DIR}"/Kbuild "${UCK_DIR}"/Makefile /root/dsm_src/
sed -i 's/\r//' /root/dsm_src/*

# --- Build in node1's chroot ---
mkdir -p ${NODE1_MNT}
mount -o loop ${MOSIX_DIR}/node1.img ${NODE1_MNT}

mkdir -p ${NODE1_MNT}/root/dsm
cp /root/dsm_src/* ${NODE1_MNT}/root/dsm/

# Bind-mount kernel infrastructure
mount --bind /proc ${NODE1_MNT}/proc 2>/dev/null || true
mount --bind /sys ${NODE1_MNT}/sys 2>/dev/null || true
mount --bind /dev ${NODE1_MNT}/dev 2>/dev/null || true

KVER=$(ls ${NODE1_MNT}/lib/modules/ | head -1)
echo "Building against kernel: ${KVER}"

chroot ${NODE1_MNT} bash -c "cd /root/dsm && make KDIR=/lib/modules/${KVER}/build"

# Install on node1
mkdir -p ${NODE1_MNT}/lib/modules/${KVER}/extra
cp ${NODE1_MNT}/root/dsm/uck.ko ${NODE1_MNT}/lib/modules/${KVER}/extra/
cp ${NODE1_MNT}/root/dsm/uckd ${NODE1_MNT}/usr/sbin/
cp ${NODE1_MNT}/root/dsm/uck_test ${NODE1_MNT}/usr/sbin/
cp ${NODE1_MNT}/root/dsm/uckctl ${NODE1_MNT}/usr/sbin/
cp ${NODE1_MNT}/root/dsm/uck_restore ${NODE1_MNT}/usr/sbin/

# Save copies for node2
cp ${NODE1_MNT}/root/dsm/uck.ko /tmp/uck.ko
cp ${NODE1_MNT}/root/dsm/uckd /tmp/uckd
cp ${NODE1_MNT}/root/dsm/uck_test /tmp/uck_test
cp ${NODE1_MNT}/root/dsm/uckctl /tmp/uckctl
cp ${NODE1_MNT}/root/dsm/uck_restore /tmp/uck_restore

echo "=== Node1: UCK installed ==="
umount ${NODE1_MNT}/proc ${NODE1_MNT}/sys ${NODE1_MNT}/dev 2>/dev/null || true
umount ${NODE1_MNT}

# --- Install into node2 ---
mkdir -p ${NODE2_MNT}
mount -o loop ${MOSIX_DIR}/node2.img ${NODE2_MNT}

KVER2=$(ls ${NODE2_MNT}/lib/modules/ | head -1)
mkdir -p ${NODE2_MNT}/lib/modules/${KVER2}/extra
cp /tmp/uck.ko ${NODE2_MNT}/lib/modules/${KVER2}/extra/
cp /tmp/uckd ${NODE2_MNT}/usr/sbin/
cp /tmp/uck_test ${NODE2_MNT}/usr/sbin/
cp /tmp/uckctl ${NODE2_MNT}/usr/sbin/
cp /tmp/uck_restore ${NODE2_MNT}/usr/sbin/

echo "=== Node2: UCK installed ==="
umount ${NODE2_MNT}

rm -f /tmp/uck.ko /tmp/uckd /tmp/uck_test /tmp/uckctl /tmp/uck_restore

echo ""
echo "============================================"
echo "UCK BUILD AND INSTALL COMPLETE"
echo ""
echo "After booting both nodes, test with:"
echo "  uckctl status"
echo "  cat /proc/uck/nodes"
echo "  Node1: uck_test write 1"
echo "  Node2: uck_test read 2"
echo "============================================"
