import os
import json
import argparse
import matplotlib.pyplot as plt

# --- Configuration ---
RESULTS_DIR = "results"
OUTPUT_DIR = "graphs/combined"

# Configuration for the two specific metrics we want to combine
METRICS = {
    'c': {'title': 'CPU Usage', 'ylabel': 'CPU Utilization (%)', 'scale': 1.0, 'color': '#1f77b4'},      # Blue
    'tx': {'title': 'Network Egress (Tx)', 'ylabel': 'Data Rate (KB/s)', 'scale': 1 / 1024, 'color': '#ff7f0e'} # Orange
}
# ---------------------

def parse_single_file(filepath):
    """Reads a single JSONL file and extracts its metrics."""
    data = {m: [] for m in ['t', 'c', 'tx']}
    
    if not os.path.exists(filepath):
        print(f"[!] File not found: {filepath}")
        return None

    with open(filepath, 'r') as f:
        start_time = None
        for line in f:
            try:
                row = json.loads(line.strip())
                if start_time is None:
                    start_time = row['t']
                
                data['t'].append(row['t'] - start_time)
                data['c'].append(row['c'] * METRICS['c']['scale'])
                data['tx'].append(row['tx'] * METRICS['tx']['scale'])
            except (json.JSONDecodeError, KeyError):
                continue
                
    return data

def plot_combined_graph(x_vals, cpu_vals, tx_vals, title, filename):
    """Renders and saves a single SVG with dual Y-axes for CPU and Tx."""
    fig, ax1 = plt.subplots(figsize=(12, 6))
    
    if not cpu_vals or not tx_vals:
        plt.close()
        return

    # --- Primary Y-Axis (Left) for CPU ---
    color_cpu = METRICS['c']['color']
    ax1.set_xlabel("Elapsed Time (Seconds)", fontsize=11)
    ax1.set_ylabel(METRICS['c']['ylabel'], color=color_cpu, fontsize=11, fontweight='bold')
    line_cpu = ax1.plot(x_vals, cpu_vals, label="CPU (%)", color=color_cpu, linewidth=1.5, alpha=0.85)
    ax1.tick_params(axis='y', labelcolor=color_cpu)
    ax1.grid(True, linestyle=':', alpha=0.7)

    # --- Secondary Y-Axis (Right) for Network Tx ---
    ax2 = ax1.twinx()  # Create a twin axis sharing the same X-axis
    color_tx = METRICS['tx']['color']
    ax2.set_ylabel(METRICS['tx']['ylabel'], color=color_tx, fontsize=11, fontweight='bold')
    line_tx = ax2.plot(x_vals, tx_vals, label="Tx (KB/s)", color=color_tx, linewidth=1.5, alpha=0.85)
    ax2.tick_params(axis='y', labelcolor=color_tx)

    # --- Combine Legends ---
    lines = line_cpu + line_tx
    labels = [l.get_label() for l in lines]
    ax1.legend(lines, labels, loc="upper right")

    plt.title(title, fontsize=14, fontweight='bold')
    fig.tight_layout()  # Ensures the right-side labels don't get cut off
    
    plt.savefig(filename, format='svg')
    plt.close()

def main():
    parser = argparse.ArgumentParser(description="Generate a combined CPU/Tx graph for a single run.")
    parser.add_argument("name", help="The name of the run to graph (e.g., 'dgs_baseline_1' or 'dgs_baseline_1.jsonl')")
    args = parser.parse_args()

    # Ensure the .jsonl extension is present
    filename = args.name if args.name.endswith('.jsonl') else f"{args.name}.jsonl"
    filepath = os.path.join(RESULTS_DIR, filename)

    print(f"[*] Parsing CPU and Tx data from {filepath}...")
    data = parse_single_file(filepath)
    
    if not data or not data['t']:
        print("[!] No valid data found or file is empty. Exiting.")
        return

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    clean_name = filename.replace('.jsonl', '')

    out_filename = f"{OUTPUT_DIR}/{clean_name}_Combined_CPU_and_Tx.svg"
    print(f"[*] Generating combined graph for {clean_name}...")
    
    plot_combined_graph(
        x_vals=data['t'], 
        cpu_vals=data['c'], 
        tx_vals=data['tx'], 
        title=f"{clean_name.upper()}: CPU Usage vs Network Egress", 
        filename=out_filename
    )
        
    print(f"[+] Complete! Graph saved to: {out_filename}")

if __name__ == "__main__":
    main()