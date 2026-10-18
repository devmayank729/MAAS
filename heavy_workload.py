#!/usr/bin/env python3
"""MAAS Stress-Test & Heavy Workload Benchmark
Simulates ~4-5 minutes of continuous data science & linear algebra computing.
Prints live progress every 10 seconds so you can watch real-time execution.
"""

import sys
import os
import time
import platform
import math

print("=" * 65, flush=True)
print(f"🔥 HEAVY COMPUTE WORKLOAD STARTED", flush=True)
print(f"Node Host:       {platform.node()}", flush=True)
print(f"OS Platform:     {platform.platform()}", flush=True)
print(f"Python Exec:     {sys.executable}", flush=True)
print(f"Target Duration: ~4 Minutes (24 Rounds x ~10s)", flush=True)
print("=" * 65, flush=True)

TOTAL_ROUNDS = 24
start_time = time.time()

for round_num in range(1, TOTAL_ROUNDS + 1):
    round_start = time.time()
    
    # 1. Heavy Mathematical & Monte Carlo Simulation (CPU Bound)
    # Estimate Pi with 15 million iterations per round
    inside_circle = 0
    total_samples = 4_000_000
    
    # Using LCG pseudo-random generator in tight loop
    seed = (round_num * 1234567) & 0xFFFFFFFF
    for i in range(total_samples):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        x = seed / 0x7FFFFFFF
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        y = seed / 0x7FFFFFFF
        if x*x + y*y <= 1.0:
            inside_circle += 1
            
    pi_estimate = 4.0 * inside_circle / total_samples

    # 2. Matrix Multiplication Simulation (Floating Point Crunching)
    dim = 250
    matrix_a = [[((i + j + round_num) % 17) * 0.1 for j in range(dim)] for i in range(dim)]
    matrix_b = [[((i * j + round_num) % 19) * 0.1 for j in range(dim)] for i in range(dim)]
    
    # Transpose B for cache-friendly multiplication
    matrix_b_t = list(zip(*matrix_b))
    trace = 0.0
    for i in range(dim):
        row_a = matrix_a[i]
        for j in range(dim):
            dot = sum(x * y for x, y in zip(row_a, matrix_b_t[j]))
            if i == j:
                trace += dot

    round_elapsed = time.time() - round_start
    total_elapsed = time.time() - start_time
    avg_per_round = total_elapsed / round_num
    remaining_sec = max(0, int((TOTAL_ROUNDS - round_num) * avg_per_round))

    pct = int((round_num / TOTAL_ROUNDS) * 100)
    bar_filled = pct // 5
    bar = "█" * bar_filled + "░" * (20 - bar_filled)

    print(
        f"[{bar}] {pct:3d}% | Round {round_num:2d}/{TOTAL_ROUNDS:2d} | "
        f"Round Time: {round_elapsed:.1f}s | Elapsed: {int(total_elapsed)}s | "
        f"ETA: {remaining_sec}s | Pi: {pi_estimate:.5f} | Trace: {trace:.1f}",
        flush=True
    )

total_time = time.time() - start_time
print("\n" + "=" * 65, flush=True)
print(f"🎉 HEAVY WORKLOAD COMPLETE!", flush=True)
print(f"Total Computation Time: {total_time:.2f} seconds ({total_time/60:.2f} minutes)", flush=True)
print(f"Processed 96,000,000 Monte Carlo points + 24 Dense Matrix Multiplications", flush=True)
print("=" * 65, flush=True)
