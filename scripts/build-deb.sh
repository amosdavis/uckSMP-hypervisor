#!/bin/bash
# build-deb.sh - Build ucksmp-dkms and ucksmp-tools .deb packages
#
# Usage: ./scripts/build-deb.sh [version]
#
# Requires: build-essential, debhelper, dkms, fakeroot
set -euo pipefail

VERSION="${1:-0.1.0}"
SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILDDIR="${SRCDIR}/build-deb"

rm -rf "${BUILDDIR}"
mkdir -p "${BUILDDIR}"

echo "=== Building ucksmp ${VERSION} ==="

# --- ucksmp-dkms ---
DKMS_PKG="ucksmp-dkms_${VERSION}_all"
DKMS_DIR="${BUILDDIR}/${DKMS_PKG}"

mkdir -p "${DKMS_DIR}/DEBIAN"
mkdir -p "${DKMS_DIR}/usr/src/ucksmp-${VERSION}"

# Copy kernel module sources
cp "${SRCDIR}"/uck_main.c "${SRCDIR}"/uck_region.c "${SRCDIR}"/uck_page.c \
   "${SRCDIR}"/uck_net.c "${SRCDIR}"/uck_heartbeat.c "${SRCDIR}"/uck_sysinfo.c \
   "${SRCDIR}"/uck_proc.c "${SRCDIR}"/uck_migrate.c "${SRCDIR}"/uck_load.c \
   "${SRCDIR}"/uck_exec.c "${SRCDIR}"/uck_hyper.c "${SRCDIR}"/uck_batch.c \
   "${SRCDIR}"/uck_futex.c "${SRCDIR}"/uck_cgroup.c "${SRCDIR}"/uck_rdma.c \
   "${SRCDIR}"/uck_join.c \
   "${SRCDIR}"/uck.h "${SRCDIR}"/uck_internal.h \
   "${SRCDIR}"/Kbuild "${SRCDIR}"/Makefile \
   "${DKMS_DIR}/usr/src/ucksmp-${VERSION}/"

cp "${SRCDIR}/debian/dkms.conf" "${DKMS_DIR}/usr/src/ucksmp-${VERSION}/dkms.conf"
sed -i "s/PACKAGE_VERSION=\".*\"/PACKAGE_VERSION=\"${VERSION}\"/" \
    "${DKMS_DIR}/usr/src/ucksmp-${VERSION}/dkms.conf"

cat > "${DKMS_DIR}/DEBIAN/control" <<EOF
Package: ucksmp-dkms
Version: ${VERSION}
Architecture: all
Maintainer: Amos Davis <amosdavis@users.noreply.github.com>
Depends: dkms
Section: kernel
Priority: optional
Homepage: https://github.com/amosdavis/uckSMP-hypervisor
Description: UCK SMP Hypervisor - kernel module (DKMS)
 A Linux kernel module that turns multiple QEMU VMs into a single SMP-like
 system with transparent shared memory, automatic fork distribution across
 nodes, distributed command execution, and unified resource reporting.
 .
 This package provides the kernel module source and DKMS configuration to
 automatically build and install uck.ko for your running kernel.
EOF

cat > "${DKMS_DIR}/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
set -e
PACKAGE_NAME="ucksmp"
PACKAGE_VERSION="__VERSION__"
if [ "$1" = "configure" ]; then
    dkms add -m "${PACKAGE_NAME}" -v "${PACKAGE_VERSION}" || true
    dkms build -m "${PACKAGE_NAME}" -v "${PACKAGE_VERSION}" || true
    dkms install -m "${PACKAGE_NAME}" -v "${PACKAGE_VERSION}" || true
fi
POSTINST
sed -i "s/__VERSION__/${VERSION}/" "${DKMS_DIR}/DEBIAN/postinst"
chmod 755 "${DKMS_DIR}/DEBIAN/postinst"

cat > "${DKMS_DIR}/DEBIAN/prerm" <<'PRERM'
#!/bin/sh
set -e
PACKAGE_NAME="ucksmp"
PACKAGE_VERSION="__VERSION__"
if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    dkms remove -m "${PACKAGE_NAME}" -v "${PACKAGE_VERSION}" --all || true
fi
PRERM
sed -i "s/__VERSION__/${VERSION}/" "${DKMS_DIR}/DEBIAN/prerm"
chmod 755 "${DKMS_DIR}/DEBIAN/prerm"

dpkg-deb --build --root-owner-group "${DKMS_DIR}"
echo "Built: ${DKMS_DIR}.deb"

# --- ucksmp-tools ---
TOOLS_PKG="ucksmp-tools_${VERSION}_amd64"
TOOLS_DIR="${BUILDDIR}/${TOOLS_PKG}"

mkdir -p "${TOOLS_DIR}/DEBIAN"
mkdir -p "${TOOLS_DIR}/usr/sbin"

# Build userspace tools
make -C "${SRCDIR}" tools
for tool in uckd uckctl uck_smp uck_run uck_test uck_restore; do
    install -m 755 "${SRCDIR}/${tool}" "${TOOLS_DIR}/usr/sbin/${tool}"
done

cat > "${TOOLS_DIR}/DEBIAN/control" <<EOF
Package: ucksmp-tools
Version: ${VERSION}
Architecture: amd64
Maintainer: Amos Davis <amosdavis@users.noreply.github.com>
Depends: libc6
Recommends: ucksmp-dkms
Section: admin
Priority: optional
Homepage: https://github.com/amosdavis/uckSMP-hypervisor
Description: UCK SMP Hypervisor - userspace tools
 Userspace tools for the UCK SMP Hypervisor:
  - uckd: daemon that configures the kernel module via ioctls at boot
  - uckctl: CLI for cluster status, jobs, exec, migrate
  - uck_smp: SMP wrapper that registers a process for fork distribution
  - uck_run: parallel runner for distributed command execution
  - uck_test: shared memory test tool
  - uck_restore: process restore stub for migrated processes
EOF

dpkg-deb --build --root-owner-group "${TOOLS_DIR}"
echo "Built: ${TOOLS_DIR}.deb"

echo ""
echo "=== Packages built ==="
ls -lh "${BUILDDIR}"/*.deb
