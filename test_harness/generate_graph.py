import os
import json
import re
import numpy as np
import matplotlib.pyplot as plt
from collections import defaultdict

# --- Configuration ---
RESULTS_DIR = "results"
OUTPUT_DIR = "graphs"

# Fenêtre de lissage pour la moyenne glissante. 
# À 20Hz : 10 = 0.5s de lissage, 20 = 1s de lissage, 40 = 2s de lissage.
SMOOTHING_WINDOW = 10 

# Display configurations
METRICS = {
    'c': {'title': 'CPU Usage', 'ylabel': 'CPU Utilization (%)', 'scale': 1.0},
    'm': {'title': 'Memory Allocation', 'ylabel': 'Memory (MB)', 'scale': 1 / (1024 * 1024)},
    'rx': {'title': 'Network Ingress (Rx)', 'ylabel': 'Data Rate (KB/s)', 'scale': 1 / 1024},
    'tx': {'title': 'Network Egress (Tx)', 'ylabel': 'Data Rate (KB/s)', 'scale': 1 / 1024}
}
FLAVOR_COLORS = {'dgs': '#1f77b4', 'arch': '#ff7f0e', 'bevy': '#2ca02c'} # Blue, Orange, Green
# ---------------------

def parse_files():
    """Reads all JSONL files and organizes them by flavor and run number."""
    data = defaultdict(lambda: defaultdict(lambda: {m: [] for m in ['t', 'c', 'm', 'rx', 'tx']}))
    run_numbers = set()
    
    if not os.path.exists(RESULTS_DIR):
        print(f"[!] Directory '{RESULTS_DIR}' not found.")
        return None, None

    file_pattern = re.compile(r'(?i)(dgs|arch|bevy).*?_(\d+)\.jsonl$')
    
    for filename in os.listdir(RESULTS_DIR):
        match = file_pattern.match(filename)
        if match:
            flavor = match.group(1).lower()
            run_num = int(match.group(2))
            run_numbers.add(run_num)
            
            filepath = os.path.join(RESULTS_DIR, filename)
            with open(filepath, 'r') as f:
                start_time = None
                for line in f:
                    try:
                        row = json.loads(line.strip())
                        if start_time is None:
                            start_time = row['t']
                        
                        data[flavor][run_num]['t'].append(row['t'] - start_time)
                        for m in METRICS.keys():
                            data[flavor][run_num][m].append(row[m] * METRICS[m]['scale'])
                    except (json.JSONDecodeError, KeyError):
                        continue
                        
    return data, sorted(list(run_numbers))

def smooth_data(y, window_size):
    """Applique une moyenne glissante simple en utilisant numpy."""
    if window_size < 2 or len(y) < window_size:
        return y
    # L'utilisation de mode='same' garantit que la longueur du tableau reste identique
    return np.convolve(y, np.ones(window_size)/window_size, mode='same')

def plot_graph(x_data_dict, y_data_dict, title, ylabel, filename):
    """Utility to render and save a single SVG."""
    plt.figure(figsize=(12, 6))
    
    has_data = False
    for flavor, y_vals in y_data_dict.items():
        if y_vals is not None and len(y_vals) > 0:
            x_vals = x_data_dict[flavor]
            color = FLAVOR_COLORS.get(flavor, '#333333')
            
            # Application du lissage avant de tracer
            y_smoothed = smooth_data(y_vals, SMOOTHING_WINDOW)
            
            plt.plot(x_vals, y_smoothed, label=flavor.upper(), color=color, linewidth=1.5, alpha=0.85)
            has_data = True
            
    if not has_data:
        plt.close()
        return

    plt.title(title, fontsize=14, fontweight='bold')
    plt.xlabel("Elapsed Time (Seconds)", fontsize=11)
    plt.ylabel(ylabel, fontsize=11)
    plt.grid(True, linestyle=':', alpha=0.7)
    plt.legend(loc="upper right")
    plt.tight_layout()
    
    plt.savefig(filename, format='svg')
    plt.close()

def build_direct_averages(data):
    """Calculates pure mathematical averages for uniform high-frequency runs."""
    avg_data = defaultdict(dict)
    time_grids = {}
    
    for flavor in data.keys():
        run_nums = list(data[flavor].keys())
        if not run_nums:
            continue
            
        # Find the minimum array length across all runs for this flavor
        # (This protects against a run missing a single 50ms tick at the very end)
        min_len = min(len(data[flavor][r]['t']) for r in run_nums)
        
        # Since times are uniform, we just use the first run's timeline truncated to min_len
        time_grids[flavor] = data[flavor][run_nums[0]]['t'][:min_len]
        
        for metric in METRICS.keys():
            # Grab the arrays, truncate them to the exact same length, and stack them
            arrays = [data[flavor][r][metric][:min_len] for r in run_nums]
            stacked_data = np.vstack(arrays)
            
            # Calculate the pure average straight down the columns
            avg_data[flavor][metric] = np.mean(stacked_data, axis=0)
                
    return time_grids, avg_data

def main():
    print("[*] Parsing JSONL files from results/...")
    data, run_numbers = parse_files()
    
    if not data:
        print("[!] No matching data found. Exiting.")
        return

    os.makedirs(f"{OUTPUT_DIR}/averages", exist_ok=True)
    os.makedirs(f"{OUTPUT_DIR}/individual", exist_ok=True)
    
    flavors = list(data.keys())
    print(f"[*] Found flavors: {flavors}")
    print(f"[*] Found runs: {run_numbers}")

    # ---------------------------------------------------------
    # 1. GENERATE AVERAGES (4 Graphs)
    # ---------------------------------------------------------
    print("[*] Calculating direct column averages for uniform runs...")
    time_grids, avg_data = build_direct_averages(data)
    
    for metric, m_info in METRICS.items():
        x_dict = {f: time_grids[f] for f in flavors if f in time_grids}
        y_dict = {f: avg_data[f].get(metric) for f in flavors if metric in avg_data.get(f, {})}
        
        filename = f"{OUTPUT_DIR}/averages/Avg_{m_info['title'].replace(' ', '_')}.svg"
        plot_graph(x_dict, y_dict, f"AVERAGE: {m_info['title']} (10 Runs)", m_info['ylabel'], filename)
    
    print("[+] Generated 4 Average Graphs.")

    # ---------------------------------------------------------
    # 2. GENERATE INDIVIDUAL RUN COMPARISONS (40 Graphs)
    # ---------------------------------------------------------
    count = 0
    for run_num in run_numbers:
        for metric, m_info in METRICS.items():
            x_dict = {}
            y_dict = {}
            
            for flavor in flavors:
                if run_num in data[flavor]:
                    x_dict[flavor] = data[flavor][run_num]['t']
                    y_dict[flavor] = data[flavor][run_num][metric]
            
            filename = f"{OUTPUT_DIR}/individual/Run_{run_num}_{m_info['title'].replace(' ', '_')}.svg"
            plot_graph(x_dict, y_dict, f"RUN {run_num}: {m_info['title']} Comparison", m_info['ylabel'], filename)
            count += 1
            
    print(f"[+] Generated {count} Individual Run Graphs.")
    print(f"\n[*] Complete! All {count + 4} graphs saved to the '{OUTPUT_DIR}/' folder.")

if __name__ == "__main__":
    main()