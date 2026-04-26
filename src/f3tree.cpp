#include "f3tree/future_btree.h"
#include "f3tree/persistent_btree.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "f3tree/platform.h"
#include "f3tree/ptfo_model.h"

#if !defined(__linux__) || !defined(__x86_64__)
#error "F3-tree only supports Linux/x86-64."
#endif

#include "../core/btree.h"

namespace {

constexpr std::size_t kFutureNodeCapacity = 30;

struct NumaTopology {
  std::vector<std::vector<int> > node_cpus;

  bool available() const { return !node_cpus.empty(); }
};

struct ReplayStats {
  std::uint64_t applied_ops = 0;
  std::uint64_t replayed_nodes = 0;
};

bool starts_with_node_prefix(const char* name) {
  return std::strncmp(name, "node", 4) == 0;
}

std::vector<int> parse_cpu_list(const std::string& cpulist) {
  std::vector<int> cpus;
  std::stringstream ss(cpulist);
  std::string token;
  while (std::getline(ss, token, ',')) {
    if (token.empty()) {
      continue;
    }

    const std::size_t dash = token.find('-');
    if (dash == std::string::npos) {
      cpus.push_back(std::atoi(token.c_str()));
      continue;
    }

    const int start = std::atoi(token.substr(0, dash).c_str());
    const int stop = std::atoi(token.substr(dash + 1).c_str());
    if (stop < start) {
      continue;
    }
    for (int cpu = start; cpu <= stop; ++cpu) {
      cpus.push_back(cpu);
    }
  }
  return cpus;
}

NumaTopology detect_numa_topology() {
  NumaTopology topology;
  std::vector<std::pair<int, std::vector<int> > > indexed_nodes;

  DIR* dir = opendir("/sys/devices/system/node");
  if (dir == NULL) {
    return topology;
  }

  struct dirent* entry = NULL;
  while ((entry = readdir(dir)) != NULL) {
    if (!starts_with_node_prefix(entry->d_name)) {
      continue;
    }

    const int node_index = std::atoi(entry->d_name + 4);
    std::string cpulist_path("/sys/devices/system/node/");
    cpulist_path += entry->d_name;
    cpulist_path += "/cpulist";

    std::ifstream cpulist_file(cpulist_path.c_str());
    if (!cpulist_file) {
      continue;
    }

    std::string cpulist;
    std::getline(cpulist_file, cpulist);
    std::vector<int> cpus = parse_cpu_list(cpulist);
    if (!cpus.empty()) {
      indexed_nodes.push_back(std::make_pair(node_index, cpus));
    }
  }
  closedir(dir);

  std::sort(indexed_nodes.begin(),
            indexed_nodes.end(),
            [](const std::pair<int, std::vector<int> >& lhs,
               const std::pair<int, std::vector<int> >& rhs) {
              return lhs.first < rhs.first;
            });

  for (std::size_t i = 0; i < indexed_nodes.size(); ++i) {
    topology.node_cpus.push_back(indexed_nodes[i].second);
  }
  return topology;
}

std::vector<int> build_cpu_to_node_map(const NumaTopology& topology) {
  int max_cpu = -1;
  for (std::size_t node = 0; node < topology.node_cpus.size(); ++node) {
    for (std::size_t i = 0; i < topology.node_cpus[node].size(); ++i) {
      if (topology.node_cpus[node][i] > max_cpu) {
        max_cpu = topology.node_cpus[node][i];
      }
    }
  }

  if (max_cpu < 0) {
    return std::vector<int>();
  }

  std::vector<int> cpu_to_node(static_cast<std::size_t>(max_cpu) + 1, -1);
  for (std::size_t node = 0; node < topology.node_cpus.size(); ++node) {
    for (std::size_t i = 0; i < topology.node_cpus[node].size(); ++i) {
      const int cpu = topology.node_cpus[node][i];
      if (cpu >= 0 && cpu <= max_cpu) {
        cpu_to_node[static_cast<std::size_t>(cpu)] = static_cast<int>(node);
      }
    }
  }
  return cpu_to_node;
}

int current_thread_numa_node(const std::vector<int>& cpu_to_node) {
  const int cpu = sched_getcpu();
  if (cpu < 0 || cpu >= static_cast<int>(cpu_to_node.size())) {
    return -1;
  }
  return cpu_to_node[static_cast<std::size_t>(cpu)];
}

bool bind_current_thread_to_cpus(const std::vector<int>& cpus) {
  if (cpus.empty()) {
    return false;
  }

  cpu_set_t set;
  CPU_ZERO(&set);
  bool has_cpu = false;
  for (std::size_t i = 0; i < cpus.size(); ++i) {
    if (cpus[i] < 0 || cpus[i] >= CPU_SETSIZE) {
      continue;
    }
    CPU_SET(cpus[i], &set);
    has_cpu = true;
  }
  if (!has_cpu) {
    return false;
  }
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

bool bind_current_thread_to_node(const NumaTopology& topology, std::size_t node_index) {
  if (!topology.available()) {
    return false;
  }
  return bind_current_thread_to_cpus(
      topology.node_cpus[node_index % topology.node_cpus.size()]);
}

bool evaluator_binding_enabled(f3tree::PlacementPolicy policy) {
  return policy == f3tree::PlacementPolicy::kBindEvaluatorsByNode ||
         policy == f3tree::PlacementPolicy::kBindEvaluatorsAndProducersByNode ||
         policy == f3tree::PlacementPolicy::kHardNumaBind;
}

bool producer_binding_enabled(f3tree::PlacementPolicy policy) {
  return policy == f3tree::PlacementPolicy::kBindEvaluatorsAndProducersByNode ||
         policy == f3tree::PlacementPolicy::kHardNumaBind;
}

bool hard_numa_bind_enabled(f3tree::PlacementPolicy policy) {
  return policy == f3tree::PlacementPolicy::kHardNumaBind;
}

// Hard NUMA memory binding via set_mempolicy(MPOL_BIND).
//
// MPOL_DEFAULT=0 and MPOL_BIND=2 are stable Linux ABI constants defined in
// <linux/mempolicy.h>. The values are used directly here to avoid a libnuma
// dependency; they have been stable since Linux 2.6.7.
//
// set_thread_mempolicy_bind() binds the calling thread's future memory
// allocations to node_index. Subsequent `new` and malloc calls from this
// thread are guaranteed by the kernel to come from that NUMA node (or fail
// with ENOMEM if the node is full and no fallback is permitted). This gives
// PTFO node allocations a stronger guarantee than first-touch heuristics.
static const int kMpolDefault = 0;
static const int kMpolBind    = 2;

bool set_thread_mempolicy_bind(std::size_t node_index) {
  // Limit to nodes representable in a single unsigned long bitmask.
  if (node_index >= static_cast<std::size_t>(sizeof(unsigned long) * CHAR_BIT)) {
    return false;
  }
  unsigned long nodemask = 1UL << node_index;
  // maxnode must be strictly greater than the highest set bit index.
  unsigned long maxnode = static_cast<unsigned long>(sizeof(unsigned long) * CHAR_BIT) + 1UL;
  return syscall(SYS_set_mempolicy, kMpolBind, &nodemask, maxnode) == 0;
}

void apply_buffered_op(btree* tree, const future_buffered_op& op) {
  if (op.is_delete) {
    if (tree->btree_search(op.key) != NULL) {
      tree->btree_delete(op.key);
    }
  } else {
    tree->btree_insert(op.key, reinterpret_cast<char*>(op.key));
  }
}

// Acquire every producer's PTFO mutex in a deterministic order (ascending by
// producer id) and return the held unique_locks. All drain helpers must hold
// the returned locks for the entire collect -> apply -> clear sequence so new
// producer inserts between collect and clear are not lost or freed mid-write.
//
// Producers never acquire another producer's PTFO mutex, so a drain that
// holds several in ascending order cannot deadlock with producer threads.
std::vector<std::unique_lock<std::mutex>> acquire_ptfo_locks(
    btree* tree, const std::vector<int>& producer_ids) {
  std::vector<int> sorted_pids = producer_ids;
  std::sort(sorted_pids.begin(), sorted_pids.end());
  std::vector<std::unique_lock<std::mutex>> locks;
  locks.reserve(sorted_pids.size());
  for (std::size_t i = 0; i < sorted_pids.size(); ++i) {
    const int pid = sorted_pids[i];
    if (pid < 0) {
      continue;
    }
    locks.emplace_back(tree->ptfo_mutex(pid));
  }
  return locks;
}

void clear_ptfo_threads_for_producers(
    btree* tree,
    const std::vector<f3tree::PTFOThreadState>& ptfo_threads,
    const std::vector<int>& producer_ids) {
  for (std::size_t i = 0; i < producer_ids.size(); ++i) {
    const int producer = producer_ids[i];
    if (producer < 0 ||
        producer >= static_cast<int>(ptfo_threads.size()) ||
        ptfo_threads[producer].entry_count == 0) {
      continue;
    }
    tree->future_clear_thread(producer);
  }
}

bool is_latest_buffered_op(const future_buffered_op& op,
                           const std::vector<future_lookup_entry>& latest_entries) {
  for (std::size_t i = 0; i < latest_entries.size(); ++i) {
    if (latest_entries[i].key != op.key) {
      continue;
    }
    return latest_entries[i].sequence == op.sequence &&
           latest_entries[i].state ==
               (op.is_delete ? FUTURE_TOMBSTONE : FUTURE_PRESENT);
  }
  return false;
}

std::vector<future_lookup_entry> collect_latest_lookup_entries(btree* tree) {
  std::vector<future_lookup_entry> entries;
  tree->future_collect_lookup_entries(&entries);
  // Sort by (key asc, sequence asc) so duplicate-key entries are adjacent
  // with the highest-sequence entry last.
  std::sort(entries.begin(), entries.end(), [](const future_lookup_entry& lhs,
                                               const future_lookup_entry& rhs) {
    if (lhs.key != rhs.key) {
      return lhs.key < rhs.key;
    }
    return lhs.sequence < rhs.sequence;
  });
  // Deduplicate: retain only the globally latest (highest-sequence) entry per
  // key. With per-producer lookup directories, multiple producers may have
  // recorded different states for the same key. Only the entry with the
  // highest sequence is the authoritative last-writer-wins result.
  std::vector<future_lookup_entry> latest;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (latest.empty() || latest.back().key != entries[i].key) {
      latest.push_back(entries[i]);
    } else {
      // entries is sorted ascending by sequence — entries[i] is newer.
      latest.back() = entries[i];
    }
  }
  return latest;
}

std::vector<int> collect_producers_for_nodes(
    const std::vector<f3tree::PTFOThreadState>& ptfo_threads,
    const std::vector<std::size_t>& node_ids) {
  std::vector<int> producers;
  for (std::size_t producer = 0; producer < ptfo_threads.size(); ++producer) {
    if (ptfo_threads[producer].entry_count == 0) {
      continue;
    }
    if (std::find(node_ids.begin(), node_ids.end(), ptfo_threads[producer].home_node) !=
        node_ids.end()) {
      producers.push_back(static_cast<int>(producer));
    }
  }
  return producers;
}

// Returns the producer IDs statically assigned to a given evaluator worker.
// Evaluator i owns producers [i*chunk, (i+1)*chunk), where
// chunk = total_producers / worker_count, with any remainder assigned to the
// last evaluator. Each evaluator thread owns a contiguous slice of the PTFO
// array (PTFO-range partitioning).
std::vector<int> producers_for_evaluator(std::size_t worker_id,
                                         std::size_t worker_count,
                                         std::size_t total_producers) {
  std::vector<int> producers;
  if (worker_count == 0 || total_producers == 0) {
    return producers;
  }
  const std::size_t chunk = total_producers / worker_count;
  const std::size_t start = worker_id * chunk;
  // Last evaluator gets all remaining producers (handles remainder).
  const std::size_t end =
      (worker_id + 1 == worker_count) ? total_producers : start + chunk;
  for (std::size_t i = start; i < end; ++i) {
    producers.push_back(static_cast<int>(i));
  }
  return producers;
}

std::vector<std::size_t> nodes_for_worker(std::size_t worker_id,
                                          std::size_t worker_count,
                                          std::size_t node_count) {
  std::vector<std::size_t> nodes;
  if (worker_count == 0 || node_count == 0) {
    return nodes;
  }
  for (std::size_t node = 0; node < node_count; ++node) {
    if ((node % worker_count) == worker_id) {
      nodes.push_back(node);
    }
  }
  return nodes;
}

std::vector<int> cpus_for_nodes(const NumaTopology& topology,
                                const std::vector<std::size_t>& node_ids) {
  std::vector<int> cpus;
  for (std::size_t i = 0; i < node_ids.size(); ++i) {
    if (node_ids[i] >= topology.node_cpus.size()) {
      continue;
    }
    cpus.insert(cpus.end(),
                topology.node_cpus[node_ids[i]].begin(),
                topology.node_cpus[node_ids[i]].end());
  }
  return cpus;
}

ReplayStats apply_key_ordered_ptfo_replay(
    btree* tree,
    const std::vector<f3tree::PTFOThreadState>& ptfo_threads,
    const std::vector<int>& producer_ids) {
  // Caller (drain_locked*) holds the per-producer PTFO locks for every
  // producer in producer_ids for the full drain window — including the
  // subsequent future_clear_lookup_* call. Do NOT acquire locks here; doing
  // so would overlap the caller's locks and could deadlock or (worse) appear
  // to succeed while leaving the directory-clear window unprotected.
  ReplayStats stats;
  std::vector<future_buffered_op> ops;
  for (std::size_t i = 0; i < producer_ids.size(); ++i) {
    const int producer = producer_ids[i];
    if (producer < 0 ||
        producer >= static_cast<int>(ptfo_threads.size()) ||
        ptfo_threads[producer].entry_count == 0) {
      continue;
    }
    tree->future_collect_ops(producer, &ops);
  }

  std::sort(ops.begin(), ops.end(), [](const future_buffered_op& lhs,
                                       const future_buffered_op& rhs) {
    if (lhs.sequence != rhs.sequence) {
      return lhs.sequence < rhs.sequence;
    }
    return lhs.producer_id < rhs.producer_id;
  });

  for (std::size_t i = 0; i < ops.size(); ++i) {
    apply_buffered_op(tree, ops[i]);
    ++stats.applied_ops;
  }

  clear_ptfo_threads_for_producers(tree, ptfo_threads, producer_ids);
  return stats;
}

// F3-K: key-based evaluation mode.
//
// Partitions producers across `evaluator_slots` groups using a contiguous
// range assignment (slot i owns producers [i*chunk, (i+1)*chunk)). Within
// each slot, each producer's PTFO is drained tail-first (oldest node first)
// so that later updates naturally overwrite earlier ones in the global tree.
//
// Tail-first traversal via future_drain_thread_from_tail provides two
// correctness/durability properties:
//   1. Old KV pairs are checkpointed before new ones, so if a key is updated
//      the newer value ends up in the global tree (last-write wins within
//      a producer's own sequence).
//   2. The tail pointer is advanced and each node freed incrementally, giving
//      a durable checkpoint boundary: crash recovery can resume from the new
//      tail without re-applying already-committed entries.
//
// This function does NOT perform cross-producer conflict resolution; it
// assumes disjoint key ranges across producers (F3-K's design contract).
ReplayStats apply_key_partitioned_ptfo_replay(
    btree* tree,
    const std::vector<f3tree::PTFOThreadState>& ptfo_threads,
    const std::vector<int>& producer_ids,
    std::size_t evaluator_slots) {
  ReplayStats stats;
  if (evaluator_slots == 0) {
    evaluator_slots = 1;
  }
  // Caller (drain_locked*) holds the per-producer PTFO locks for the full
  // drain window. future_drain_thread_from_tail here therefore runs with
  // exclusive access to each producer's chain.

  // Assign each producer to a slot by contiguous range partitioning so that
  // slot i owns producers [i*chunk, (i+1)*chunk), matching the static
  // evaluator-to-PTFO ownership assignment.
  const std::size_t n_producers = producer_ids.size();
  const std::size_t chunk =
      n_producers > 0 ? (n_producers + evaluator_slots - 1) / evaluator_slots : 1;

  std::vector<std::vector<int> > slots(evaluator_slots);
  for (std::size_t i = 0; i < producer_ids.size(); ++i) {
    std::size_t slot = i / chunk;
    if (slot >= evaluator_slots) {
      slot = evaluator_slots - 1;
    }
    slots[slot].push_back(producer_ids[i]);
  }

  // Process each slot: drain each assigned producer's PTFO tail-first.
  // future_drain_thread_from_tail applies entries AND frees nodes incrementally,
  // so no separate clear step is needed.
  for (std::size_t slot = 0; slot < evaluator_slots; ++slot) {
    for (std::size_t pi = 0; pi < slots[slot].size(); ++pi) {
      const int producer = slots[slot][pi];
      if (producer < 0 ||
          producer >= static_cast<int>(ptfo_threads.size()) ||
          ptfo_threads[producer].entry_count == 0) {
        continue;
      }
      const int applied = tree->future_drain_thread_from_tail(producer);
      stats.applied_ops += static_cast<std::uint64_t>(applied);
    }
  }
  // future_drain_thread_from_tail resets each producer's PTFO chain, so no
  // additional clear_ptfo_threads_for_producers call is needed here.
  return stats;
}

ReplayStats apply_node_ordered_ptfo_replay(
    btree* tree,
    const std::vector<f3tree::PTFOThreadState>& ptfo_threads,
    const std::vector<int>& producer_ids) {
  // Caller (drain_locked*) holds the per-producer PTFO locks for every
  // producer in producer_ids for the full drain window
  // (collect -> filter -> apply -> chain clear -> directory clear). This is
  // critical for F3-N correctness: the latest_entries snapshot below is
  // compared against op sequences later, and a producer must not be able to
  // insert a new op into its chain/directory in between the chain-clear
  // inside this function and the directory-clear performed by the caller
  // (future_clear_lookup_entries_for_producers).
  ReplayStats stats;
  const std::vector<future_lookup_entry> latest_entries = collect_latest_lookup_entries(tree);
  std::vector<future_buffered_node> nodes;
  for (std::size_t i = 0; i < producer_ids.size(); ++i) {
    const int producer = producer_ids[i];
    if (producer < 0 ||
        producer >= static_cast<int>(ptfo_threads.size()) ||
        ptfo_threads[producer].entry_count == 0) {
      continue;
    }
    tree->future_collect_nodes(producer, &nodes);
  }

  std::sort(nodes.begin(), nodes.end(), [](const future_buffered_node& lhs,
                                           const future_buffered_node& rhs) {
    if (lhs.first_sequence != rhs.first_sequence) {
      return lhs.first_sequence < rhs.first_sequence;
    }
    return lhs.producer_id < rhs.producer_id;
  });

  for (std::size_t i = 0; i < nodes.size(); ++i) {
    ++stats.replayed_nodes;
    for (std::size_t j = 0; j < nodes[i].ops.size(); ++j) {
      if (is_latest_buffered_op(nodes[i].ops[j], latest_entries)) {
        apply_buffered_op(tree, nodes[i].ops[j]);
        ++stats.applied_ops;
      }
    }
  }

  clear_ptfo_threads_for_producers(tree, ptfo_threads, producer_ids);
  return stats;
}

void validate_runtime_config(const f3tree::RuntimeConfig& config) {
  if (!f3tree::supported_platform()) {
    throw std::runtime_error(f3tree::unsupported_platform_message());
  }
  if (config.producer_threads == 0) {
    throw std::runtime_error("producer_threads must be greater than zero");
  }
  if (config.evaluator_threads == 0) {
    throw std::runtime_error("evaluator_threads must be greater than zero");
  }
  if (config.hash_capacity == 0) {
    throw std::runtime_error("hash_capacity must be greater than zero");
  }
}

// write_latency_in_ns is still a file-scope variable used by clflush(), which
// is called from page methods that have no btree reference. Keeping it as a
// shared global is acceptable here because (a) it simulates hardware write
// latency which is machine-wide, and (b) all btree operations are serialized
// through tree_mu in the wrapper. n_threads, eval_threads, and hash_capacity
// are now per-instance btree members and no longer need to be set here.
void apply_legacy_config(const f3tree::RuntimeConfig& config) {
  write_latency_in_ns = config.write_latency_ns;
}

}  // namespace

