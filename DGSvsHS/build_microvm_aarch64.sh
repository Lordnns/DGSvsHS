#!/usr/bin/env bash

set -e

# Prerequisites on the build host:
#   - docker with buildx + binfmt_misc (cross-arch image build)
#   - qemu-system-aarch64 + cpio + gzip + curl
#   - A Unity Linux Dedicated Server build for the ARM64 architecture
#     (Build Settings → Dedicated Server → Linux → Architecture: ARM64)
#
# Usage:
#   ./build_microvm_aarch64.sh                        # auto-locate most recent ARM64 build
#   ./build_microvm_aarch64.sh /path/to/Unity/Server  # explicit build dir
#
# The build dir must contain DGSvsHS (the headless binary), UnityPlayer.so, and DGSvsHS_Data/.

UNITY_PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$UNITY_PROJECT_ROOT"

# Static IP configuration baked into init.sh. Override at build time, e.g.:
#   STATIC_IP=192.168.1.50 STATIC_GATEWAY=192.168.1.1 ./build_microvm_aarch64.sh
STATIC_IP="${STATIC_IP:-192.168.0.205}"
STATIC_CIDR="${STATIC_CIDR:-24}"
STATIC_GATEWAY="${STATIC_GATEWAY:-192.168.0.1}"
STATIC_DNS="${STATIC_DNS:-8.8.8.8}"
echo "==> Static IP for this build: ${STATIC_IP}/${STATIC_CIDR} gw=${STATIC_GATEWAY} dns=${STATIC_DNS}"

BUILD_DIR="${1:-}"
if [ -z "$BUILD_DIR" ]; then
    # Look for the most recent dated build dir containing a Linux Server tree.
    # Heuristic: directory that contains UnityPlayer.so (the binary may be named
    # after the project, the build profile, or have a .aarch64 suffix).
    BUILD_DIR=$(find ../Build -mindepth 2 -maxdepth 5 -type f -name "UnityPlayer.so" 2>/dev/null \
                | while read f; do dirname "$f"; done \
                | sort | tail -1)
fi

if [ -z "$BUILD_DIR" ] || [ ! -f "$BUILD_DIR/UnityPlayer.so" ]; then
    cat >&2 <<EOF
ERROR: cannot find a Unity Linux ARM64 server build.

Pass the build directory explicitly:
    $0 /absolute/path/to/Build/<date>/Server/LinuxServerARM64

Build profile must target:
    Platform     = Dedicated Server
    Target       = Linux
    Architecture = ARM64
    Output dir must contain the entrypoint binary, UnityPlayer.so, and a *_Data/ folder.
EOF
    exit 1
fi

DATA_DIR=$(find "$BUILD_DIR" -maxdepth 1 -type d -name "*_Data" | head -1)
if [ -z "$DATA_DIR" ]; then
    echo "ERROR: no *_Data folder found in $BUILD_DIR" >&2
    exit 1
fi
DATA_BASENAME=$(basename "$DATA_DIR")
BINARY_BASENAME="${DATA_BASENAME%_Data}"
# Entrypoint binary file: try <basename>, <basename>.aarch64 in that order.
if   [ -f "$BUILD_DIR/$BINARY_BASENAME" ];          then BINARY_FILE="$BINARY_BASENAME"
elif [ -f "$BUILD_DIR/$BINARY_BASENAME.aarch64" ];  then BINARY_FILE="$BINARY_BASENAME.aarch64"
else
    echo "ERROR: no entrypoint binary matching $BINARY_BASENAME[.aarch64] in $BUILD_DIR" >&2
    exit 1
fi

BUILD_DIR=$(cd "$BUILD_DIR" && pwd)

echo "==> Using Unity build at: $BUILD_DIR"
echo "==> Entrypoint binary:    $BINARY_FILE"

mkdir -p .microvm_aarch64
cd .microvm_aarch64

KERNEL_VERSION="6.18.35-r0"
KERNEL_MOD_DIR="6.18.35-0-virt"

if [ ! -f "vmlinuz-virt" ] || [ ! -d "modules" ]; then
    echo "==> Fetching kernel + virtio_net modules from Alpine apk..."
    curl -sLO "https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/aarch64/linux-virt-${KERNEL_VERSION}.apk"
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

rm -rf unity_build
cp -a "$BUILD_DIR" unity_build

