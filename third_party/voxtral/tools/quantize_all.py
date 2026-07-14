#!/usr/bin/env python3

import subprocess
import sys
import os

SUPPORTED_TYPES = [
    "Q4_0", "Q4_1", "Q5_0", "Q5_1", "Q8_0",
    "Q2_K", "Q3_K", "Q4_K", "Q5_K", "Q6_K",
    "Q4_K_M"
]

def find_executable():
    # Check project root build directory
    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    paths = [
        os.path.join(root_dir, "build", "voxtral-quantize"),
        os.path.join(root_dir, "build", "bin", "voxtral-quantize"),
        "voxtral-quantize" # Hope it's in PATH
    ]
    for p in paths:
        if os.path.exists(p) and os.access(p, os.X_OK):
            return p
        # Check if in PATH
        try:
            subprocess.run(["which", p], capture_output=True, check=True)
            return p
        except (subprocess.CalledProcessError, FileNotFoundError):
            continue
    return None

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <input_model.gguf> [nthreads]")
        sys.exit(1)

    input_model = sys.argv[1]
    nthreads = sys.argv[2] if len(sys.argv) > 2 else None

    if not os.path.exists(input_model):
        print(f"Error: Input model '{input_model}' not found.")
        sys.exit(1)

    executable = find_executable()
    if not executable:
        print("Error: 'voxtral-quantize' executable not found. Please build the project first.")
        sys.exit(1)

    base_name, _ = os.path.splitext(input_model)

    for qtype in SUPPORTED_TYPES:
        output_model = f"{base_name}-{qtype}.gguf"
        print(f"Quantizing {input_model} to {output_model} ({qtype})")
        
        cmd = [executable, input_model, output_model, qtype]
        if nthreads:
            cmd.append(nthreads)
        
        try:
            subprocess.run(cmd, check=True)
        except subprocess.CalledProcessError as e:
            print(f"Error: Quantization failed for {qtype} with exit code {e.returncode}")
            continue
        except KeyboardInterrupt:
            print("Quantization interrupted by user.")
            sys.exit(1)

    print("All quantizations completed.")

if __name__ == "__main__":
    main()