namespace f3tree {

struct PersistentBTree::Impl {
  explicit Impl(const RuntimeConfig& runtime_config)
      : config(runtime_config),
        tree(new btree(static_cast<int>(runtime_config.producer_threads),
                       static_cast<int>(runtime_config.evaluator_threads),
                       static_cast<int>(runtime_config.hash_capacity),
                       runtime_config.write_latency_ns,
                       runtime_config.persistent_path,
                       runtime_config.persistent_pool_bytes)) {}

  RuntimeConfig config;
  std::unique_ptr<btree> tree;
};

PersistentBTree::PersistentBTree(const RuntimeConfig& config) {
  validate_runtime_config(config);
  apply_legacy_config(config);
  impl_.reset(new Impl(config));
}

PersistentBTree::~PersistentBTree() {}

void PersistentBTree::insert(Key key) {
  impl_->tree->btree_insert(key, reinterpret_cast<char*>(key));
}

bool PersistentBTree::contains(Key key) const {
  return impl_->tree->btree_search(key) != NULL;
}

void PersistentBTree::erase(Key key) { impl_->tree->btree_delete(key); }

const RuntimeConfig& PersistentBTree::config() const { return impl_->config; }

struct FutureBTree::Impl {
  explicit Impl(const RuntimeConfig& runtime_config)
      : config(runtime_config),
        tree(new btree(static_cast<int>(runtime_config.producer_threads),
                       static_cast<int>(runtime_config.evaluator_threads),
                       static_cast<int>(runtime_config.hash_capacity),
                       runtime_config.write_latency_ns,
                       runtime_config.persistent_path,
                       runtime_config.persistent_pool_bytes)),
        ptfo_threads(runtime_config.producer_threads),
        topology(detect_numa_topology()),
        cpu_to_node(build_cpu_to_node_map(topology)),
        producer_bound(runtime_config.producer_threads, false),
        // On a fresh PMDK pool seq_counter_resume() returns 0 so we start at
        // 1; on reopen it returns the durable max-seq + gap so new sequences
        // strictly dominate any recovered one. Under a non-PMDK build it is
        // always 1.
        next_ptfo_sequence(std::max<std::uint64_t>(
            1, tree->seq_counter_resume() + 1)) {
    const std::size_t node_count = topology.available() ? topology.node_cpus.size() : 1;
    for (std::size_t producer = 0; producer < ptfo_threads.size(); ++producer) {
      ptfo_threads[producer].home_node = producer % node_count;
    }
    pending_operation_count_by_node.resize(node_count, 0);
    stats.placement_topology_nodes = topology.node_cpus.size();
  }

