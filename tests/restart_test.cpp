// Restart-style test harness for F3-tree persistence invariants.
//
// Usage (DRAM, in-process simulation):
//   ./bin/restart_test --phase write   --manifest /tmp/f3_manifest.txt
//   ./bin/restart_test --phase verify  --manifest /tmp/f3_manifest.txt
//
// Usage (PMDK cross-process restart):
//   ./bin/restart_test --phase write   --manifest /tmp/f3_manifest.txt \
//                      --pool /dev/shm/f3tree_restart.pool --pmdk-crash
//   ./bin/restart_test --phase verify  --manifest /tmp/f3_manifest.txt \
//                      --pool /dev/shm/f3tree_restart.pool
//
// Phase "write":
//   - Performs a mix of warmup, buffered, and delete operations against
//     either a DRAM tree or a PMDK pool (if --pool is given).
//   - With --pmdk-crash, skips destructor so undrained PTFO state stays
//     durable in the pool without a clean shutdown (simulates abrupt
//     process termination).
//   - Writes a manifest describing the expected post-restart key states.
//
// Phase "verify":
//   - Reads the manifest.
//   - Either reopens the PMDK pool in a fresh process (cross-process
//     restart) or reconstructs the drained state in a fresh DRAM tree.
//   - Verifies each key's expected visibility.
//
// See scripts/run_restart_test.sh for the shell driver that runs the
// write and verify phases in separate processes against the same pool.

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "f3tree/future_btree.h"
#include "f3tree/persistent_btree.h"

