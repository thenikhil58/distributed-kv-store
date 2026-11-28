#!/usr/bin/env python3
"""Run bounded benchmark matrices and emit environment/resource records as JSON Lines."""

import argparse
import json
import platform
from pathlib import Path
import re
import subprocess
import time
from urllib.request import urlopen


def metrics_snapshot(url):
    if not url:
        return None
    with urlopen(url, timeout=3) as response:
        text = response.read().decode("utf-8")
    values = {}
    for metric in ("raft_storage_bytes", "process_cpu_seconds_total",
                   "process_max_resident_memory_bytes"):
        matches = re.findall(
            rf"^{metric}(?:\{{[^}}]*\}})?\s+([0-9.eE+-]+)$",
            text, flags=re.MULTILINE,
        )
        values[metric] = [float(value) for value in matches]
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="./build/kvstore_benchmark")
    parser.add_argument("--target", default="localhost:50051")
    parser.add_argument("--cluster-size", type=int, required=True,
                        help="operator-supplied topology label; not auto-detected")
    parser.add_argument("--operations", type=int, default=1000)
    parser.add_argument("--workers", type=int, nargs="+", default=[1, 4, 16])
    parser.add_argument("--value-bytes", type=int, nargs="+", default=[16, 1024])
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--mode", choices=("set", "get", "mixed"),
                        nargs="+", default=["set", "get", "mixed"])
    parser.add_argument("--metrics-url", help="optional node Prometheus endpoint")
    parser.add_argument("--insecure-dev", action="store_true")
    parser.add_argument("--ca")
    parser.add_argument("--cert")
    parser.add_argument("--key")
    parser.add_argument("--token-file")
    parser.add_argument("--crl-directory")
    parser.add_argument("--output", default="-", help="JSONL file path or - for stdout")
    args = parser.parse_args()

    if not 1 <= args.cluster_size <= 9 or not 1 <= args.runs <= 100:
        parser.error("cluster-size must be 1..9 and runs must be 1..100")
    if not 1 <= args.operations <= 10_000_000:
        parser.error("operations must be 1..10000000")
    if any(worker < 1 or worker > 256 for worker in args.workers):
        parser.error("workers must be between 1 and 256")
    if any(value < 0 or value > 1024 * 1024 for value in args.value_bytes):
        parser.error("value sizes must be between 0 and 1048576")

    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"], check=False, capture_output=True, text=True
    )
    commit = revision.stdout.strip() if revision.returncode == 0 else "unknown"
    output = None if args.output == "-" else open(args.output, "a", encoding="utf-8")
    try:
        for run in range(args.runs):
            for mode in args.mode:
                for workers in args.workers:
                    for value_bytes in args.value_bytes:
                        prefix = f"bm{run}-{mode[0]}-{workers}-{value_bytes}/"
                        command = [
                            args.binary, args.target, str(args.operations),
                            "--workers", str(workers),
                            "--cluster-size", str(args.cluster_size),
                            "--value-bytes", str(value_bytes),
                            "--mode", mode,
                            "--key-prefix", prefix,
                        ]
                        for option, value in (
                            ("--ca", args.ca), ("--cert", args.cert),
                            ("--key", args.key), ("--token-file", args.token_file),
                            ("--crl-directory", args.crl_directory),
                        ):
                            if value:
                                command.extend((option, value))
                        if args.insecure_dev:
                            command.append("--insecure-dev")
                        before = metrics_snapshot(args.metrics_url)
                        started = time.time()
                        result = subprocess.run(
                            command, check=False, capture_output=True, text=True
                        )
                        after = metrics_snapshot(args.metrics_url)
                        if result.returncode != 0:
                            raise RuntimeError(
                                f"benchmark failed ({result.returncode}): "
                                f"{result.stderr.strip()}"
                            )
                        record = {
                            "recorded_unix_seconds": time.time(),
                            "run": run,
                            "commit": commit,
                            "platform": platform.platform(),
                            "target": args.target,
                            "cluster_size_label": args.cluster_size,
                            "setup_seconds_including_process_start": time.time() - started,
                            "resource_before": before,
                            "resource_after": after,
                            "result": json.loads(result.stdout),
                        }
                        line = json.dumps(record, sort_keys=True)
                        if output is None:
                            print(line, flush=True)
                        else:
                            output.write(line + "\n")
                            output.flush()
    finally:
        if output is not None:
            output.close()


if __name__ == "__main__":
    main()