  bool async_checkpoint_enabled() const {
    return config.checkpoint_threshold_ops > 0 || config.checkpoint_interval_us > 0;
  }

  bool should_checkpoint_locked() const {
    if (pending_operation_count == 0) {
      return false;
    }
    return config.checkpoint_threshold_ops > 0 &&
           pending_operation_count >= config.checkpoint_threshold_ops;
  }

  bool should_checkpoint_for_producers_locked(const std::vector<int>& producer_ids) const {
    std::size_t producer_pending = 0;
    for (std::size_t i = 0; i < producer_ids.size(); ++i) {
      const int pid = producer_ids[i];
      if (pid >= 0 && pid < static_cast<int>(ptfo_threads.size())) {
        producer_pending += ptfo_threads[pid].entry_count;
      }
    }
    if (producer_pending == 0) {
      return false;
    }
    return config.checkpoint_threshold_ops > 0 &&
           producer_pending >= config.checkpoint_threshold_ops;
  }

  bool should_checkpoint_for_nodes_locked(const std::vector<std::size_t>& node_ids) const {
    std::size_t node_pending = 0;
    for (std::size_t i = 0; i < node_ids.size(); ++i) {
      if (node_ids[i] < pending_operation_count_by_node.size()) {
        node_pending += pending_operation_count_by_node[node_ids[i]];
      }
    }
    if (node_pending == 0) {
      return false;
    }
    return config.checkpoint_threshold_ops > 0 &&
           node_pending >= config.checkpoint_threshold_ops;
  }

