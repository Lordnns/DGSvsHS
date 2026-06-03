#!/usr/bin/env bash
# Usage:
#   ./scripts/publish_linux.sh                  # publish to publish/
#   ./scripts/publish_linux.sh --run            # …then launch in this shell
#   ./scripts/publish_linux.sh --god-mode       # publish to publish-godmode/ with GODMODE_DEFAULT
#   ./scripts/publish_linux.sh --god-mode --run # both

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

run_after=0
god_mode=0
for arg in "$@"; do
    case "$arg" in
        --run) run_after=1 ;;
        --god-mode) god_mode=1 ;;
        *) echo "[publish] unknown arg: $arg" >&2; exit 2 ;;
    esac
done

# Tagged output dir so godmode and normal builds can coexist on disk.
flavor_suffix=""
if [[ $god_mode -eq 1 ]]; then flavor_suffix="-godmode"; fi
publish_dir="$project_root/publish$flavor_suffix"
published_bin="$publish_dir/DGSvsHS.ArchServer"

# ---------- 1. Stop any running server so the binary isn't busy. ----------
if pgrep -x DGSvsHS.ArchServer >/dev/null 2>&1; then
    echo "[publish] stopping running ArchServer process(es)…"
    pkill -x DGSvsHS.ArchServer || true
    sleep 0.3
fi

# ---------- 2. Publish (which triggers a build first). ----------
echo "[publish] dotnet publish -c Release -r linux-x64 --self-contained true -p:PublishSingleFile=true"
cd "$project_root"
# IncludeAllContentForSelfExtract is required because StirlingLabs.MsQuic's static initializer
# reads `typeof(MsQuic).Assembly.Location` — empty under the default single-file packing
# (managed assemblies stay in the bundle). Self-extracting ALL content guarantees Location
# points to a real on-disk path and libmsquic-openssl.so ends up next to it.
msbuild_props=(
    -p:PublishSingleFile=true
    -p:IncludeAllContentForSelfExtract=true
)
if [[ $god_mode -eq 1 ]]; then
    echo "[publish] flavor: godmode (defining GODMODE_DEFAULT)"
    msbuild_props+=(-p:GodModeDefault=true)
fi

dotnet publish -c Release \
               -r linux-x64 \
               --self-contained true \
               "${msbuild_props[@]}" \
               -o "$publish_dir"

echo ""
echo "[done] $published_bin"
echo "       (run with ./DGSvsHS.ArchServer; 62.5 Hz tick loop, heartbeat once per second)"

# ---------- 3. Optionally launch the freshly-built binary. ----------
if [[ $run_after -eq 1 ]]; then
    echo ""
    echo "[run] launching $published_bin …"
    exec "$published_bin"
fi
