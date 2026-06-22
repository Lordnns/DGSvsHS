import os
import json
import argparse
import matplotlib.pyplot as plt


# Example 1: Passing just the base name
# python generate_single.py dgs_baseline_1

# Example 2: Passing the file with the extension
# python generate_single.py arch_stress_3.jsonl


# --- Configuration ---
RESULTS_DIR = "results"
OUTPUT_DIR = "graphs/single"

# Display configurations reused from the main script
METRICS = {
    'c': {'title': 'CPU Usage', 'ylabel': 'CPU Utilization (%)', 'scale': 1.0},
    'm': {'title': 'Memory Allocation', 'ylabel': 'Memory (MB)', 'scale': 1 / (1024 * 1024)},
    'rx': {'title': 'Network Ingress (Rx)', 'ylabel': 'Data Rate (KB/s)', 'scale': 1 / 1024},
    'tx': {'title': 'Network Egress (Tx)', 'ylabel': 'Data Rate (KB/s)', 'scale': 1 / 1024}
}
# ---------------------

def parse_single_file(filepath):
    """Reads a single JSONL file and extracts its metrics."""
    data = {m: [] for m in ['t', 'c', 'm', 'rx', 'tx']}
    
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
                for m in METRICS.keys():
                    data[m].append(row[m] * METRICS[m]['scale'])
            except (json.JSONDecodeError, KeyError):
                continue
                
    return data

def plot_single_graph(x_vals, y_vals, title, ylabel, filename, run_name):
    """Renders and saves a single SVG for one specific run."""
    plt.figure(figsize=(12, 6))
    
    if not y_vals:
        plt.close()
        return

    # Defaulting to a clean blue for single-run plots
    plt.plot(x_vals, y_vals, label=run_name, color='#1f77b4', linewidth=1.5, alpha=0.85)

    plt.title(title, fontsize=14, fontweight='bold')
    plt.xlabel("Elapsed Time (Seconds)", fontsize=11)
    plt.ylabel(ylabel, fontsize=11)
    plt.grid(True, linestyle=':', alpha=0.7)
    plt.legend(loc="upper right")
    plt.tight_layout()
    
    plt.savefig(filename, format='svg')
    plt.close()

def main():
    parser = argparse.ArgumentParser(description="Generate graphs for a single telemetry run.")
    parser.add_argument("name", help="The name of the run to graph (e.g., 'dgs_baseline_1' or 'dgs_baseline_1.jsonl')")
    args = parser.parse_args()

    # Ensure the .jsonl extension is present
    filename = args.name if args.name.endswith('.jsonl') else f"{args.name}.jsonl"
    filepath = os.path.join(RESULTS_DIR, filename)

    print(f"[*] Parsing data from {filepath}...")
    data = parse_single_file(filepath)
    
    if not data or not data['t']:
        print("[!] No valid data found or file is empty. Exiting.")
        return

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    clean_name = filename.replace('.jsonl', '')

    print(f"[*] Generating graphs for {clean_name}...")
    
    for metric, m_info in METRICS.items():
        x_vals = data['t']
        y_vals = data[metric]
        
        out_filename = f"{OUTPUT_DIR}/{clean_name}_{m_info['title'].replace(' ', '_')}.svg"
        plot_single_graph(x_vals, y_vals, f"{clean_name.upper()}: {m_info['title']}", m_info['ylabel'], out_filename, clean_name)
        
    print(f"[+] Complete! 4 graphs saved to the '{OUTPUT_DIR}/' folder.")

if __name__ == "__main__":
    main()