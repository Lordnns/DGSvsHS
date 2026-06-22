#!/usr/bin/env bash

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_ROOT"

# Prerequisites on the build host:
#   - dotnet SDK 10 (for `dotnet publish -r linux-musl-x64`)
#   - docker with buildx (no binfmt needed — same-arch x86_64 builds run native)
#   - qemu-system-x86_64 + cpio + gzip + curl
# On Linux x86_64 host (Ryzen / Proxmox / Hetzner): accel=kvm is native speed.
# On macOS Intel: accel=hvf. On WSL or any non-KVM host: drop the accel flag (TCG).
#
# Usage:
#   ./build_microvm_x86_64.sh             # normal build
#   ./build_microvm_x86_64.sh --god-mode  # build with GODMODE_DEFAULT (-> *-godmode artifacts)

god_mode=0
for arg in "$@"; do
    case "$arg" in
        --god-mode) god_mode=1 ;;
        *) echo "[build] unknown arg: $arg" >&2; exit 2 ;;
    esac
done
flavor_suffix=""
godmode_props=()
if [[ $god_mode -eq 1 ]]; then
    flavor_suffix="-godmode"
    godmode_props=(-p:GodModeDefault=true)
fi
echo "==> Flavor: ${flavor_suffix:-normal}"

# Static IP configuration baked into init.sh. Override at build time, e.g.:
#   STATIC_IP=192.168.1.50 STATIC_GATEWAY=192.168.1.1 ./build_microvm_x86_64.sh
STATIC_IP="${STATIC_IP:-192.168.0.205}"
STATIC_CIDR="${STATIC_CIDR:-24}"
STATIC_GATEWAY="${STATIC_GATEWAY:-192.168.0.1}"
STATIC_DNS="${STATIC_DNS:-8.8.8.8}"
echo "==> Static IP for this build: ${STATIC_IP}/${STATIC_CIDR} gw=${STATIC_GATEWAY} dns=${STATIC_DNS}"

echo "==> Publishing Arch Server (linux-musl-x64, self-contained)..."
dotnet publish -c Release \
               -r linux-musl-x64 \
               --self-contained true \
               -p:PublishSingleFile=true \
               -p:IncludeAllContentForSelfExtract=true \
               "${godmode_props[@]}" \
               -o "publish-microvm${flavor_suffix}"

mkdir -p .microvm_x86_64
cd .microvm_x86_64

KERNEL_VERSION="6.18.35-r0"
KERNEL_MOD_DIR="6.18.35-0-virt"

if [ ! -f "vmlinuz-virt" ] || [ ! -d "modules" ]; then
    echo "==> Fetching kernel + virtio_net modules from Alpine apk..."
    curl -sLO "https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/x86_64/linux-virt-${KERNEL_VERSION}.apk"
    mkdir -p apk_extract modules
    tar -xzf "linux-virt-${KERNEL_VERSION}.apk" -C apk_extract 2>/dev/null
    cp apk_extract/boot/vmlinuz-virt vmlinuz-virt
    for m in failover net_failover virtio_net; do
        src=$(find "apk_extract/lib/modules/${KERNEL_MOD_DIR}" -name "${m}.ko.gz")
        gunzip -c "$src" > "modules/${m}.ko"
    done
    rm -rf apk_extract "linux-virt-${KERNEL_VERSION}.apk"
else
    echo "==> Kernel + modules already extracted. Skipping."
fi

rm -rf publish
cp -a "../publish-microvm${flavor_suffix}" publish

cat > Dockerfile.microvm <<'DOCKEREOF'
FROM --platform=linux/amd64 alpine:3.21

# Add edge community as a tagged source so apk add @edge picks libmsquic from there
# without bumping everything else to edge.
RUN echo "@edge https://dl-cdn.alpinelinux.org/alpine/edge/community" >> /etc/apk/repositories

RUN apk add --no-cache \
        musl libstdc++ libgcc icu-libs \
        ca-certificates openssl \
        procps less \
        libmsquic@edge

COPY publish/ /opt/app/
RUN chmod +x /opt/app/DGSvsHS.ArchServer

ENV DOTNET_BUNDLE_EXTRACT_BASE_DIR=/opt/dotnet_extract
RUN mkdir -p /opt/dotnet_extract && \
    cd /opt/app && \
    (timeout 5 ./DGSvsHS.ArchServer >/dev/null 2>&1 || true) && \
    EXTRACTED=$(find /opt/dotnet_extract -name 'libmsquic-openssl.so' -type f | head -1) && \
    if [ -n "$EXTRACTED" ]; then \
        echo "Replacing bundled libmsquic-openssl.so at $EXTRACTED" && \
        rm -f "$EXTRACTED" && \
        ln -sf /usr/lib/libmsquic.so.2 "$EXTRACTED"; \
    else \
        echo "WARN: bundled libmsquic-openssl.so not found after pre-extract"; \
    fi

COPY init.sh /init
RUN chmod +x /init
DOCKEREOF

cat > init.sh <<INITEOF
#!/bin/sh

# Mount /dev first so /dev/console exists, then bind our I/O to it explicitly.
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t tmpfs tmpfs /tmp 2>/dev/null