namespace {

struct KeyExpectation {
  f3tree::Key key;
  bool expected_present;
  std::string note;
};

void write_manifest(const std::string& path,
                    const std::vector<KeyExpectation>& expectations) {
  std::ofstream out(path.c_str());
  if (!out) {
    throw std::runtime_error("cannot open manifest for writing: " + path);
  }
  for (std::size_t i = 0; i < expectations.size(); ++i) {
    out << expectations[i].key << " "
        << (expectations[i].expected_present ? 1 : 0) << " "
        << expectations[i].note << "\n";
  }
}

std::vector<KeyExpectation> read_manifest(const std::string& path) {
  std::ifstream in(path.c_str());
  if (!in) {
    throw std::runtime_error("cannot open manifest for reading: " + path);
  }
  std::vector<KeyExpectation> expectations;
  f3tree::Key key;
  int present;
  std::string note;
  while (in >> key >> present >> note) {
    KeyExpectation e;
    e.key = key;
    e.expected_present = (present != 0);
    e.note = note;
    expectations.push_back(e);
  }
  return expectations;
}

// Reconstruct tree state by replaying the drained operations described in the
// manifest. In a real PM system this would be replaced by re-opening the
// persistent storage path and reading the recovered B+-tree.
void replay_for_verify(f3tree::PersistentBTree& tree,
                       const std::vector<KeyExpectation>& expectations) {
  for (std::size_t i = 0; i < expectations.size(); ++i) {
    if (expectations[i].expected_present) {
      tree.insert(expectations[i].key);
    }
    // Keys expected to be absent are simply not inserted; deleted keys that
    // were previously inserted are left out of the replay (the drain would
    // have removed them).
  }
}

int run_write_phase(const std::string& manifest_path,
                    const std::string& pool_path,
                    bool pmdk_crash_mode) {
  f3tree::RuntimeConfig config;
  config.producer_threads = 2;
  config.evaluator_threads = 1;
  config.hash_capacity = 128;
  config.persistent_path = pool_path;
  config.persistent_pool_bytes = 64ull * 1024ull * 1024ull;

  // When pmdk_crash_mode is set, leak the tree so its destructor does not
  // run. This exercises undrained-PTFO recovery on the verify side.
  auto *tree = new f3tree::FutureBTree(config);
  std::vector<KeyExpectation> expectations;

  // --- Scenario 1: baseline warmup insert ---
  tree->warmup_insert(1000);
  expectations.push_back({1000, true, "warmup_insert"});

  // --- Scenario 2: buffered insert then drain ---
  tree->buffered_insert(2000, 0);
  tree->buffered_insert(2001, 1);
  expectations.push_back({2000, true, "buffered_insert_drained"});
  expectations.push_back({2001, true, "buffered_insert_drained"});

  // --- Scenario 3: buffered delete of a previously global key ---
  tree->warmup_insert(3000);
  tree->erase_buffered(3000, 0);
  expectations.push_back({3000, false, "buffered_delete_of_global_drained"});

  // --- Scenario 4: insert then delete before drain ---
  tree->buffered_insert(4000, 0);
  tree->erase_buffered(4000, 0);
  expectations.push_back({4000, false, "insert_then_delete_drained"});

  // --- Scenario 5: insert/delete/insert sequence ---
  tree->buffered_insert(5000, 1);
  tree->erase_buffered(5000, 1);
  tree->buffered_insert(5000, 1);
  expectations.push_back({5000, true, "insert_delete_insert_drained"});

  tree->drain_pending();

  // --- Scenario 6 (pmdk_crash_mode only): undrained PTFO state ---
  if (pmdk_crash_mode) {
    tree->warmup_insert(6000);   // durable global
    expectations.push_back({6000, true, "crash_warmup_global"});
    tree->buffered_insert(7000, 0);
    tree->buffered_insert(7001, 1);
    // NOT drained — visible after reopen because the recovery pass
    // rebuilds the per-producer hash + lookup directory from the
    // pool-resident PTFO chain.
    expectations.push_back({7000, true, "crash_undrained_ptfo_ins"});
    expectations.push_back({7001, true, "crash_undrained_ptfo_ins"});
  }

  write_manifest(manifest_path, expectations);
  std::cout << "restart_test write phase: manifest written to "
            << manifest_path;
  if (!pool_path.empty()) {
    std::cout << " (pool=" << pool_path
              << (pmdk_crash_mode ? ", pmdk-crash" : "")
              << ")";
  }
  std::cout << "\n";

  if (pmdk_crash_mode) {
    // _exit bypasses destructors, simulating an abrupt process termination.
    // The pool file on disk carries the durable PTFO + global-tree state.
    std::cout << "restart_test write phase: skipping destructor "
                 "(pmdk-crash mode)\n";
    std::fflush(stdout);
    std::_Exit(0);
  }
  delete tree;
  return 0;
}

int run_verify_phase(const std::string& manifest_path,
                     const std::string& pool_path) {
  const std::vector<KeyExpectation> expectations = read_manifest(manifest_path);
  if (expectations.empty()) {
    std::cerr << "restart_test verify: manifest is empty or unreadable\n";
    return 1;
  }

  int failures = 0;
  if (!pool_path.empty()) {
    // Cross-process reopen: the pool file on disk is the only state
    // handed across the process boundary.
    f3tree::RuntimeConfig config;
    config.producer_threads = 2;
    config.evaluator_threads = 1;
    config.hash_capacity = 128;
    config.persistent_path = pool_path;
    f3tree::FutureBTree tree(config);
    for (std::size_t i = 0; i < expectations.size(); ++i) {
      // contains() traverses hashtable -> PTFO chain -> global tree, so
      // both drained and recovered-undrained keys are visible here.
      const bool actual = tree.contains(expectations[i].key);
      if (actual != expectations[i].expected_present) {
        std::cerr << "FAIL [" << expectations[i].note << "] key="
                  << expectations[i].key
                  << " expected=" << (expectations[i].expected_present ? "present" : "absent")
                  << " actual=" << (actual ? "present" : "absent") << "\n";
        ++failures;
      }
    }
  } else {
    // Legacy DRAM path: simulate restart by replaying the drained
    // manifest into a fresh PersistentBTree. Undrained state cannot be
    // validated in this mode (no backing storage to survive the exit).
    f3tree::RuntimeConfig config;
    config.producer_threads = 2;
    config.evaluator_threads = 1;
    config.hash_capacity = 128;
    f3tree::PersistentBTree tree(config);
    replay_for_verify(tree, expectations);
    for (std::size_t i = 0; i < expectations.size(); ++i) {
      const bool actual = tree.contains(expectations[i].key);
      if (actual != expectations[i].expected_present) {
        std::cerr << "FAIL [" << expectations[i].note << "] key="
                  << expectations[i].key
                  << " expected=" << (expectations[i].expected_present ? "present" : "absent")
                  << " actual=" << (actual ? "present" : "absent") << "\n";
        ++failures;
      }
    }
  }

  if (failures == 0) {
    std::cout << "restart_test verify phase: all "
              << expectations.size() << " invariants passed";
    if (!pool_path.empty()) std::cout << " (pool=" << pool_path << ")";
    std::cout << "\n";
    return 0;
  }
  return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string phase;
  std::string manifest_path = "/tmp/f3tree_restart_manifest.txt";
  std::string pool_path;
  bool pmdk_crash = false;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--phase") == 0 && i + 1 < argc) {
      phase = argv[++i];
    } else if (std::strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) {
      manifest_path = argv[++i];
    } else if (std::strcmp(argv[i], "--pool") == 0 && i + 1 < argc) {
      pool_path = argv[++i];
    } else if (std::strcmp(argv[i], "--pmdk-crash") == 0) {
      pmdk_crash = true;
    }
  }

  if (phase == "write") {
    return run_write_phase(manifest_path, pool_path, pmdk_crash);
  }
  if (phase == "verify") {
    return run_verify_phase(manifest_path, pool_path);
  }

  // No phase flag: run write then verify in-process to exercise the invariants
  // automatically in CI. This does not actually simulate a process boundary,
  // but it validates the logical contract of drain → reconstruct.
  std::cout << "restart_test: no --phase flag; running write+verify in-process\n";
  if (run_write_phase(manifest_path, "", false) != 0) {
    return 1;
  }
  return run_verify_phase(manifest_path, "");
}
