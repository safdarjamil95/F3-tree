#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "f3tree/config.h"

namespace f3tree {

// FutureBTree: the F3-tree concurrent B+-tree with per-producer PTFO buffering.
//
// Writes go into per-producer PTFO (Per-Thread Future Object) chains and are
// merged into the global B+-tree asynchronously by background evaluator threads.
// Range queries require draining all PTFOs first; see scan_range().

struct FutureBTreeStats {
  std::uint64_t checkpoint_count = 0;
  // async_checkpoint_count: background evaluator-driven checkpoints only;
  // excludes manual drain_pending() calls.
  std::uint64_t async_checkpoint_count = 0;
  std::uint64_t checkpoint_time_us = 0;
  std::uint64_t replayed_ops = 0;
  std::uint64_t replayed_nodes = 0;
  std::uint64_t placement_topology_nodes = 0;
  std::uint64_t bound_evaluator_threads = 0;
  std::uint64_t bound_producer_threads = 0;
  // hard_numa_memory_bound: producers for which set_mempolicy(MPOL_BIND)
  // succeeded. Non-zero only when PlacementPolicy::kHardNumaBind is active.
  std::uint64_t hard_numa_memory_bound = 0;
  // NUMA locality counters for producer enqueue operations.
  std::uint64_t producer_enqueue_local_ops = 0;
  std::uint64_t producer_enqueue_remote_ops = 0;
  std::uint64_t producer_enqueue_unknown_ops = 0;
  // NUMA locality counters for checkpoint drain operations.
  std::uint64_t checkpoint_source_local_ops = 0;
  std::uint64_t checkpoint_source_remote_ops = 0;
  std::uint64_t checkpoint_source_unknown_ops = 0;
};

class FutureBTree {
 public:
  explicit FutureBTree(const RuntimeConfig& config);
  ~FutureBTree();

  FutureBTree(const FutureBTree&) = delete;
  FutureBTree& operator=(const FutureBTree&) = delete;

  void warmup_insert(Key key);
  void buffered_insert(Key key, std::size_t producer_id);
  void erase_buffered(Key key, std::size_t producer_id);
  bool contains(Key key) const;

  bool global_contains(Key key);
  int buffered_owner(Key key) const;
  std::size_t pending_future_nodes(std::size_t producer_id) const;
  void drain_pending();
  FutureBTreeStats stats() const;

  // Drains all PTFOs then returns keys in [start, start+count) that exist
  // in the global tree. Results written into `out`; returns count found.
  std::size_t scan_range(Key start, int count, std::vector<Key> &out);

  const RuntimeConfig& config() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace f3tree