  void subtract_pending_counts_locked(const std::vector<int>& producer_ids) {
    for (std::size_t i = 0; i < producer_ids.size(); ++i) {
      const int producer = producer_ids[i];
      if (producer < 0 || producer >= static_cast<int>(ptfo_threads.size())) {
        continue;
      }
      const std::size_t node = ptfo_threads[producer].home_node;
      if (node < pending_operation_count_by_node.size()) {
        pending_operation_count_by_node[node] -= ptfo_threads[producer].entry_count;
      }
      pending_operation_count -= ptfo_threads[producer].entry_count;
      ptfo_threads[producer].node_count = 0;
      ptfo_threads[producer].entry_count = 0;
    }
  }

  void record_checkpoint_locality_locked(const std::vector<int>& producer_ids) {
    const int drainer_node = current_thread_numa_node(cpu_to_node);
    for (std::size_t i = 0; i < producer_ids.size(); ++i) {
      const int producer = producer_ids[i];
      if (producer < 0 || producer >= static_cast<int>(ptfo_threads.size())) {
        continue;
      }
      const std::uint64_t op_count =
          static_cast<std::uint64_t>(ptfo_threads[producer].entry_count);
      if (op_count == 0) {
        continue;
      }
      if (drainer_node < 0) {
        stats.checkpoint_source_unknown_ops += op_count;
      } else if (ptfo_threads[producer].home_node ==
                 static_cast<std::size_t>(drainer_node)) {
        stats.checkpoint_source_local_ops += op_count;
      } else {
        stats.checkpoint_source_remote_ops += op_count;
      }
    }
  }

