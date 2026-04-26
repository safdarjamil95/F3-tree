#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace f3tree {

using Key = std::int64_t;

enum class EvaluatorPolicy {
  // F3-K: key-based evaluation — each evaluator consumes PTFO entries
  // sequentially per producer, partitioned across evaluator slots.
  // Assumes disjoint key ranges across producers; does not resolve
  // cross-producer conflicts.
  kF3K,
  // F3-N: node-based evaluation — each evaluator checkpoints complete PTFO
  // nodes as atomic units, ordered by node sequence. A drain-start snapshot
  // of the lookup directory suppresses stale operations.
  kF3N,

  // kDeterministicReplay: global cross-producer sort by operation sequence.
  // Deterministic and conflict-resolving but not part of the paper's design.
  kDeterministicReplay,
  // Aliases retained for source compatibility.
  kLegacyKeyPartitioned,   // = kF3K
  kLegacyNodePartitioned,  // = kF3N
};

enum class PlacementPolicy {
  kDefault,
  kBindEvaluatorsByNode,
  kBindEvaluatorsAndProducersByNode,
  // kHardNumaBind: combines CPU affinity with kernel-enforced NUMA memory
  // policy (set_mempolicy MPOL_BIND) per producer thread, guaranteeing PTFO
  // allocations land on the producer's home node. Requires Linux with NUMA
  // support (/sys/devices/system/node populated).
  kHardNumaBind,
};

struct RuntimeConfig {
  std::size_t producer_threads = 1;
  std::size_t evaluator_threads = 1;
  unsigned long write_latency_ns = 0;
  std::size_t hash_capacity = 1024;
  std::size_t checkpoint_threshold_ops = 0;
  std::uint64_t checkpoint_interval_us = 0;
  EvaluatorPolicy evaluator_policy = EvaluatorPolicy::kF3N;
  PlacementPolicy placement_policy = PlacementPolicy::kDefault;

  // PMDK persistence. Requires building with `make PMDK=1` (-DF3_PMDK).
  // When non-empty, opens or creates a libpmemobj pool at this path;
  // B+-tree pages and PTFO nodes survive process restart. Empty = DRAM only.
  std::string persistent_path;
  std::uint64_t persistent_pool_bytes = 128ull * 1024ull * 1024ull;
};

struct WorkloadConfig {
  std::size_t key_count = 1000;
  std::uint64_t seed = 1;
};

}  // namespace f3tree
