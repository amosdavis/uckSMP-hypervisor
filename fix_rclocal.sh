#!/bin/bash
# fix_rclocal.sh - Update rc.local on both nodes for UCK
set -ex

# Node1
mount -o loop /root/mosix/node1.img /mnt/node1

cat > /mnt/node1/etc/rc.local <<'EOF'
#!/bin/bash
# UCK: Unified Compute Kernel boot setup
insmod /lib/modules/6.1.0-42-amd64/extra/uck.ko
sleep 1
uckd --node-id 1 --ip 10.4.4.100 --port 9999 \
     --remote 2:10.4.4.101:9999 \
     --create-region 1:67108864 \
     --daemon &
EOF
chmod +x /mnt/node1/etc/rc.local
echo "=== Node1 rc.local ==="
cat /mnt/node1/etc/rc.local
umount /mnt/node1

# Node2
mount -o loop /root/mosix/node2.img /mnt/node2

cat > /mnt/node2/etc/rc.local <<'EOF'
#!/bin/bash
# UCK: Unified Compute Kernel boot setup
insmod /lib/modules/6.1.0-42-amd64/extra/uck.ko
sleep 1
uckd --node-id 2 --ip 10.4.4.101 --port 9999 \
     --remote 1:10.4.4.100:9999 \
     --join-region 1:67108864:1 \
     --daemon &
EOF
chmod +x /mnt/node2/etc/rc.local
echo "=== Node2 rc.local ==="
cat /mnt/node2/etc/rc.local
umount /mnt/node2

echo "=== BOTH RC.LOCALS UPDATED ==="