  void drain_locked() {
    // Include ALL producers (not just entry_count > 0) in the PTFO-lock set
    // so a producer whose counter was just 0 but whose future_insert is
    // in-flight cannot slip an orphan entry between the chain-clear and
    // directory-clear. The threshold short-circuit below uses
    // pending_operation_count — that counter is still correct as a "nothing
    // to do" signal because it is updated under impl_->mu, which the caller
    // holds; any in-flight producer is blocked on impl_->mu.
    if (pending_operation_count == 0) {
      return;
    }

    std::vector<int> producer_ids;
    for (std::size_t producer = 0; producer < ptfo_threads.size(); ++producer) {
      if (ptfo_threads[producer].entry_count > 0) {
        producer_ids.push_back(static_cast<int>(producer));
      }
    }
    record_checkpoint_locality_locked(producer_ids);

    const std::chrono::steady_clock::time_point checkpoint_start =
        std::chrono::steady_clock::now();
    ReplayStats replay_stats;
    {
      std::lock_guard<std::mutex> tree_lock(tree_mu);
      // Acquire PTFO locks for ALL n_threads producers. future_clear_lookup_directory
      // clears every directory, so we need exclusive access to every chain+directory
      // pair for the duration of this block. Lock order with producers is safe:
      // producers only ever take ptfo_mu[their_tid]; we take all in ascending order.
      std::vector<int> all_producers;
      all_producers.reserve(ptfo_threads.size());
      for (std::size_t i = 0; i < ptfo_threads.size(); ++i) {
        all_producers.push_back(static_cast<int>(i));
      }
      std::vector<std::unique_lock<std::mutex>> ptfo_locks =
          acquire_ptfo_locks(tree.get(), all_producers);
      switch (config.evaluator_policy) {
        case EvaluatorPolicy::kF3K:
        case EvaluatorPolicy::kLegacyKeyPartitioned:
          replay_stats = apply_key_partitioned_ptfo_replay(
              tree.get(), ptfo_threads, producer_ids, config.evaluator_threads);
          break;
        case EvaluatorPolicy::kF3N:
        case EvaluatorPolicy::kLegacyNodePartitioned:
          replay_stats = apply_node_ordered_ptfo_replay(tree.get(), ptfo_threads, producer_ids);
          break;
        case EvaluatorPolicy::kDeterministicReplay:
          replay_stats = apply_key_ordered_ptfo_replay(tree.get(), ptfo_threads, producer_ids);
          break;
      }
      tree->is_Done = true;
      tree->future_clear_lookup_directory();
    }
    const std::chrono::steady_clock::time_point checkpoint_stop =
        std::chrono::steady_clock::now();
    for (std::size_t node = 0; node < pending_operation_count_by_node.size(); ++node) {
      pending_operation_count_by_node[node] = 0;
    }
    for (std::size_t producer = 0; producer < ptfo_threads.size(); ++producer) {
      ptfo_threads[producer].node_count = 0;
      ptfo_threads[producer].entry_count = 0;
    }
    pending_operation_count = 0;
    ++stats.checkpoint_count;
    stats.checkpoint_time_us +=
        std::chrono::duration_cast<std::chrono::microseconds>(checkpoint_stop -
                                                              checkpoint_start)
            .count();
    stats.replayed_ops += replay_stats.applied_ops;
    stats.replayed_nodes += replay_stats.replayed_nodes;
  }

