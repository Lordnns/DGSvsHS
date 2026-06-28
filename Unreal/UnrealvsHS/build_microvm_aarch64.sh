#!/usr/bin/env bash

set -e

# Prerequisites on the build host:
#   - docker with buildx + binfmt_misc (cross-arch image build)
#   - qemu-system-aarch64 + cpio + gzip + curl
#   - An Unreal Linux Dedicated Server build for the ARM64 architecture
#     (UE5 Editor → Platforms → Linux ARM64 → Cook Content + Package Server)
#
# Usage:
#   ./build_microvm_aarch64.sh                        # auto-locate most recent ARM64 build
#   ./build_microvm_aarch64.sh /path/to/StagedBuild   # explicit staged-build root
#
# The staged build dir must contain <Project>Server.sh and
# <Project>/Binaries/LinuxArm64/<Project>Server.

UNREAL_PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$UNREAL_PROJECT_ROOT"

# Static IP configuration baked into init.sh. Override at build time, e.g.:
#   STATIC_IP=192.168.1.50 STATIC_GATEWAY=192.168.1.1 ./build_microvm_aarch64.sh
STATIC_IP="${STATIC_IP:-192.168.0.205}"
STATIC_CIDR="${STATIC_CIDR:-24}"
STATIC_GATEWAY="${STATIC_GATEWAY:-192.168.0.1}"
STATIC_DNS="${STATIC_DNS:-8.8.8.8}"
echo "==> Static IP for this build: ${STATIC_IP}/${STATIC_CIDR} gw=${STATIC_GATEWAY} dns=${STATIC_DNS}"

BUILD_DIR="${1:-}"
if [ -z "$BUILD_DIR" ]; then
    # Look for the most recent staged Unreal server tree containing an arm64 binary.
    # Heuristic: a regular file whose path ends in /Binaries/LinuxArm64/<X>Server
    # (UE convention — server binary has no extension; .debug/.sym don't match).
    BIN_PATH=$(find ../../Build -maxdepth 8 -type f -path "*/Binaries/LinuxArm64/*Server" 2>/dev/null \
               | sort | tail -1)
    if [ -n "$BIN_PATH" ]; then
        BUILD_DIR=$(cd "$(dirname "$BIN_PATH")/../../.." && pwd)
    fi
fi

if [ -z "$BUILD_DIR" ] || [ ! -d "$BUILD_DIR" ]; then
    cat >&2 <<EOF
ERROR: cannot find an Unreal Linux ARM64 server staged build.

Pass the staged-build directory explicitly:
    $0 /absolute/path/to/Build/<date>/UnrealServer/LinuxArm64Server

Build profile must target:
    Platform = Linux ARM64
    Build    = Server
    Output must contain <Project>Server.sh and
    <Project>/Binaries/LinuxArm64/<Project>Server.
EOF
    exit 1
fi

BUILD_DIR=$(cd "$BUILD_DIR" && pwd)

# Pick the launcher (top-level <Project>Server.sh) and derive project / binary names.
LAUNCHER=$(find "$BUILD_DIR" -maxdepth 1 -type f -name "*Server.sh" | head -1)
if [ -z "$LAUNCHER" ]; then
    echo "ERROR: no <Project>Server.sh launcher in $BUILD_DIR" >&2
    exit 1
fi
LAUNCHER_NAME=$(basename "$LAUNCHER")               # UnrealvsHSServer.sh
PROJECT_BINARY="${LAUNCHER_NAME%.sh}"               # UnrealvsHSServer
PROJECT_NAME="${PROJECT_BINARY%Server}"             # UnrealvsHS

BIN_FULL_PATH="$BUILD_DIR/$PROJECT_NAME/Binaries/LinuxArm64/$PROJECT_BINARY"
if [ ! -f "$BIN_FULL_PATH" ]; then
    echo "ERROR: expected server binary not found at $BIN_FULL_PATH" >&2
    exit 1
fi

echo "==> Using Unreal staged build at: $BUILD_DIR"
echo "==> Project:                     $PROJECT_NAME"
echo "==> Launcher:                    $LAUNCHER_NAME"
echo "==> Server binary:               $PROJECT_NAME/Binaries/LinuxArm64/$PROJECT_BINARY"

