#!/usr/bin/env bash

set -e

# x86_64 sibling of build_microvm_aarch64.sh — produces a vmlinuz + initramfs
# pair that boots on an x86_64 host with native KVM (no TCG emulation), which
# is required for valid power-measurement trials on Samuel's Ryzen / Proxmox
# homelab. The original aarch64 script (VALERE91) is untouched.

echo "==> Compiling Bevy Server..."
cargo zigbuild --target x86_64-unknown-linux-musl --release

mkdir -p .microvm_x86_64
cd .microvm_x86_64

KERNEL_VERSION="6.18.35-r0"
KERNEL_MOD_DIR="6.18.35-0-virt"

# Pull both kernel and modules from the same apk so versions always match.
if [ ! -f "vmlinuz-virt" ] || [ ! -d "modules" ]; then
    echo "==> Fetching kernel + virtio_net modules from Alpine apk..."
    curl -sLO "https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/x86_64/linux-virt-${KERNEL_VERSION}.apk"
    mkdir -p apk_extract modules
    tar -xzf "linux-virt-${KERNEL_VERSION}.apk" -C apk_extract 2>/dev/null
    cp apk_extract/boot/vmlinuz-virt vmlinuz-virt
    # vmlinuz-virt ships virtio_net as a module (CONFIG_VIRTIO_NET=m). Load
    # order at init: failover → net_failover → virtio_net.
    for m in failover net_failover virtio_net; do
        src=$(find "apk_extract/lib/modules/${KERNEL_MOD_DIR}" -name "${m}.ko.gz")
        gunzip -c "$src" > "modules/${m}.ko"
    done
    rm -rf apk_extract "linux-virt-${KERNEL_VERSION}.apk"
else
    echo "==> Kernel + modules already extracted. Skipping."
fi

echo "==> Packaging initramfs..."
mkdir -p rootfs_dir
cp ../target/x86_64-unknown-linux-musl/release/cli rootfs_dir/init
mkdir -p rootfs_dir/modules
cp modules/*.ko rootfs_dir/modules/

cd rootfs_dir
find . | cpio -H newc -o 2>/dev/null | gzip -9 > ../initramfs.cpio.gz
cd ..

echo "==> Packaging bootable ISO..."
command -v grub-mkrescue >/dev/null || {
    cat >&2 <<EOF
ERROR: grub-mkrescue not found. Install ISO build tools:
    sudo apt install -y grub-pc-bin grub-efi-amd64-bin xorriso mtools
EOF
    exit 1
}

# Hybrid BIOS+UEFI ISO via grub-mkrescue — boots in Proxmox under SeaBIOS or OVMF
# with no VM-conf hacks. console=tty0 routes logs to Proxmox's default VGA Console;
# console=ttyS0 also writes to serial for `qm terminal` if you wire serial0.
rm -rf iso_staging
mkdir -p iso_staging/boot/grub
cp vmlinuz-virt        iso_staging/boot/vmlinuz
cp initramfs.cpio.gz   iso_staging/boot/initrd.img

cat > iso_staging/boot/grub/grub.cfg <<'GRUBEOF'
set timeout=2
set default=0
menuentry "DGSvsHS Bevy MicroVM" {
    linux  /boot/vmlinuz console=ttyS0 console=tty0 panic=1 cgroup_disable=memory,pids
    initrd /boot/initrd.img
}
GRUBEOF

grub-mkrescue -o bevy-microvm.iso iso_staging >/dev/null 2>&1
rm -rf iso_staging

echo "==> ISO ready: $(pwd)/bevy-microvm.iso"
echo "    Upload via Proxmox UI > Storage > ISO Images > Upload."
echo "    Create VM, set CD/DVD to this ISO, start. Logs in the Console tab."