  void drain_locked_for_nodes(const std::vector<std::size_t>& node_ids) {
    const std::vector<int> producer_ids = collect_producers_for_nodes(ptfo_threads, node_ids);
    if (producer_ids.empty()) {
      return;
    }
    record_checkpoint_locality_locked(producer_ids);

    const std::chrono::steady_clock::time_point checkpoint_start =
        std::chrono::steady_clock::now();
    ReplayStats replay_stats;
    {
      std::lock_guard<std::mutex> tree_lock(tree_mu);
      // Hold PTFO locks across replay AND the lookup-directory clear. Without
      // this, a blocked producer could complete future_insert (updating both
      // chain and directory) during the small window between
      // apply_*_ptfo_replay's internal chain clear and the directory clear
      // below, leaving an orphaned chain entry that F3-N's latest-state
      // filter will reject on the next drain.
      std::vector<std::unique_lock<std::mutex>> ptfo_locks =
          acquire_ptfo_locks(tree.get(), producer_ids);
      switch (config.evaluator_policy) {
        case EvaluatorPolicy::kF3K:
        case EvaluatorPolicy::kLegacyKeyPartitioned:
          replay_stats = apply_key_partitioned_ptfo_replay(
              tree.get(), ptfo_threads, producer_ids, config.evaluator_threads);
          break;
        case EvaluatorPolicy::kF3N:
        case EvaluatorPolicy::kLegacyNodePartitioned:
          replay_stats = apply_node_ordered_ptfo_replay(tree.get(), ptfo_threads, producer_ids);
          break;
        case EvaluatorPolicy::kDeterministicReplay:
          replay_stats = apply_key_ordered_ptfo_replay(tree.get(), ptfo_threads, producer_ids);
          break;
      }
      tree->future_clear_lookup_entries_for_producers(producer_ids);
    }
    const std::chrono::steady_clock::time_point checkpoint_stop =
        std::chrono::steady_clock::now();
    subtract_pending_counts_locked(producer_ids);
    ++stats.checkpoint_count;
    ++stats.async_checkpoint_count;
    stats.checkpoint_time_us +=
        std::chrono::duration_cast<std::chrono::microseconds>(checkpoint_stop -
                                                              checkpoint_start)
            .count();
    stats.replayed_ops += replay_stats.applied_ops;
    stats.replayed_nodes += replay_stats.replayed_nodes;
  }

  // Drain only the producers listed in producer_ids. Used by async evaluator
  // workers that own a static slice of the PTFO array.
  void drain_locked_for_producers(const std::vector<int>& producer_ids) {
    // Filter to producers that actually have pending operations.
    std::vector<int> active_producers;
    for (std::size_t i = 0; i < producer_ids.size(); ++i) {
      const int pid = producer_ids[i];
      if (pid >= 0 &&
          pid < static_cast<int>(ptfo_threads.size()) &&
          ptfo_threads[pid].entry_count > 0) {
        active_producers.push_back(pid);
      }
    }
    if (active_producers.empty()) {
      return;
    }
    record_checkpoint_locality_locked(active_producers);

    const std::chrono::steady_clock::time_point checkpoint_start =
        std::chrono::steady_clock::now();
    ReplayStats replay_stats;
    {
      std::lock_guard<std::mutex> tree_lock(tree_mu);
      // Hold PTFO locks across replay + directory clear
      // (see drain_locked_for_nodes for rationale).
      std::vector<std::unique_lock<std::mutex>> ptfo_locks =
          acquire_ptfo_locks(tree.get(), active_producers);
      switch (config.evaluator_policy) {
        case EvaluatorPolicy::kF3K:
        case EvaluatorPolicy::kLegacyKeyPartitioned:
          // Each evaluator processes its own producers; use a single slot so
          // they are applied in per-producer PTFO order without re-partitioning.
          replay_stats = apply_key_partitioned_ptfo_replay(
              tree.get(), ptfo_threads, active_producers, 1);
          break;
        case EvaluatorPolicy::kF3N:
        case EvaluatorPolicy::kLegacyNodePartitioned:
          replay_stats =
              apply_node_ordered_ptfo_replay(tree.get(), ptfo_threads, active_producers);
          break;
        case EvaluatorPolicy::kDeterministicReplay:
          replay_stats =
              apply_key_ordered_ptfo_replay(tree.get(), ptfo_threads, active_producers);
          break;
      }
      tree->future_clear_lookup_entries_for_producers(active_producers);
    }
    const std::chrono::steady_clock::time_point checkpoint_stop =
        std::chrono::steady_clock::now();
    subtract_pending_counts_locked(active_producers);
    ++stats.checkpoint_count;
    ++stats.async_checkpoint_count;
    stats.checkpoint_time_us +=
        std::chrono::duration_cast<std::chrono::microseconds>(checkpoint_stop -
                                                              checkpoint_start)
            .count();
    stats.replayed_ops += replay_stats.applied_ops;
    stats.replayed_nodes += replay_stats.replayed_nodes;
  }

  void evaluator_loop(std::size_t worker_id, std::size_t worker_count) {
    std::unique_lock<std::mutex> lock(mu);

    // NUMA-aware path: determine NUMA nodes owned by this evaluator.
    const std::vector<std::size_t> owned_nodes =
        evaluator_binding_enabled(config.placement_policy) && topology.available()
            ? nodes_for_worker(worker_id, worker_count, topology.node_cpus.size())
            : std::vector<std::size_t>();

    // Static PTFO-range assignment: evaluator i owns producers
    // [i*chunk, (i+1)*chunk). When NUMA binding is active the owned_nodes
    // path is used instead, but owned_producers is always computed so the
    // non-NUMA path can drain only this worker's assigned producers rather
    // than doing a full drain that would race with other evaluator workers.
    const std::vector<int> owned_producers =
        producers_for_evaluator(worker_id, worker_count, ptfo_threads.size());

    while (!stop_requested) {
      if (!async_checkpoint_enabled()) {
        cv.wait(lock, [this] { return stop_requested; });
        continue;
      }

      // NUMA-aware: drain producers assigned to owned NUMA nodes.
      if (!owned_nodes.empty() && should_checkpoint_for_nodes_locked(owned_nodes)) {
        drain_locked_for_nodes(owned_nodes);
        continue;
      }

      // Non-NUMA: drain only this evaluator's statically assigned producers.
      if (owned_nodes.empty() &&
          should_checkpoint_for_producers_locked(owned_producers)) {
        drain_locked_for_producers(owned_producers);
        continue;
      }

      if (config.checkpoint_interval_us > 0 && pending_operation_count > 0) {
        cv.wait_for(
            lock,
            std::chrono::microseconds(config.checkpoint_interval_us),
            [this, &owned_nodes, &owned_producers] {
              return stop_requested ||
                     (!owned_nodes.empty() &&
                      should_checkpoint_for_nodes_locked(owned_nodes)) ||
                     (owned_nodes.empty() &&
                      should_checkpoint_for_producers_locked(owned_producers));
            });
        if (stop_requested) {
          break;
        }
        if (!owned_nodes.empty()) {
          drain_locked_for_nodes(owned_nodes);
        } else {
          // Interval elapsed: drain any pending ops for this evaluator's
          // producers, regardless of whether the threshold was reached.
          // drain_locked_for_producers filters empty producers internally.
          drain_locked_for_producers(owned_producers);
        }
        continue;
      }

      cv.wait(lock, [this, &owned_nodes, &owned_producers] {
        return stop_requested ||
               (!owned_nodes.empty() &&
                should_checkpoint_for_nodes_locked(owned_nodes)) ||
               (owned_nodes.empty() &&
                should_checkpoint_for_producers_locked(owned_producers)) ||
               (async_checkpoint_enabled() && config.checkpoint_interval_us > 0 &&
                pending_operation_count > 0);
      });
    }
  }

