# F3-tree

This is the canonical open-source implementation of F3-tree, a concurrent B+-tree designed for high-throughput write workloads on multi-core systems.

F3-tree was proposed and designed by S. Jamil. The algorithm is described in:

> **F3-tree: A Novel Concurrent B+-tree with Per-Thread Future Objects**  
> S. Jamil — *Cluster Computing*, 2022

The included PDF (`Cluster_Computing_2022_S_Jamil.pdf`) is the published paper.

## Design

F3-tree decouples writes from the global tree structure. Each producer thread appends operations to its own PTFO (Per-Thread Future Object) chain rather than acquiring a global lock. Background evaluator threads drain PTFOs into the shared B+-tree asynchronously, enabling producer and evaluator work to overlap.

Two evaluator policies are provided:

- **F3-N** (default): node-based evaluation — drains complete PTFO nodes as atomic units, using a drain-start snapshot of the lookup directory to suppress stale operations.
- **F3-K**: key-based evaluation — partitions PTFO entries across evaluator slots by producer; assumes disjoint key ranges across producers.

## Layout

```
include/f3tree/   public API headers
src/              wrapper and utility implementations
core/             B+-tree core (btree.h, hash.h, bloom_filter.hpp, …)
bench/            benchmark harness programs
tests/            correctness and persistence tests
scripts/          workload generation and test drivers
YCSB-cpp/         YCSB-cpp with F3-tree backend (YCSB-cpp/f3tree/)
```

## Build

Requires Linux x86-64 and a C++11-compatible GCC toolchain.

```bash
make
```

Build outputs are written to `bin/`.

### PMDK (persistent memory) build

To enable the libpmemobj-backed persistent backend:

```bash
# One-time: install PMDK development packages
sudo apt install libpmem-dev libpmemobj-dev libpmemobj-cpp-dev

make PMDK=1
```

With `make PMDK=1`, setting `RuntimeConfig::persistent_path` to a non-empty path opens (or creates) a libpmemobj pool at that path. B+-tree pages and per-producer PTFO nodes live in the pool and survive process restart. When `persistent_path` is empty, the backend falls back to DRAM-only allocation.

Cross-process restart test:

```bash
make PMDK=1
./scripts/run_restart_test.sh
```

## Benchmarks

```bash
./bin/baseline_benchmark -n 100000 -t 4
./bin/future_benchmark   -n 100000 -t 4 -e 2
./bin/mixed_benchmark    -n 100000 -t 4 -e 2
./bin/hash_benchmark     -n 100000
```

All benchmarks generate keys deterministically by default. Use `-i <path>` to load keys from a file.

## YCSB

[YCSB-cpp](https://github.com/ls4154/YCSB-cpp) is included as a submodule under `YCSB-cpp/`. The F3-tree backend adapter lives in `YCSB-cpp/f3tree/`.

### Build

```bash
cd YCSB-cpp

# DRAM build
make BIND_F3TREE=1

# PMDK build (requires libpmem-dev / libpmemobj-dev / libpmemobj-cpp-dev)
make BIND_F3TREE=1 PMDK=1
```

### Run

```bash
# Workloads A–F (DRAM)
./ycsb -load -run -db f3tree -threads 4 \
  -P workloads/workloada \
  -P f3tree/f3tree.properties \
  -p recordcount=100000 \
  -p operationcount=100000 \
  -p f3tree.hash_capacity=131072

# PMDK: add a pool path; pool is created on first run and reopened on subsequent runs
./ycsb -load -run -db f3tree -threads 4 \
  -P workloads/workloada \
  -P f3tree/f3tree.properties \
  -p recordcount=100000 \
  -p operationcount=100000 \
  -p f3tree.hash_capacity=131072 \
  -p f3tree.persistent_path=/dev/shm/f3tree.pool \
  -p f3tree.persistent_pool_bytes=536870912
```

Supported workloads: A (50% read / 50% update), B (95/5 read/update), C (100% read), F (50% read / 50% read-modify-write), E (95% scan / 5% insert).

Workload E range scans drain all pending PTFO state into the global B+-tree before each scan. For scans to return populated results, add `-p insertorder=ordered` so consecutive YCSB key numbers map to consecutive sorted tree keys.

### Key configuration properties

| Property | Default | Description |
|---|---|---|
| `f3tree.evaluator_policy` | `f3n` | Evaluator mode: `f3n` (node-based) or `f3k` (key-based) |
| `f3tree.evaluator_threads` | `1` | Background evaluator thread count |
| `f3tree.hash_capacity` | `1048576` | Per-producer hashtable slots; set to ≥ `recordcount` |
| `f3tree.checkpoint_threshold_ops` | `0` | Auto-drain after N buffered ops per producer (0 = off) |
| `f3tree.checkpoint_interval_us` | `0` | Auto-drain every N microseconds (0 = off) |
| `f3tree.persistent_path` | _(empty)_ | Pool file path for PMDK build; empty = DRAM only |
| `f3tree.persistent_pool_bytes` | `536870912` | Pool size in bytes (used only on first creation) |

## Tests

```bash
make smoke
```

## Input generation

```bash
python3 scripts/generate_input.py 100000 generated/input.txt --seed 7
```

## Public API

```cpp
#include "f3tree/future_btree.h"

f3tree::RuntimeConfig cfg;
cfg.producer_threads  = 4;
cfg.evaluator_threads = 2;
cfg.hash_capacity     = 1 << 20;

f3tree::FutureBTree tree(cfg);

// Producer threads (each with a unique id in [0, producer_threads))
tree.buffered_insert(key, producer_id);
tree.erase_buffered(key, producer_id);

// Point lookup (checks hashtable, PTFO chain, then global tree)
bool found = tree.contains(key);

// Drain all pending state synchronously
tree.drain_pending();
```

For a persistent B+-tree without the PTFO buffering layer, use `f3tree::PersistentBTree`.

## License

This implementation is released under the MIT License. See [LICENSE](LICENSE) for details.