cat > Dockerfile.microvm <<DOCKEREOF
FROM --platform=linux/arm64 debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \\
        libstdc++6 libgcc-s1 ca-certificates \\
        kmod iproute2 udhcpc coreutils procps less \\
        libcurl4 libssl3 libgssapi-krb5-2 \\
        libxrandr2 libxcursor1 libxext6 libxi6 libxinerama1 libxss1 \\
        libasound2 libdbus-1-3 libnss3 libuuid1 libgl1 libegl1 \\
    && rm -rf /var/lib/apt/lists/*

COPY unity_build/ /opt/app/
RUN chmod +x "/opt/app/${BINARY_FILE}"

COPY init.sh /init
RUN chmod +x /init
DOCKEREOF

cat > init.sh <<INITEOF
#!/usr/bin/dash

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

# Unity writes player log to \$HOME/.config/unity3d/... by default — point at tmpfs.
export HOME=/tmp
export TMPDIR=/tmp
log "env: HOME=\$HOME TMPDIR=\$TMPDIR"

for m in failover net_failover virtio_net; do
    insmod /lib/modules/\${m}.ko 2>/dev/null
    log "insmod \${m}"
done

ip link set lo up && log "lo up"
# Static IP — baked in at build time. Override with STATIC_IP=... at build invocation.
ip addr add ${STATIC_IP}/${STATIC_CIDR} dev eth0 && log "ip addr add ${STATIC_IP}/${STATIC_CIDR}"
ip link set eth0 up && log "eth0 up"
ip route add default via ${STATIC_GATEWAY} 2>/dev/null && log "default route via ${STATIC_GATEWAY}" || log "WARN: failed to add default route"
echo "nameserver ${STATIC_DNS}" > /etc/resolv.conf && log "dns: ${STATIC_DNS}"

log "===== diag block ====="
log "kernel: \$(uname -a)"
log "/opt/app:"
ls -1 /opt/app | while read f; do log "  \$f"; done
log "checking entrypoint binary at /opt/app/${BINARY_FILE}"
if [ -f "/opt/app/${BINARY_FILE}" ]; then
    log "binary exists, size: \$(stat -c%s /opt/app/${BINARY_FILE} 2>/dev/null) bytes"
else
    log "WARN: binary NOT FOUND at /opt/app/${BINARY_FILE}"
fi
log "checking UnityPlayer.so"
if [ -f /opt/app/UnityPlayer.so ]; then
    log "UnityPlayer.so exists, size: \$(stat -c%s /opt/app/UnityPlayer.so 2>/dev/null) bytes"
else
    log "WARN: UnityPlayer.so NOT FOUND"
fi
log "checking UnityPlayer.so dynamic deps"
MISSING=\$(ldd /opt/app/UnityPlayer.so 2>&1 | grep 'not found' || true)
if [ -n "\$MISSING" ]; then
    log "WARN: UnityPlayer.so has missing libs:"
    echo "\$MISSING" | while read line; do log "  \$line"; done
else
    log "UnityPlayer.so: all deps resolved"
fi
log "===== diag end ====="

log "launching Unity: ./${BINARY_FILE} -batchmode -nographics"
log "Output -> /tmp/unity.log (tail -f /tmp/unity.log from shell)"

cd /opt/app
"./${BINARY_FILE}" -batchmode -nographics -logFile - > /tmp/unity.log 2>&1 &
UNITY_PID=\$!
log "Unity launched as PID \$UNITY_PID"

setsid /usr/bin/dash -i </dev/console >/dev/console 2>&1 &
log "diagnostic shell PID \$! — press Enter for prompt"

(
    wait \$UNITY_PID
    UEXIT=\$?
    echo "[init] Unity exited code \$UEXIT — see /tmp/unity.log" > /dev/kmsg
) &

while true; do
    sleep 3600
done
INITEOF

echo "==> Building aarch64 rootfs via Docker buildx..."
docker buildx build --platform linux/arm64 --load \
    -t dgsvshs-unity-microvm:latest \
    -f Dockerfile.microvm .

echo "==> Exporting rootfs..."
CID=$(docker create --platform linux/arm64 dgsvshs-unity-microvm:latest)
rm -rf rootfs_dir
mkdir rootfs_dir
docker export "$CID" | tar -x -C rootfs_dir
docker rm "$CID" >/dev/null

mkdir -p rootfs_dir/lib/modules
cp modules/*.ko rootfs_dir/lib/modules/

echo "==> Packaging initramfs (this can take a while — Unity build is large)..."
cd rootfs_dir
find . | cpio -H newc -o 2>/dev/null | gzip -9 > ../initramfs.cpio.gz
cd ..

echo "==> Booting MicroVM via QEMU (UDP 7777 forwarded to host)..."
qemu-system-aarch64 \
  -machine virt,accel=hvf \
  -cpu host \
  -m 2048 \
  -kernel vmlinuz-virt \
  -initrd initramfs.cpio.gz \
  -append "console=ttyAMA0 cgroup_disable=memory,pids" \
  -nographic \
  -netdev user,id=net0,hostfwd=udp::7777-:7777 \
  -device virtio-net-pci,netdev=net0