  RuntimeConfig config;
  std::unique_ptr<btree> tree;
  std::vector<PTFOThreadState> ptfo_threads;
  NumaTopology topology;
  std::vector<int> cpu_to_node;
  std::vector<bool> producer_bound;
  std::atomic<std::uint64_t> next_ptfo_sequence;
  std::size_t pending_operation_count = 0;
  std::vector<std::size_t> pending_operation_count_by_node;
  mutable std::mutex mu;
  mutable std::mutex tree_mu;
  std::condition_variable cv;
  bool stop_requested = false;
  std::vector<std::thread> evaluator_workers;
  FutureBTreeStats stats;
};

FutureBTree::FutureBTree(const RuntimeConfig& config) {
  validate_runtime_config(config);
  apply_legacy_config(config);
  impl_.reset(new Impl(config));
  const std::size_t available_nodes =
      impl_->topology.available() ? impl_->topology.node_cpus.size() : 1;
  const std::size_t worker_count =
      evaluator_binding_enabled(config.placement_policy) && impl_->topology.available()
          ? std::min(config.evaluator_threads, available_nodes)
          : config.evaluator_threads;
  for (std::size_t i = 0; i < worker_count; ++i) {
    impl_->evaluator_workers.push_back(std::thread([this, i, worker_count] {
      const std::vector<std::size_t> owned_nodes =
          evaluator_binding_enabled(impl_->config.placement_policy) &&
                  impl_->topology.available()
              ? nodes_for_worker(i,
                                 std::min(impl_->config.evaluator_threads,
                                          impl_->topology.node_cpus.size()),
                                 impl_->topology.node_cpus.size())
              : std::vector<std::size_t>();
      if (!owned_nodes.empty() &&
          bind_current_thread_to_cpus(cpus_for_nodes(impl_->topology, owned_nodes))) {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ++impl_->stats.bound_evaluator_threads;
      }
      impl_->evaluator_loop(i, worker_count);
    }));
  }
}

FutureBTree::~FutureBTree() {
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->pending_operation_count > 0) {
      impl_->drain_locked();
    }
    impl_->stop_requested = true;
  }
  impl_->cv.notify_all();
  for (std::size_t i = 0; i < impl_->evaluator_workers.size(); ++i) {
    if (impl_->evaluator_workers[i].joinable()) {
      impl_->evaluator_workers[i].join();
    }
  }
}

void FutureBTree::warmup_insert(Key key) {
  std::lock_guard<std::mutex> lock(impl_->tree_mu);
  impl_->tree->btree_insert(key, reinterpret_cast<char*>(key));
}

void FutureBTree::buffered_insert(Key key, std::size_t producer_id) {
  if (producer_id >= impl_->ptfo_threads.size()) {
    throw std::out_of_range("producer_id exceeds configured producer_threads");
  }

  // NUMA producer binding: first call only. Check under mu, then bind outside
  // mu (pthread_setaffinity_np and set_mempolicy are per-thread and don't need
  // the lock).
  if (producer_binding_enabled(impl_->config.placement_policy) &&
      !impl_->producer_bound[producer_id]) {
    bool do_bind = false;
    {
      std::lock_guard<std::mutex> lock(impl_->mu);
      if (!impl_->producer_bound[producer_id]) {
        impl_->producer_bound[producer_id] = true;
        do_bind = true;
      }
    }
    if (do_bind) {
      const std::size_t home_node = impl_->ptfo_threads[producer_id].home_node;
      if (bind_current_thread_to_node(impl_->topology, home_node)) {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ++impl_->stats.bound_producer_threads;
      }
      // Hard NUMA memory policy: set_mempolicy(MPOL_BIND) enforces that
      // future allocations from this thread land on home_node, giving PTFO
      // node allocations a kernel-level locality guarantee beyond first-touch
      // heuristics.
      if (hard_numa_bind_enabled(impl_->config.placement_policy) &&
          set_thread_mempolicy_bind(home_node)) {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ++impl_->stats.hard_numa_memory_bound;
      }
    }
  }

  // Sequence number: lock-free atomic fetch-add. This gives each operation a
  // globally unique monotonically increasing sequence without a mutex.
  const std::uint64_t sequence =
      impl_->next_ptfo_sequence.fetch_add(1, std::memory_order_relaxed);

  // PTFO insert: entirely lock-free on the per-producer structures.
  // future_insert writes to local_fut[producer_id] (linked list) and
  // future_lookup_directory[producer_id] (per-producer dir, protected by
  // lookup_dir_mu[producer_id] inside future_insert). No global tree lock.
  impl_->tree->future_insert(key, static_cast<int>(producer_id), false, sequence);

  // Update wrapper-level counters and wake the evaluator. This brief lock
  // covers only the accounting update, not the PTFO write above.
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->ptfo_threads[producer_id].entry_count == 0 ||
        (impl_->ptfo_threads[producer_id].entry_count % kFutureNodeCapacity) == 0) {
      ++impl_->ptfo_threads[producer_id].node_count;
    }
    ++impl_->ptfo_threads[producer_id].entry_count;
    ++impl_->pending_operation_count;
    ++impl_->pending_operation_count_by_node[impl_->ptfo_threads[producer_id].home_node];
    const int producer_node = current_thread_numa_node(impl_->cpu_to_node);
    if (producer_node < 0) {
      ++impl_->stats.producer_enqueue_unknown_ops;
    } else if (impl_->ptfo_threads[producer_id].home_node ==
               static_cast<std::size_t>(producer_node)) {
      ++impl_->stats.producer_enqueue_local_ops;
    } else {
      ++impl_->stats.producer_enqueue_remote_ops;
    }
    impl_->cv.notify_one();
  }
}

