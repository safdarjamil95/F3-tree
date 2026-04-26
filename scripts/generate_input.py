#!/usr/bin/env python3

import argparse
import random


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate deterministic F3-tree input keys.")
    parser.add_argument("count", type=int, help="number of keys to generate")
    parser.add_argument("output", help="output file path")
    parser.add_argument("--seed", type=int, default=1, help="shuffle seed")
    args = parser.parse_args()

    keys = list(range(1, args.count + 1))
    random.Random(args.seed).shuffle(keys)

    with open(args.output, "w", encoding="utf-8") as handle:
        for key in keys:
            handle.write(f"{key}\n")


if __name__ == "__main__":
    main()