exec >/dev/console 2>&1

log() {
    msg="[init] \$*"
    echo "\$msg"
    echo "\$msg" > /dev/kmsg 2>/dev/null
}

log "starting (PID \$\$)"
log "all four basic mounts done"

export DOTNET_BUNDLE_EXTRACT_BASE_DIR=/opt/dotnet_extract
log "env set: DOTNET_BUNDLE_EXTRACT_BASE_DIR=\$DOTNET_BUNDLE_EXTRACT_BASE_DIR"

for m in failover net_failover virtio_net; do
    insmod /lib/modules/\${m}.ko 2>/dev/null
    log "insmod \${m}"
done

ip link set lo up   && log "lo up"

# Static IP — baked in at build time. Override with STATIC_IP=... at build invocation.
ip addr add ${STATIC_IP}/${STATIC_CIDR} dev eth0 && log "ip addr add ${STATIC_IP}/${STATIC_CIDR}"
ip link set eth0 up && log "eth0 up"
ip route add default via ${STATIC_GATEWAY} 2>/dev/null && log "default route via ${STATIC_GATEWAY}" || log "WARN: failed to add default route via ${STATIC_GATEWAY}"
echo "nameserver ${STATIC_DNS}" > /etc/resolv.conf && log "dns: ${STATIC_DNS}"
log "ip addr now: \$(ip -4 -o addr show eth0 | awk '{print \$4}')"

echo ""
echo "######################################################"
echo "##  DGSvsHS Arch MicroVM diag                       ##"
echo "######################################################"
uname -a
echo "--- /opt/app contents ---"
ls -la /opt/app
echo "--- pre-extracted libmsquic-openssl.so ---"
find /opt/dotnet_extract -name 'libmsquic-openssl.so' -print -exec ls -la {} \;
echo "--- libmsquic on system ---"
ls -la /usr/lib/libmsquic* 2>&1
echo ""
echo "######################################################"
echo "##  Launching DGSvsHS.ArchServer                    ##"
echo "######################################################"
echo ""

log "launching DGSvsHS.ArchServer"
log "Output -> /tmp/arch.log (tail -f /tmp/arch.log from shell to watch)"

cd /opt/app
# Server output to log file so it doesn't interleave with the shell on /dev/console.
./DGSvsHS.ArchServer > /tmp/arch.log 2>&1 &
ARCH_PID=\$!
log "ArchServer launched as PID \$ARCH_PID"

setsid /bin/sh -i </dev/console >/dev/console 2>&1 &
SHELL_PID=\$!
log "diagnostic shell PID \$SHELL_PID — press Enter for prompt"

# Poll server in the background; surface its exit code via kmsg.
(
    wait \$ARCH_PID
    AEXIT=\$?
    echo "[init] ArchServer exited code \$AEXIT — see /tmp/arch.log for output" > /dev/kmsg
) &

# PID 1 must never exit — sleep forever.
while true; do
    sleep 3600
done
INITEOF

echo "==> Building x86_64 rootfs via Docker buildx..."
docker buildx build --platform linux/amd64 --load \
    -t "dgsvshs-arch-microvm-x64${flavor_suffix}:latest" \
    -f Dockerfile.microvm .

echo "==> Exporting rootfs..."
CID=$(docker create --platform linux/amd64 "dgsvshs-arch-microvm-x64${flavor_suffix}:latest")
rm -rf rootfs_dir
mkdir rootfs_dir
docker export "$CID" | tar -x -C rootfs_dir
docker rm "$CID" >/dev/null

mkdir -p rootfs_dir/lib/modules
cp modules/*.ko rootfs_dir/lib/modules/

echo "==> Packaging initramfs..."
cd rootfs_dir
find . | cpio -H newc -o 2>/dev/null | gzip -9 > "../initramfs${flavor_suffix}.cpio.gz"
cd ..

echo "==> Packaging bootable ISO..."
command -v grub-mkrescue >/dev/null || {
    cat >&2 <<EOF
ERROR: grub-mkrescue not found. Install ISO build tools:
    sudo apt install -y grub-pc-bin grub-efi-amd64-bin xorriso mtools
EOF
    exit 1
}

rm -rf iso_staging
mkdir -p iso_staging/boot/grub
cp vmlinuz-virt                          iso_staging/boot/vmlinuz
cp "initramfs${flavor_suffix}.cpio.gz"   iso_staging/boot/initrd.img

flavor_label="${flavor_suffix:+ (godmode)}"
cat > iso_staging/boot/grub/grub.cfg <<GRUBEOF
set timeout=2
set default=0
menuentry "DGSvsHS Arch MicroVM${flavor_label}" {
    linux  /boot/vmlinuz console=tty0 console=ttyS0 cgroup_disable=memory,pids
    initrd /boot/initrd.img
}
GRUBEOF

grub-mkrescue -o "arch-microvm${flavor_suffix}.iso" iso_staging >/dev/null 2>&1
rm -rf iso_staging

echo "==> ISO ready: $(pwd)/arch-microvm${flavor_suffix}.iso"
echo "    Upload via Proxmox UI > Storage > ISO Images > Upload."
echo "    Create VM, set CD/DVD to this ISO, start. Logs in the Console tab."