void FutureBTree::erase_buffered(Key key, std::size_t producer_id) {
  if (producer_id >= impl_->ptfo_threads.size()) {
    throw std::out_of_range("producer_id exceeds configured producer_threads");
  }

  // NUMA producer binding: first call only (same pattern as buffered_insert).
  if (producer_binding_enabled(impl_->config.placement_policy) &&
      !impl_->producer_bound[producer_id]) {
    bool do_bind = false;
    {
      std::lock_guard<std::mutex> lock(impl_->mu);
      if (!impl_->producer_bound[producer_id]) {
        impl_->producer_bound[producer_id] = true;
        do_bind = true;
      }
    }
    if (do_bind) {
      const std::size_t home_node = impl_->ptfo_threads[producer_id].home_node;
      if (bind_current_thread_to_node(impl_->topology, home_node)) {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ++impl_->stats.bound_producer_threads;
      }
      if (hard_numa_bind_enabled(impl_->config.placement_policy) &&
          set_thread_mempolicy_bind(home_node)) {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ++impl_->stats.hard_numa_memory_bound;
      }
    }
  }

  const std::uint64_t sequence =
      impl_->next_ptfo_sequence.fetch_add(1, std::memory_order_relaxed);

  // Lock-free PTFO tombstone insert (isDone=true).
  impl_->tree->future_insert(key, static_cast<int>(producer_id), true, sequence);

  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->ptfo_threads[producer_id].entry_count == 0 ||
        (impl_->ptfo_threads[producer_id].entry_count % kFutureNodeCapacity) == 0) {
      ++impl_->ptfo_threads[producer_id].node_count;
    }
    ++impl_->ptfo_threads[producer_id].entry_count;
    ++impl_->pending_operation_count;
    ++impl_->pending_operation_count_by_node[impl_->ptfo_threads[producer_id].home_node];
    const int producer_node = current_thread_numa_node(impl_->cpu_to_node);
    if (producer_node < 0) {
      ++impl_->stats.producer_enqueue_unknown_ops;
    } else if (impl_->ptfo_threads[producer_id].home_node ==
               static_cast<std::size_t>(producer_node)) {
      ++impl_->stats.producer_enqueue_local_ops;
    } else {
      ++impl_->stats.producer_enqueue_remote_ops;
    }
    impl_->cv.notify_one();
  }
}

bool FutureBTree::contains(Key key) const {
  // 3-tier hierarchical search via direct PTFO linked-list traversal.
  //   Tier 1: per-producer hashtable pre-filter (inside _via_ptfo).
  //   Tier 2: scan the producer's PTFO chain under ptfo_mu[tid] (inside
  //           _via_ptfo). Authoritative for buffered state.
  //   Tier 3: global B+-tree fallback below.
  const future_entry_state ptfo_state =
      impl_->tree->future_hash_directed_state_via_ptfo(key);
  switch (ptfo_state) {
    case FUTURE_PRESENT:
      return true;
    case FUTURE_TOMBSTONE:
      return false;
    case FUTURE_ABSENT:
      // Global tree fallback. ptfo_mu[tid] locks released on return from
      // _via_ptfo; tree_mu is independent so no deadlock risk.
      {
        std::lock_guard<std::mutex> lock(impl_->tree_mu);
        return impl_->tree->btree_search(key) != NULL;
      }
  }
  return false;
}

bool FutureBTree::global_contains(Key key) {
  std::lock_guard<std::mutex> lock(impl_->tree_mu);
  return impl_->tree->btree_search(key) != NULL;
}

int FutureBTree::buffered_owner(Key key) const {
  // Use the direct-PTFO-traversal owner lookup so buffered_owner() and
  // contains() are driven by the same authoritative PTFO chain, not by two
  // independent proxies (chain vs lookup-directory).
  return impl_->tree->future_hash_directed_owner_via_ptfo(key);
}

std::size_t FutureBTree::pending_future_nodes(std::size_t producer_id) const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (producer_id >= impl_->ptfo_threads.size()) {
    throw std::out_of_range("producer_id exceeds configured producer_threads");
  }
  return impl_->ptfo_threads[producer_id].node_count;
}

void FutureBTree::drain_pending() {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->drain_locked();
  impl_->cv.notify_all();
}

FutureBTreeStats FutureBTree::stats() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->stats;
}

const RuntimeConfig& FutureBTree::config() const { return impl_->config; }

std::size_t FutureBTree::scan_range(Key start, int count, std::vector<Key> &out) {
  // Drain first so every key inserted via buffered_insert is visible.
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->drain_locked();
    impl_->cv.notify_all();
  }
  if (count <= 0) return 0;

  // Allocate a buffer large enough for `count` values and call the underlying
  // B+-tree range scan.  The buffer holds opaque entry_ptr_t values; we treat
  // any non-zero slot as a present key, counting them as results.
  std::vector<unsigned long> buf(static_cast<std::size_t>(count), 0UL);
  Key end = start + static_cast<Key>(count);
  impl_->tree->btree_search_range(start, end, buf.data());

  out.clear();
  std::size_t found = 0;
  for (int i = 0; i < count; ++i) {
    if (buf[static_cast<std::size_t>(i)] != 0UL) {
      out.push_back(start + static_cast<Key>(i));
      ++found;
    }
  }
  return found;
}

}  // namespace f3tree
