import argparse
import paramiko
import os
import sys
from dotenv import load_dotenv

# Load the environment variables from the .env file
load_dotenv()

def main():
    parser = argparse.ArgumentParser(description="20Hz Proxmox Telemetry Recorder")
    parser.add_argument("--vmid", required=True, help="Target VM or LXC ID (e.g., 100)")
    parser.add_argument("--name", required=True, help="Name of the run (used for the output file)")
    parser.add_argument("--type", default="qemu", choices=["qemu", "lxc"], help="Type of guest")

    # Pull defaults dynamically from the .env file
    parser.add_argument("--host", default=os.getenv("PROXMOX_HOST", "192.168.1.100"), help="Proxmox IP Address")
    parser.add_argument("--user", default=os.getenv("PROXMOX_USER", "root"), help="SSH Username")
    parser.add_argument("--password", default=os.getenv("PROXMOX_PASSWORD"), help="SSH Password")

    parser.add_argument("--hz", type=int, default=20, help="Sampling frequency")
    args = parser.parse_args()

    if not args.password:
        print("[!] Error: PROXMOX_PASSWORD not found.")
        print("    Please set it in your .env file or pass it via --password.")
        sys.exit(1)

    remote_code = f"""
import time, json, sys, os, subprocess, select, re
vmid = "{args.vmid}"
vm_type = "{args.type}"
interval = 1.0 / {args.hz}

if vm_type == "qemu":
    cgroup_dir = f"/sys/fs/cgroup/qemu.slice/{{vmid}}.scope"
    net_iface = f"tap{{vmid}}i0"
else:
    cgroup_dir = f"/sys/fs/cgroup/lxc/{{vmid}}"
    net_iface = f"veth{{vmid}}i0"

# --- Serial console reader (qemu/VM only) ---
# Each microvm /init spawns an interactive sh on /dev/console; we attach to the
# QEMU serial socket via socat (same mechanism `qm terminal` uses) and send
# `cat /tmp/stats.log` per sample, capturing the JSON line each server writes
# at 20 Hz. LXC doesn't have a serial socket so the merge is skipped there.
serial_sock = f"/var/run/qemu-server/{{vmid}}.serial0"
console_proc = None
if vm_type == "qemu" and os.path.exists(serial_sock):
    try:
        console_proc = subprocess.Popen(
            ["socat", "-", f"UNIX-CONNECT:{{serial_sock}}"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, bufsize=0)
        # Wake the shell and drain its banner / motd.
        console_proc.stdin.write(b"\\n")
        console_proc.stdin.flush()
        time.sleep(0.3)
        try:
            while True:
                r, _, _ = select.select([console_proc.stdout], [], [], 0.05)
                if not r: break
                if not console_proc.stdout.read(4096): break
        except Exception:
            pass
    except Exception as e:
        sys.stderr.write(f"[recorder] could not open serial console: {{e}}\\n")
        console_proc = None

_json_re = re.compile(rb"\\{{\\s*\\\"t\\\".*?\\}}")

def grab_server_stats():
    if console_proc is None: return None
    try:
        console_proc.stdin.write(b"cat /tmp/stats.log\\n")
        console_proc.stdin.flush()
        deadline = time.time() + (interval * 0.5)
        buf = b""
        while time.time() < deadline:
            r, _, _ = select.select([console_proc.stdout], [], [], 0.01)
            if r:
                chunk = console_proc.stdout.read(4096)
                if chunk: buf += chunk
        m = None
        for hit in _json_re.finditer(buf):
            m = hit
        if m is None: return None
        return json.loads(m.group(0).decode("utf-8", errors="ignore"))
    except Exception:
        return None

def read_int(p):
    try:
        with open(p, 'r') as f: return int(f.read().strip())
    except: return 0

def get_cpu():
    try:
        with open(f"{{cgroup_dir}}/cpu.stat", 'r') as f:
            for line in f:
                if line.startswith("usage_usec"): return int(line.split()[1])
    except: pass
    return 0

def get_net():
    rx = read_int(f"/sys/class/net/{{net_iface}}/statistics/rx_bytes")
    tx = read_int(f"/sys/class/net/{{net_iface}}/statistics/tx_bytes")
    return rx, tx

last_time = time.time()
last_cpu = get_cpu()
last_rx, last_tx = get_net()

while True:
    t = time.time()
    dt = t - last_time
    if dt >= interval:
        cur_cpu = get_cpu()
        cur_rx, cur_tx = get_net()
        mem = read_int(f"{{cgroup_dir}}/memory.current")

        cpu_p = ((cur_cpu - last_cpu) / (dt * 1000000.0)) * 100.0
        rx_r = (cur_rx - last_rx) / dt
        tx_r = (cur_tx - last_tx) / dt

        sample = {{"t": t, "c": cpu_p, "m": mem, "rx": rx_r, "tx": tx_r}}
        srv = grab_server_stats()
        if srv:
            for k in ("inner_fps", "outer_fps", "to_spawn", "spawned", "alive", "tick", "state"):
                if k in srv: sample[k] = srv[k]

        print(json.dumps(sample))
        sys.stdout.flush()

        last_time, last_cpu, last_rx, last_tx = t, cur_cpu, cur_rx, cur_tx
    time.sleep(0.005)
"""

    os.makedirs("results", exist_ok=True)
    output_file = f"results/{args.name}.jsonl"

    print(f"[*] Starting {args.hz}Hz recording for VMID {args.vmid} ({args.type})")
    print(f"[*] Connecting to {args.host} as {args.user}...")

    # Initialize Paramiko SSH Client
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    try:
        ssh.connect(
            hostname=args.host,
            username=args.user,
            password=args.password,
            timeout=5
        )
        print("[*] Connection established. Injecting telemetry script...")

        # Execute python3 and keep standard streams open
        stdin, stdout, stderr = ssh.exec_command("python3 -")

        # Feed the script into the remote Python interpreter
        stdin.write(remote_code)
        stdin.flush()
        stdin.channel.shutdown_write() # Tell remote interpreter EOF is reached for input

        print(f"[*] Saving telemetry stream to: {output_file}")
        print("[*] Press CTRL+C to stop recording...\n")

        with open(output_file, 'a') as f:
            for line in iter(stdout.readline, ''):
                sys.stdout.write(f"\r[Live] Writing: {line.strip()}          ")
                sys.stdout.flush()
                f.write(line)

    except paramiko.AuthenticationException:
        print("\n[!] Authentication failed. Check your password.")
    except Exception as e:
        print(f"\n[!] SSH Error: {e}")
    except KeyboardInterrupt:
        print(f"\n\n[+] Recording stopped. Data saved to {output_file}")
    finally:
        # Ensure the SSH connection is cleanly severed
        ssh.close()

if __name__ == "__main__":
    main()