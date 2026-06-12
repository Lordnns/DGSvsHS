# DGSvsHS

**Homogeneous vs. Heterogeneous Game Server — Benchmark Harness.**

Measures server-side performance for a fixed Unity gameplay workload across **three ECS-based server implementations**, all driven by the same Unity client binary:

| Leg | Stack | Transport | Port | Role |
|---|---|---|---|---|
| **DGS** (Build 1) | Unity DOTS / Entities (C#, server-as-Unity-headless) | NGO + UnityTransport (UDP) | 7777 | Baseline "what studios ship today, ECS-flavored" |
| **Arch** (Build 3) | C# Arch ECS + BepuPhysics, plain .NET (no Unity runtime) | QUIC (StirlingLabs.MsQuic) | 7778 | "Same language, escape the engine" |
| **Bevy** (Build 2) | Rust Bevy ECS + Avian physics, headless | QUIC (quiche / BoringSSL) | 4433 | "Same paradigm, change language + runtime" |

Only the **server** is benchmarked. The Unity client runs on a separate host so client load doesn't contaminate the measurement. The same client binary talks to all three servers via a per-build define (`HS_TARGET_DGS`, `HS_TARGET_ARCH`, `HS_TARGET_BEVY`); see `DGSvsHS/Assets/_Game/Editor/BuildModeSwitcher.cs`.

---

## Repo layout

```
DGSvsHS/                 Unity project (client for all three legs + DGS server)
csharp_arch_server/      C#/Arch headless server (no Unity)
rust/                    Rust/Bevy headless server (owned by VALERE91)
  ├─ cli/                Server entrypoint binary
  └─ gameplay/           Pure ECS sim + QUIC plugin
native/quic_client/      Rust QUIC cdylib (Unity loads this via DllImport for HS builds)
Build/                   Unity Editor build outputs (dated subfolders)
```

Each server leg ships **two microvm build scripts** at its root:

```
DGSvsHS/build_microvm_aarch64.sh           Unity DGS, aarch64
DGSvsHS/build_microvm_x86_64.sh            Unity DGS, x86_64
csharp_arch_server/build_microvm_aarch64.sh    Arch, aarch64
csharp_arch_server/build_microvm_x86_64.sh     Arch, x86_64
rust/build_microvm_aarch64.sh              Bevy, aarch64
rust/build_microvm_x86_64.sh               Bevy, x86_64
```

And the Arch leg has desktop publish scripts:

```
csharp_arch_server/scripts/publish_windows.ps1
csharp_arch_server/scripts/publish_linux.sh
```

And the Unity client has a native-plugin build helper:

```
native/quic_client/scripts/build_and_deploy.ps1
native/quic_client/scripts/build_and_deploy.sh
```

---

## 0. One-time prerequisites (the host you build on)

What you install depends on which scripts you intend to run. The matrix:

| You want to…                                      | Need |
|---------------------------------------------------|---|
| Build the Rust QUIC client DLL for Unity          | Rust 1.75+ stable (`rustup`) |
| Publish the Arch server for desktop (Win/Linux)   | .NET 10 SDK |
| Build any aarch64 microvm                         | Docker (with `buildx` + `binfmt_misc` for cross-arch) **OR** an aarch64 Linux host. `qemu-system-aarch64`, `cpio`, `gzip`, `curl` |
| Build any x86_64 microvm                          | Docker (`buildx`, native, no binfmt). `qemu-system-x86_64`, `cpio`, `gzip`, `curl` |
| Produce an uploadable ISO from any x86_64 microvm | `grub-pc-bin`, `grub-efi-amd64-bin`, `xorriso`, `mtools` |
| Build the Bevy server outside its microvm script  | `cargo zigbuild` (`pip install ziglang`, `cargo install cargo-zigbuild`), or run inside WSL — `boring-sys` will not compile on Windows |
| Build the Unity DGS server                        | Unity 6 with the Dedicated Server build module installed |

---

## 1. Unity client — build the native QUIC plugin (HS modes only)

The DGS mode uses NGO/UnityTransport and needs no native plugin. The **Arch and Bevy modes** use QUIC, which is implemented in `native/quic_client/` (Rust, `quinn` + `rustls`) and consumed by `DGSvsHS/Assets/_Game/Net/Quic/QuicNetworkClient.cs` through `[DllImport("dgsvshs_socket")]`.

You must build this cdylib once per host platform before pressing Play in any HS mode.

### 1.a Windows

```powershell
cd native/quic_client
.\scripts\build_and_deploy.ps1
```

What it does:
1. `cargo build --release --lib` in `native/quic_client/`.
2. Copies `target/release/dgsvshs_socket.dll` to `DGSvsHS/Assets/Plugins/x86_64/dgsvshs_socket.dll`.
3. Unity 6 picks it up on next editor focus (the Plugin Inspector should auto-tag it as Windows/x86_64).

### 1.b Linux / macOS

```bash
cd native/quic_client
./scripts/build_and_deploy.sh
```

Produces `libdgsvshs_socket.so` (Linux) or `libdgsvshs_socket.dylib` (macOS) into the same plugin folder. Same base name; Unity resolves by base name (`dgsvshs_socket`) so DllImport just works.

### 1.c Manual / cross-compile

```bash
cd native/quic_client
cargo build --release --lib
# Output: target/release/{dgsvshs_socket.dll | libdgsvshs_socket.so | libdgsvshs_socket.dylib}
```

Cross-compile (e.g. building a Linux .so from a Windows host):

```bash
cargo install cross
cross build --release --lib --target x86_64-unknown-linux-gnu
# Output: target/x86_64-unknown-linux-gnu/release/libdgsvshs_socket.so
```

You then place the output into the Unity `Assets/Plugins/<arch>/` folder for whichever Unity build target you plan to ship.

---

## 2. Unity DGS server — Editor build

The DGS leg is the Unity Dedicated Server itself; no separate publish script.

1. Open `DGSvsHS/` in Unity 6 (Dedicated Server module installed).
2. Project Settings → Player → **Scripting Define Symbols** must contain `WITH_DGS` for both *Windows* and *Windows Server* platform tabs (and also Linux Server if you build for that). This compiles NGO transport in, QUIC transport out. The setting is already saved in the project; re-confirm if you cloned fresh.
3. Switch build profile to `Server-Windows` (or `Server-Linux-x86_64` / `Server-Linux-ARM64`). Scene list must contain **only** `Assets/Scenes/Server.unity`.
4. Build → output to `Build/<date>/Server/<arch>/`. The folder must contain the entrypoint binary, `UnityPlayer.so` (or `.dll`), and `*_Data/`. The microvm scripts in §4.a expect this layout.
5. Run locally: `Build/<date>/Server/Windows/DGSvsHS.exe`. Watch for `[DedicatedServerMain] Listening on port 7777, seed C0FFEEF00D`.

(For the Unity **client** build, switch to a `Client-Windows` (etc.) profile with scene list `Assets/Scenes/Client.unity`. `WITH_DGS` define is the same project setting and is shared.)

---

## 3. C# Arch server — desktop publish

These are the standalone-desktop self-contained publishes. **Not used by the microvm path** — those scripts run their own `dotnet publish` against the musl runtime.

### 3.a Windows (`publish_windows.ps1`)

```powershell
cd csharp_arch_server
.\scripts\publish_windows.ps1                # normal build → publish/
.\scripts\publish_windows.ps1 -Run           # build, then launch the .exe
.\scripts\publish_windows.ps1 -GodMode       # build with GODMODE_DEFAULT define → publish-godmode/
.\scripts\publish_windows.ps1 -GodMode -Run  # both
```

Step-by-step the script does:
1. Kills any running `DGSvsHS.ArchServer` process so the .exe and its native DLLs aren't file-locked.
2. `dotnet publish -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeAllContentForSelfExtract=true` (the IncludeAllContentForSelfExtract flag is mandatory — StirlingLabs.MsQuic's static initializer reads `Assembly.Location`, which is empty under default single-file packing).
3. If `-GodMode`, adds `-p:GodModeDefault=true`. The csproj turns that into a `GODMODE_DEFAULT` define; `Program.cs` flips `Config.GodMode = true` by default. Output goes to a tagged `publish-godmode/` folder so normal + godmode builds coexist.
4. Outputs `publish[-godmode]/DGSvsHS.ArchServer.exe`. Double-clickable. Listens on UDP 7778.
5. If `-Run`, executes the freshly-built .exe in the same shell.

### 3.b Linux (`publish_linux.sh`)

```bash
cd csharp_arch_server
./scripts/publish_linux.sh                   # normal → publish/
./scripts/publish_linux.sh --run             # build, then exec
./scripts/publish_linux.sh --god-mode        # godmode → publish-godmode/
./scripts/publish_linux.sh --god-mode --run  # both
```

Same flow as the Windows script (`-r linux-x64` instead of `win-x64`). The csproj contains a post-build `DeployMsQuicNative` target that symlinks the bundled `libmsquic-openssl.so` (built against OpenSSL 1.1, absent on Ubuntu 24.04) to the apt-installed `libmsquic.so.2` (OpenSSL 3). Install msquic on the runtime host:

```bash
sudo apt install libmsquic
```

Outputs `publish[-godmode]/DGSvsHS.ArchServer`. Listens on UDP 7778.

---

## 4. MicroVM builds (all three legs)

Each leg produces the same two files into a leg-local `.microvm_<arch>/`:

- `vmlinuz-virt` — Alpine `linux-virt` kernel (~10 MB)
- `initramfs.cpio.gz` — rootfs + custom `/init` (size varies: Rust ~5 MB, Arch ~80 MB, Unity ~200+ MB)

All scripts bake a **static IP** into `/init`. Defaults: `192.168.0.205/24`, gateway `192.168.0.1`, DNS `8.8.8.8`. Override per build:

```bash
STATIC_IP=192.168.1.50 STATIC_GATEWAY=192.168.1.1 ./build_microvm_x86_64.sh
```

### 4.a Unity DGS microvm

**Prereqs:** A Unity Linux Dedicated Server build for the right architecture must already exist (run a Unity build first; see §2). Architecture must be ARM64 or x86_64 matching the script. Rootfs is Debian bookworm-slim (glibc — Unity's `UnityPlayer.so` won't run on musl).

```bash
cd DGSvsHS
./build_microvm_aarch64.sh                                     # auto-locate latest Build/<date>/Server/...
./build_microvm_aarch64.sh /abs/path/to/Build/.../LinuxServerARM64   # explicit dir

./build_microvm_x86_64.sh                                      # same, x86_64
./build_microvm_x86_64.sh /abs/path/to/Build/.../LinuxServerx86_64
```

What the script does:
1. Auto-locates (or accepts) a Unity build dir containing `UnityPlayer.so` + `*_Data/` + the entrypoint binary.
2. Downloads Alpine `linux-virt` apk for the target arch; extracts `vmlinuz-virt` + virtio_net modules.
3. Copies the Unity build tree into `.microvm_<arch>/unity_build/`.
4. Generates `Dockerfile.microvm` (Debian bookworm-slim base + all the system libs Unity dlopens at startup — libcurl, libssl3, libgssapi, libx11/xrandr/xcursor/asound stubs, etc.).
5. Generates `init.sh` (PID 1: mounts /dev /proc /sys /tmp, brings up eth0 with the static IP, loads virtio_net modules, launches `./<binary> -batchmode -nographics -logFile -` with output to `/tmp/unity.log`, then forks a diagnostic shell on the serial console).
6. `docker buildx build --platform linux/<arch>` builds the rootfs; `docker export` flattens it; `cpio -H newc | gzip -9` packages into `initramfs.cpio.gz`.
7. **aarch64 script**: boots immediately via `qemu-system-aarch64 -machine virt,accel=hvf` with UDP 7777 host-forwarded.
8. **x86_64 script**: additionally runs `grub-mkrescue` to produce `dgs-microvm.iso`. Hybrid BIOS+UEFI bootable. Upload via Proxmox UI → Storage → ISO Images → Upload, then create a VM with CD/DVD pointing at it.

### 4.b Arch microvm

Rootfs is Alpine 3.21 (musl). Pulls **libmsquic@edge** (msquic 2.5.7) because Alpine 3.21's 2.4.18 negotiates QUIC datagrams differently from what StirlingLabs.MsQuic 23.7.1 expects (handshake completes but `DatagramsAllowed = False` and snapshots never reach the client — see memory: `project_libmsquic_version_skew`).

```bash
cd csharp_arch_server
./build_microvm_aarch64.sh
./build_microvm_x86_64.sh
```

Step-by-step:
1. `dotnet publish -c Release -r linux-musl-<arch> --self-contained true -p:PublishSingleFile=true -p:IncludeAllContentForSelfExtract=true -o publish-microvm`.
2. Fetches kernel + modules (same Alpine apk as the Unity script).
3. Generates Dockerfile + init.sh. Dockerfile installs `musl libstdc++ libgcc icu-libs ca-certificates openssl libmsquic@edge`. Then it runs the binary for a few seconds to *force* .NET single-file extraction into `/opt/dotnet_extract/` (the binary will crash trying to load the bundled OpenSSL-1.1 libmsquic — fine, we only want the files extracted). Then it deletes the extracted `libmsquic-openssl.so` and symlinks it to `/usr/lib/libmsquic.so.2`. .NET single-file does an existence check on next run and skips re-extraction, so the symlink survives.
4. Builds rootfs via Docker buildx; packages initramfs.
5. **aarch64**: boots in QEMU with UDP 7778 forwarded.
6. **x86_64**: emits `arch-microvm.iso` via `grub-mkrescue` for Proxmox upload.

### 4.c Bevy microvm

```bash
cd rust
./build_microvm_aarch64.sh
./build_microvm_x86_64.sh
```

Step-by-step:
1. `cargo zigbuild --target <arch>-unknown-linux-musl --release` (requires `cargo-zigbuild`).
2. Fetches Alpine `linux-virt` kernel + virtio_net modules.
3. Copies `target/<arch>-unknown-linux-musl/release/cli` to `rootfs_dir/init`. No Dockerfile, no separate init.sh — the binary is `/init` directly.
4. `cpio -H newc | gzip -9` → `initramfs.cpio.gz`.
5. **aarch64**: boots in QEMU with UDP 4433 forwarded.
6. **x86_64**: also emits `bevy-microvm.iso` for Proxmox.

---

## 5. End-to-end smoke test — all three legs

Once you have the three things built (Unity client + native plugin, all three servers):

1. **Pick a leg in Unity.** Menu: `DGSvsHS/Build Mode → DGS | HS/Arch | HS/Bevy | BareBone`. The switcher rewrites scripting defines and patches `ClientMain.Port` in `Client.unity` so the Inspector matches.
2. **Start the server** for that leg (desktop publish or microvm — pick one; both behave identically over the wire).
3. **Run the Unity client** — Editor Play or built `Client-Windows.exe`. Default `Host = 127.0.0.1`; change to the server's IP for cross-host trials. WASD / mouse / LMB.
4. **Verify in the server log:**
   - DGS: `[DedicatedServerMain] Listening on port 7777, seed C0FFEEF00D`
   - Arch: `[QuicServer] listening on 0.0.0.0:7778`
   - Bevy: equivalent line on 4433
   - Then `Slot 0 assigned to client <connID>` and the round-1 countdown.
5. **For benchmark trials** (autopilot): see §10 of `CLAUDE.md`. The Unity client takes `--server <ip> --port <p> --bot-id 0..3 --seed N --duration <sec> --output trial.ndjson`.
---