mkdir -p .microvm_aarch64
cd .microvm_aarch64

APK_MIRROR="https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/aarch64/"

if [ ! -f "vmlinuz-virt" ] || [ ! -d "modules" ]; then
    echo "==> Discovering latest linux-virt apk from $APK_MIRROR ..."
    APK_NAME=$(curl -fsSL "$APK_MIRROR" \
               | grep -oE 'linux-virt-[0-9.]+-r[0-9]+\.apk' \
               | sort -V | tail -1)
    if [ -z "$APK_NAME" ]; then
        echo "ERROR: could not find linux-virt apk in $APK_MIRROR" >&2
        echo "       (mirror layout changed, or curl/grep is broken)" >&2
        exit 1
    fi
    echo "==> Selected: $APK_NAME"
    echo "==> Fetching kernel + virtio_net modules..."
    curl -fsSL -O "$APK_MIRROR$APK_NAME"
    mkdir -p apk_extract modules
    tar -xzf "$APK_NAME" -C apk_extract
    cp apk_extract/boot/vmlinuz-virt vmlinuz-virt
    # Module dir name (e.g. 6.18.35-0-virt) is whatever the apk contains.
    KERNEL_MOD_DIR=$(ls apk_extract/lib/modules/ | head -1)
    if [ -z "$KERNEL_MOD_DIR" ]; then
        echo "ERROR: no module directory inside extracted apk" >&2
        exit 1
    fi
    echo "==> Module dir: $KERNEL_MOD_DIR"
    for m in failover net_failover virtio_net; do
        src=$(find "apk_extract/lib/modules/${KERNEL_MOD_DIR}" -name "${m}.ko.gz" | head -1)
        if [ -z "$src" ]; then
            echo "ERROR: ${m}.ko.gz not found in extracted apk" >&2
            exit 1
        fi
        gunzip -c "$src" > "modules/${m}.ko"
    done
    rm -rf apk_extract "$APK_NAME"
else
    echo "==> Kernel + modules already extracted. Skipping."
fi

# Stage the build, skipping the ~1.4 GB of *.debug / *.sym / *.target debug symbols.
echo "==> Staging Unreal build (excluding *.debug, *.sym, *.target)..."
rm -rf unreal_build
mkdir -p unreal_build
( cd "$BUILD_DIR" && tar -cf - --exclude='*.debug' --exclude='*.sym' --exclude='*.target' . ) \
    | ( cd unreal_build && tar -xf - )

