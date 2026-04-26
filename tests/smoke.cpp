#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "f3tree/future_btree.h"
#include "f3tree/persistent_btree.h"

namespace {

f3tree::RuntimeConfig make_future_config(
    std::size_t producer_threads,
    std::size_t evaluator_threads,
    std::size_t hash_capacity,
    f3tree::EvaluatorPolicy evaluator_policy) {
  f3tree::RuntimeConfig config;
  config.producer_threads = producer_threads;
  config.evaluator_threads = evaluator_threads;
  config.hash_capacity = hash_capacity;
  config.evaluator_policy = evaluator_policy;
  return config;
}

void expect_out_of_range_on_invalid_producer(f3tree::FutureBTree& tree) {
  bool insert_failed = false;
  try {
    tree.buffered_insert(999, 99);
  } catch (const std::out_of_range&) {
    insert_failed = true;
  }
  assert(insert_failed);

  bool erase_failed = false;
  try {
    tree.erase_buffered(999, 99);
  } catch (const std::out_of_range&) {
    erase_failed = true;
  }
  assert(erase_failed);
}

void test_persistent_btree_baseline() {
  f3tree::RuntimeConfig config;
  config.producer_threads = 2;
  config.evaluator_threads = 1;
  config.hash_capacity = 128;

  f3tree::PersistentBTree tree(config);
  for (f3tree::Key key = 1; key <= 32; ++key) {
    tree.insert(key);
  }
  for (f3tree::Key key = 1; key <= 32; ++key) {
    assert(tree.contains(key));
  }

  tree.erase(8);
  assert(!tree.contains(8));
}

void test_future_btree_buffered_visibility() {
  f3tree::FutureBTree tree(
      make_future_config(2, 1, 256, f3tree::EvaluatorPolicy::kDeterministicReplay));
  for (f3tree::Key key = 1; key <= 35; ++key) {
    tree.buffered_insert(key, 0);
  }
  tree.buffered_insert(1001, 1);

  assert(tree.contains(5));
  assert(tree.contains(1001));
  assert(!tree.contains(5000));
  assert(tree.buffered_owner(5) == 0);
  assert(tree.buffered_owner(1001) == 1);
  assert(tree.pending_future_nodes(0) >= 2);
  assert(tree.pending_future_nodes(1) == 1);
  assert(!tree.global_contains(5));

  tree.erase_buffered(5, 1);
  assert(!tree.contains(5));
  assert(tree.buffered_owner(5) == 1);
  assert(!tree.global_contains(5));

  tree.drain_pending();

  assert(tree.global_contains(1));
  assert(tree.global_contains(10));
  assert(tree.global_contains(1001));
  assert(tree.contains(1));
  assert(tree.contains(10));
  assert(tree.contains(1001));
  assert(!tree.contains(5));
  assert(tree.pending_future_nodes(0) == 0);
  assert(tree.pending_future_nodes(1) == 0);
  assert(tree.buffered_owner(5) == -1);
}

void test_future_btree_duplicate_operation_ordering() {
  f3tree::FutureBTree tree(
      make_future_config(3, 1, 64, f3tree::EvaluatorPolicy::kDeterministicReplay));
  tree.warmup_insert(200);
  tree.warmup_insert(201);

  tree.buffered_insert(50, 0);
  tree.buffered_insert(50, 1);
  assert(tree.contains(50));
  assert(tree.buffered_owner(50) == 1);

  tree.buffered_insert(60, 0);
  tree.erase_buffered(60, 1);
  assert(!tree.contains(60));
  assert(tree.buffered_owner(60) == 1);

  tree.erase_buffered(70, 0);
  tree.buffered_insert(70, 2);
  assert(tree.contains(70));
  assert(tree.buffered_owner(70) == 2);

  tree.erase_buffered(80, 0);
  tree.buffered_insert(80, 1);
  tree.erase_buffered(80, 2);
  assert(!tree.contains(80));
  assert(tree.buffered_owner(80) == 2);

  tree.erase_buffered(200, 1);
  assert(!tree.contains(200));
  assert(tree.global_contains(200));
  tree.buffered_insert(200, 2);
  assert(tree.contains(200));
  assert(tree.buffered_owner(200) == 2);

  tree.erase_buffered(201, 0);
  assert(!tree.contains(201));
  assert(tree.global_contains(201));

  tree.erase_buffered(9999, 0);
  assert(!tree.contains(9999));
  assert(tree.buffered_owner(9999) == 0);

  expect_out_of_range_on_invalid_producer(tree);

  tree.drain_pending();

  assert(!tree.contains(60));
  assert(!tree.contains(80));
  assert(!tree.contains(201));
  assert(!tree.contains(9999));

  assert(tree.pending_future_nodes(0) == 0);
  assert(tree.pending_future_nodes(1) == 0);
  assert(tree.pending_future_nodes(2) == 0);

  assert(tree.buffered_owner(50) == -1);
  assert(tree.buffered_owner(60) == -1);
  assert(tree.buffered_owner(70) == -1);
  assert(tree.buffered_owner(80) == -1);
  assert(tree.buffered_owner(200) == -1);
  assert(tree.buffered_owner(201) == -1);
  assert(tree.buffered_owner(9999) == -1);

  tree.drain_pending();

  assert(!tree.contains(60));
  assert(!tree.contains(80));
  assert(!tree.contains(201));
}

void test_future_btree_empty_and_repeated_drain() {
  f3tree::FutureBTree tree(
      make_future_config(1, 1, 1, f3tree::EvaluatorPolicy::kDeterministicReplay));
  assert(!tree.contains(1));
  assert(tree.pending_future_nodes(0) == 0);

  tree.drain_pending();
  assert(!tree.contains(1));
  assert(tree.pending_future_nodes(0) == 0);

  tree.erase_buffered(1, 0);
  assert(!tree.contains(1));
  assert(tree.buffered_owner(1) == 0);

  tree.drain_pending();
  assert(!tree.contains(1));
  assert(tree.pending_future_nodes(0) == 0);

  tree.drain_pending();
  assert(!tree.contains(1));
}

void test_future_btree_async_checkpointing() {
  f3tree::RuntimeConfig config;
  config.producer_threads = 2;
  config.evaluator_threads = 2;
  config.hash_capacity = 64;
  config.evaluator_policy = f3tree::EvaluatorPolicy::kDeterministicReplay;
  config.checkpoint_threshold_ops = 2;
  config.checkpoint_interval_us = 1000;

  f3tree::FutureBTree tree(config);
  tree.buffered_insert(41, 0);
  tree.buffered_insert(42, 1);

  bool drained = false;
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (tree.global_contains(41) && tree.global_contains(42) &&
        tree.pending_future_nodes(0) == 0 && tree.pending_future_nodes(1) == 0) {
      drained = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  assert(drained);
  assert(tree.contains(41));
  assert(tree.contains(42));
  assert(tree.buffered_owner(41) == -1);
  assert(tree.buffered_owner(42) == -1);
  const f3tree::FutureBTreeStats stats = tree.stats();
  assert(stats.async_checkpoint_count > 0);
  assert(stats.checkpoint_source_local_ops +
             stats.checkpoint_source_remote_ops +
             stats.checkpoint_source_unknown_ops >=
         2);
}

// Tests correctness for policies that resolve cross-producer conflicts:
//   kDeterministicReplay — global-sort path
//   kF3N / kLegacyNodePartitioned — node-ordered replay with latest-state filter
// kF3K / kLegacyKeyPartitioned are intentionally excluded: they assume
// disjoint key ranges per producer and do not resolve cross-producer
// conflicts. For conflicting workloads their result is undefined.
void run_conflict_resolving_policy_case(f3tree::EvaluatorPolicy policy) {
  f3tree::FutureBTree tree(make_future_config(4, 2, 128, policy));

  tree.warmup_insert(500);
  tree.buffered_insert(10, 0);
  tree.buffered_insert(11, 1);
  tree.buffered_insert(12, 2);
  tree.buffered_insert(13, 3);
  tree.erase_buffered(11, 0);   // cross-producer: producer 0 deletes key owned by producer 1
  tree.erase_buffered(500, 1);  // cross-producer: producer 1 deletes warmup key
  tree.buffered_insert(500, 2); // cross-producer: producer 2 re-inserts
  tree.erase_buffered(700, 3);

  assert(tree.contains(10));
  assert(!tree.contains(11));
  assert(tree.contains(12));
  assert(tree.contains(13));
  assert(tree.contains(500));
  assert(!tree.contains(700));

  tree.drain_pending();

  assert(tree.contains(10));
  assert(!tree.contains(11));
  assert(tree.contains(12));
  assert(tree.contains(13));
  assert(tree.contains(500));
  assert(!tree.contains(700));
  assert(tree.buffered_owner(10) == -1);
  assert(tree.buffered_owner(11) == -1);
  assert(tree.buffered_owner(12) == -1);
  assert(tree.buffered_owner(13) == -1);
  assert(tree.buffered_owner(500) == -1);
  assert(tree.buffered_owner(700) == -1);
}

// Tests all policies on a non-conflicting workload where each producer
// exclusively owns its key range. Verifies that kF3K (and its alias
// kLegacyKeyPartitioned) produce correct results when producers write
// disjoint key ranges.
void run_nonconflicting_policy_case(f3tree::EvaluatorPolicy policy) {
  f3tree::FutureBTree tree(make_future_config(4, 2, 128, policy));

  // Each producer writes exclusively to its own key range — no cross-producer
  // conflicts (disjoint key ranges per producer).
  tree.buffered_insert(100, 0);
  tree.buffered_insert(101, 0);
  tree.erase_buffered(101, 0);  // producer 0 inserts then deletes its own key

  tree.buffered_insert(200, 1);
  tree.buffered_insert(201, 1);

  tree.buffered_insert(300, 2);
  tree.erase_buffered(300, 2);  // producer 2 inserts then deletes its own key
  tree.buffered_insert(301, 2);

  tree.buffered_insert(400, 3);

  // Pre-drain visibility
  assert(tree.contains(100));
  assert(!tree.contains(101));
  assert(tree.contains(200));
  assert(tree.contains(201));
  assert(!tree.contains(300));
  assert(tree.contains(301));
  assert(tree.contains(400));

  tree.drain_pending();

  // Post-drain: same state expected from all policies
  assert(tree.contains(100));
  assert(!tree.contains(101));
  assert(tree.contains(200));
  assert(tree.contains(201));
  assert(!tree.contains(300));
  assert(tree.contains(301));
  assert(tree.contains(400));
  assert(tree.buffered_owner(100) == -1);
  assert(tree.buffered_owner(200) == -1);
  assert(tree.buffered_owner(301) == -1);
  assert(tree.buffered_owner(400) == -1);
}

void test_future_btree_evaluator_policies() {
  // Conflict-resolving policies: correct for cross-producer conflicting ops.
  // kDeterministicReplay uses a global sort.
  // kF3N uses node-ordered replay with a latest-state snapshot filter.
  run_conflict_resolving_policy_case(f3tree::EvaluatorPolicy::kDeterministicReplay);
  run_conflict_resolving_policy_case(f3tree::EvaluatorPolicy::kF3N);
  run_conflict_resolving_policy_case(f3tree::EvaluatorPolicy::kLegacyNodePartitioned);

  // Non-conflicting workload (disjoint key ranges per producer): all policies
  // must produce the same result. kF3K assumes disjoint key ranges per
  // producer and is correct for this workload class.
  run_nonconflicting_policy_case(f3tree::EvaluatorPolicy::kDeterministicReplay);
  run_nonconflicting_policy_case(f3tree::EvaluatorPolicy::kF3K);
  run_nonconflicting_policy_case(f3tree::EvaluatorPolicy::kF3N);
  run_nonconflicting_policy_case(f3tree::EvaluatorPolicy::kLegacyKeyPartitioned);
  run_nonconflicting_policy_case(f3tree::EvaluatorPolicy::kLegacyNodePartitioned);

  // Verify policy names round-trip through config().
  {
    f3tree::FutureBTree tree(
        make_future_config(2, 2, 32, f3tree::EvaluatorPolicy::kF3K));
    assert(tree.config().evaluator_policy == f3tree::EvaluatorPolicy::kF3K);
    tree.drain_pending();
    assert(!tree.contains(1));
  }

  {
    f3tree::FutureBTree tree(
        make_future_config(2, 2, 32, f3tree::EvaluatorPolicy::kF3N));
    assert(tree.config().evaluator_policy == f3tree::EvaluatorPolicy::kF3N);
    tree.drain_pending();
    assert(!tree.contains(1));
  }

  {
    f3tree::FutureBTree tree(
        make_future_config(2, 2, 32, f3tree::EvaluatorPolicy::kLegacyKeyPartitioned));
    assert(tree.config().evaluator_policy ==
           f3tree::EvaluatorPolicy::kLegacyKeyPartitioned);
    tree.drain_pending();
    assert(!tree.contains(1));
  }

  {
    f3tree::FutureBTree tree(
        make_future_config(2, 2, 32, f3tree::EvaluatorPolicy::kLegacyNodePartitioned));
    assert(tree.config().evaluator_policy ==
           f3tree::EvaluatorPolicy::kLegacyNodePartitioned);
    tree.drain_pending();
    assert(!tree.contains(1));
  }
}

void test_future_btree_numa_placement_policy() {
  f3tree::RuntimeConfig config;
  config.producer_threads = 2;
  config.evaluator_threads = 2;
  config.hash_capacity = 64;
  config.evaluator_policy = f3tree::EvaluatorPolicy::kDeterministicReplay;
  config.placement_policy = f3tree::PlacementPolicy::kBindEvaluatorsByNode;

  f3tree::FutureBTree tree(config);
  tree.buffered_insert(1, 0);
  tree.buffered_insert(2, 1);
  tree.drain_pending();

  const f3tree::FutureBTreeStats stats = tree.stats();
  assert(stats.placement_topology_nodes >= 1);
  assert(stats.bound_evaluator_threads <= config.evaluator_threads);
}

void test_future_btree_hard_numa_bind_policy() {
  f3tree::RuntimeConfig config;
  config.producer_threads = 2;
  config.evaluator_threads = 2;
  config.hash_capacity = 64;
  config.evaluator_policy = f3tree::EvaluatorPolicy::kDeterministicReplay;
  config.placement_policy = f3tree::PlacementPolicy::kHardNumaBind;

  f3tree::FutureBTree tree(config);
  tree.buffered_insert(101, 0);
  tree.buffered_insert(202, 1);
  tree.drain_pending();

  const f3tree::FutureBTreeStats stats = tree.stats();
  assert(stats.placement_topology_nodes >= 1);
  assert(stats.bound_evaluator_threads <= config.evaluator_threads);
  assert(stats.bound_producer_threads <= config.producer_threads);
  assert(stats.hard_numa_memory_bound <= config.producer_threads);
  assert(stats.producer_enqueue_local_ops +
             stats.producer_enqueue_remote_ops +
             stats.producer_enqueue_unknown_ops ==
         2);
  assert(stats.checkpoint_source_local_ops +
             stats.checkpoint_source_remote_ops +
             stats.checkpoint_source_unknown_ops >=
         2);
}

// Insert/delete/insert on the same key: final visible state must be "present".
void test_insert_delete_insert_ordering() {
  f3tree::FutureBTree tree(
      make_future_config(1, 1, 64, f3tree::EvaluatorPolicy::kDeterministicReplay));

  tree.buffered_insert(42, 0);
  assert(tree.contains(42));

  tree.erase_buffered(42, 0);
  assert(!tree.contains(42));

  tree.buffered_insert(42, 0);
  assert(tree.contains(42));
  assert(tree.buffered_owner(42) == 0);

  tree.drain_pending();
  assert(tree.contains(42));
}

// Delete/insert/delete on the same key: final visible state must be "absent".
void test_delete_insert_delete_ordering() {
  f3tree::FutureBTree tree(
      make_future_config(1, 1, 64, f3tree::EvaluatorPolicy::kDeterministicReplay));

  tree.warmup_insert(77);
  assert(tree.global_contains(77));

  tree.erase_buffered(77, 0);
  assert(!tree.contains(77));

  tree.buffered_insert(77, 0);
  assert(tree.contains(77));

  tree.erase_buffered(77, 0);
  assert(!tree.contains(77));

  tree.drain_pending();
  assert(!tree.contains(77));
}

// A buffered delete of a key that was never inserted must not create a
// spurious "present" entry after drain.
void test_delete_of_nonexistent_key() {
  f3tree::FutureBTree tree(
      make_future_config(1, 1, 64, f3tree::EvaluatorPolicy::kDeterministicReplay));

  assert(!tree.contains(999));
  tree.erase_buffered(999, 0);
  assert(!tree.contains(999));
  assert(tree.buffered_owner(999) == 0);

  tree.drain_pending();
  assert(!tree.contains(999));
  assert(tree.buffered_owner(999) == -1);
}

// A globally-visible key that is buffered-deleted must be invisible before
// drain, and physically absent after drain.
void test_buffered_delete_hides_global_key() {
  f3tree::FutureBTree tree(
      make_future_config(1, 1, 64, f3tree::EvaluatorPolicy::kDeterministicReplay));

  tree.warmup_insert(55);
  assert(tree.global_contains(55));
  assert(tree.contains(55));

  tree.erase_buffered(55, 0);
  assert(!tree.contains(55));
  assert(tree.global_contains(55));  // global tree not yet touched

  tree.drain_pending();
  assert(!tree.contains(55));
  assert(!tree.global_contains(55));  // physical delete applied
}

// Two producers insert different keys — both must be visible before and after
// drain. No cross-producer interaction.
void test_two_producer_disjoint_inserts() {
  f3tree::FutureBTree tree(
      make_future_config(2, 1, 64, f3tree::EvaluatorPolicy::kDeterministicReplay));

  tree.buffered_insert(10, 0);
  tree.buffered_insert(20, 1);

  assert(tree.contains(10));
  assert(tree.contains(20));
  assert(tree.buffered_owner(10) == 0);
  assert(tree.buffered_owner(20) == 1);

  tree.drain_pending();

  assert(tree.contains(10));
  assert(tree.contains(20));
  assert(tree.buffered_owner(10) == -1);
  assert(tree.buffered_owner(20) == -1);
}

// Repeated drains with no pending work must be idempotent and not corrupt state.
void test_repeated_drain_idempotence() {
  f3tree::FutureBTree tree(
      make_future_config(1, 1, 32, f3tree::EvaluatorPolicy::kDeterministicReplay));

  tree.buffered_insert(5, 0);
  tree.drain_pending();
  assert(tree.contains(5));

  tree.drain_pending();
  tree.drain_pending();
  assert(tree.contains(5));
  assert(tree.buffered_owner(5) == -1);
  assert(tree.pending_future_nodes(0) == 0);
}

// Duplicate inserts from different producers: the last-write-wins semantics
// must hold — key is present and owned by the last writer.
void test_duplicate_insert_last_writer_wins() {
  f3tree::FutureBTree tree(
      make_future_config(2, 1, 64, f3tree::EvaluatorPolicy::kDeterministicReplay));

  tree.buffered_insert(33, 0);
  tree.buffered_insert(33, 1);

  assert(tree.contains(33));
  // The last writer (producer 1, which has the higher sequence) owns the key.
  assert(tree.buffered_owner(33) == 1);

  tree.drain_pending();
  assert(tree.contains(33));
}

// Multi-instance test: two FutureBTree instances with different configs must
// be independently operational. This is now possible after removing the
// process-global n_threads/eval_threads/T_S state.
//
// Under F3_PMDK this test is intentionally skipped: the pool-backed backend
// uses a single process-global pool-UUID context which is incompatible with
// two concurrent instances. DRAM / non-PMDK builds continue to run this test.
#ifndef F3_PMDK
void test_multi_instance_isolation() {
  f3tree::RuntimeConfig cfg_a;
  cfg_a.producer_threads = 2;
  cfg_a.evaluator_threads = 1;
  cfg_a.hash_capacity = 64;

  f3tree::RuntimeConfig cfg_b;
  cfg_b.producer_threads = 3;
  cfg_b.evaluator_threads = 1;
  cfg_b.hash_capacity = 128;

  f3tree::FutureBTree tree_a(cfg_a);
  f3tree::FutureBTree tree_b(cfg_b);

  tree_a.buffered_insert(1001, 0);
  tree_a.buffered_insert(1002, 1);
  tree_b.buffered_insert(2001, 0);
  tree_b.buffered_insert(2002, 1);
  tree_b.buffered_insert(2003, 2);

  // Trees must not see each other's keys.
  assert(tree_a.contains(1001));
  assert(tree_a.contains(1002));
  assert(!tree_a.contains(2001));
  assert(tree_b.contains(2001));
  assert(tree_b.contains(2003));
  assert(!tree_b.contains(1001));

  tree_a.drain_pending();
  tree_b.drain_pending();

  assert(tree_a.global_contains(1001));
  assert(tree_a.global_contains(1002));
  assert(!tree_a.global_contains(2001));
  assert(tree_b.global_contains(2001));
  assert(tree_b.global_contains(2003));
  assert(!tree_b.global_contains(1001));
}
#endif  // !F3_PMDK

}  // namespace

int main() {
  test_persistent_btree_baseline();
  test_future_btree_buffered_visibility();
  test_future_btree_duplicate_operation_ordering();
  test_future_btree_empty_and_repeated_drain();
  test_future_btree_async_checkpointing();
  test_future_btree_evaluator_policies();
  test_future_btree_numa_placement_policy();
  test_future_btree_hard_numa_bind_policy();
  // Extended scenario and edge-case coverage
  test_insert_delete_insert_ordering();
  test_delete_insert_delete_ordering();
  test_delete_of_nonexistent_key();
  test_buffered_delete_hides_global_key();
  test_two_producer_disjoint_inserts();
  test_repeated_drain_idempotence();
  test_duplicate_insert_last_writer_wins();
#ifndef F3_PMDK
  test_multi_instance_isolation();
#endif

  std::cout << "smoke_test_passed\n";
  return 0;
}