cat > Dockerfile.microvm <<DOCKEREOF
FROM --platform=linux/arm64 debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \\
        libstdc++6 libgcc-s1 ca-certificates \\
        kmod iproute2 udhcpc coreutils procps less findutils \\
        libcurl4 libssl3 libgssapi-krb5-2 libnuma1 \\
        libxrandr2 libxcursor1 libxext6 libxi6 libxinerama1 libxss1 \\
        libasound2 libdbus-1-3 libnss3 libuuid1 libgl1 libegl1 \\
    && rm -rf /var/lib/apt/lists/*

COPY unreal_build/ /opt/app/
RUN chmod +x "/opt/app/${LAUNCHER_NAME}" && \\
    chmod +x "/opt/app/${PROJECT_NAME}/Binaries/LinuxArm64/${PROJECT_BINARY}"

# UE5 refuses to run as root. Create an unprivileged user and give it ownership.
RUN useradd -u 1000 -m -s /bin/sh unreal && \\
    chown -R unreal:unreal /opt/app

COPY init.sh /init
RUN chmod +x /init
DOCKEREOF

cat > init.sh <<INITEOF
#!/bin/sh

# Mount /dev first so /dev/console exists, then bind I/O to it explicitly.
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

# UE writes user config to \$HOME/.config/Epic/... — point at tmpfs.
export HOME=/tmp
export TMPDIR=/tmp
log "env: HOME=\$HOME TMPDIR=\$TMPDIR"

for m in failover net_failover virtio_net; do
    insmod /lib/modules/\${m}.ko 2>/dev/null
    log "insmod \${m}"
done

ip link set lo up && log "lo up"
ip addr add ${STATIC_IP}/${STATIC_CIDR} dev eth0 && log "ip addr add ${STATIC_IP}/${STATIC_CIDR}"
ip link set eth0 up && log "eth0 up"
ip route add default via ${STATIC_GATEWAY} 2>/dev/null && log "default route via ${STATIC_GATEWAY}" || log "WARN: failed to add default route"
echo "nameserver ${STATIC_DNS}" > /etc/resolv.conf && log "dns: ${STATIC_DNS}"

log "===== diag block ====="
log "kernel: \$(uname -a)"
log "/opt/app:"
ls -1 /opt/app | while read f; do log "  \$f"; done
log "checking launcher at /opt/app/${LAUNCHER_NAME}"
if [ -f "/opt/app/${LAUNCHER_NAME}" ]; then
    log "launcher exists, size: \$(stat -c%s /opt/app/${LAUNCHER_NAME} 2>/dev/null) bytes"
else
    log "WARN: launcher NOT FOUND at /opt/app/${LAUNCHER_NAME}"
fi
log "checking server binary at /opt/app/${PROJECT_NAME}/Binaries/LinuxArm64/${PROJECT_BINARY}"
if [ -f "/opt/app/${PROJECT_NAME}/Binaries/LinuxArm64/${PROJECT_BINARY}" ]; then
    log "server binary exists, size: \$(stat -c%s /opt/app/${PROJECT_NAME}/Binaries/LinuxArm64/${PROJECT_BINARY} 2>/dev/null) bytes"
else
    log "WARN: server binary NOT FOUND"
fi
log "checking server binary dynamic deps"
MISSING=\$(ldd /opt/app/${PROJECT_NAME}/Binaries/LinuxArm64/${PROJECT_BINARY} 2>&1 | grep 'not found' || true)
if [ -n "\$MISSING" ]; then
    log "WARN: server binary has missing libs:"
    echo "\$MISSING" | while read line; do log "  \$line"; done
else
    log "server binary: all deps resolved"
fi
log "===== diag end ====="

log "launching Unreal (as user 'unreal', uid 1000): ./${LAUNCHER_NAME} -QuicPort=7780 -log"
log "Output -> /tmp/unreal.log (tail -f /tmp/unreal.log from shell)"

cd /opt/app
# Drop root before exec — UE5 refuses to run as root. -allowroot also passed as belt-and-suspenders.
su unreal -c "cd /opt/app && ./${LAUNCHER_NAME} -QuicPort=7780 -log -allowroot -nullrhi -nosound -unattended" > /tmp/unreal.log 2>&1 &
UNREAL_PID=\$!
log "Unreal launched as PID \$UNREAL_PID"

setsid /bin/sh -i </dev/console >/dev/console 2>&1 &
log "diagnostic shell PID \$! — press Enter for prompt"

(
    wait \$UNREAL_PID
    UEXIT=\$?
    echo "[init] Unreal exited code \$UEXIT — see /tmp/unreal.log" > /dev/kmsg
) &

while true; do
    sleep 3600
done
INITEOF

echo "==> Building aarch64 rootfs via Docker buildx..."
docker buildx build --platform linux/arm64 --load \
    -t dgsvshs-unreal-microvm:latest \
    -f Dockerfile.microvm .

echo "==> Exporting rootfs..."
CID=$(docker create --platform linux/arm64 dgsvshs-unreal-microvm:latest)
rm -rf rootfs_dir
mkdir rootfs_dir
docker export "$CID" | tar -x -C rootfs_dir
docker rm "$CID" >/dev/null

mkdir -p rootfs_dir/lib/modules
cp modules/*.ko rootfs_dir/lib/modules/

echo "==> Packaging initramfs (this can take a while — Unreal build is large)..."
cd rootfs_dir
find . | cpio -H newc -o 2>/dev/null | gzip -9 > ../initramfs.cpio.gz
cd ..

echo "==> Booting MicroVM via QEMU (UDP 7780 forwarded to host)..."
qemu-system-aarch64 \
  -machine virt,accel=hvf \
  -cpu host \
  -m 2048 \
  -kernel vmlinuz-virt \
  -initrd initramfs.cpio.gz \
  -append "console=ttyAMA0 cgroup_disable=memory,pids" \
  -nographic \
  -netdev user,id=net0,hostfwd=udp::7780-:7780 \
  -device virtio-net-pci,netdev=net0
