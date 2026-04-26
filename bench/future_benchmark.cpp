#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "f3tree/future_btree.h"
#include "f3tree/workload.h"

namespace {

struct Options {
  std::size_t key_count = 100000;
  std::size_t producer_threads = 4;
  std::size_t evaluator_threads = 2;
  unsigned long write_latency_ns = 0;
  std::size_t checkpoint_threshold_ops = 0;
  std::uint64_t checkpoint_interval_us = 0;
  std::uint64_t seed = 1;
  std::string evaluator_mode = "key";
  std::string placement_mode = "default";
  std::string input_path;
};

f3tree::EvaluatorPolicy parse_evaluator_mode(const std::string& mode) {
  if (mode == "f3k" || mode == "key") {
    return f3tree::EvaluatorPolicy::kF3K;
  }
  if (mode == "f3n" || mode == "node") {
    return f3tree::EvaluatorPolicy::kF3N;
  }
  if (mode == "deterministic") {
    return f3tree::EvaluatorPolicy::kDeterministicReplay;
  }
  throw std::runtime_error("unknown evaluator mode: " + mode);
}

f3tree::PlacementPolicy parse_placement_mode(const std::string& mode) {
  if (mode == "default") {
    return f3tree::PlacementPolicy::kDefault;
  }
  if (mode == "bind-evaluators") {
    return f3tree::PlacementPolicy::kBindEvaluatorsByNode;
  }
  if (mode == "bind-all") {
    return f3tree::PlacementPolicy::kBindEvaluatorsAndProducersByNode;
  }
  if (mode == "hard-numa") {
    return f3tree::PlacementPolicy::kHardNumaBind;
  }
  throw std::runtime_error("unknown placement mode: " + mode);
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-n" && i + 1 < argc) {
      options.key_count = static_cast<std::size_t>(std::stoull(argv[++i]));
    } else if (arg == "-t" && i + 1 < argc) {
      options.producer_threads = static_cast<std::size_t>(std::stoull(argv[++i]));
    } else if (arg == "-e" && i + 1 < argc) {
      options.evaluator_threads = static_cast<std::size_t>(std::stoull(argv[++i]));
    } else if (arg == "-w" && i + 1 < argc) {
      options.write_latency_ns = static_cast<unsigned long>(std::stoul(argv[++i]));
    } else if (arg == "-c" && i + 1 < argc) {
      options.checkpoint_threshold_ops = static_cast<std::size_t>(std::stoull(argv[++i]));
    } else if (arg == "-u" && i + 1 < argc) {
      options.checkpoint_interval_us = static_cast<std::uint64_t>(std::stoull(argv[++i]));
    } else if (arg == "-i" && i + 1 < argc) {
      options.input_path = argv[++i];
    } else if (arg == "-m" && i + 1 < argc) {
      options.evaluator_mode = argv[++i];
    } else if (arg == "-p" && i + 1 < argc) {
      options.placement_mode = argv[++i];
    } else if (arg == "-s" && i + 1 < argc) {
      options.seed = static_cast<std::uint64_t>(std::stoull(argv[++i]));
    } else {
      throw std::runtime_error("unknown option: " + arg);
    }
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    if (options.producer_threads == 0 || options.evaluator_threads == 0) {
      throw std::runtime_error("producer and evaluator counts must be greater than zero");
    }
    const std::vector<f3tree::Key> keys =
        f3tree::load_or_generate_keys(options.input_path, options.key_count,
                                      options.seed);

    f3tree::RuntimeConfig config;
    config.producer_threads = options.producer_threads;
    config.evaluator_threads = options.evaluator_threads;
    config.write_latency_ns = options.write_latency_ns;
    config.hash_capacity = options.key_count == 0 ? 1 : options.key_count;
    config.checkpoint_threshold_ops = options.checkpoint_threshold_ops;
    config.checkpoint_interval_us = options.checkpoint_interval_us;
    config.evaluator_policy = parse_evaluator_mode(options.evaluator_mode);
    config.placement_policy = parse_placement_mode(options.placement_mode);

    f3tree::FutureBTree tree(config);
    std::vector<std::future<void> > workers;
    const std::size_t chunk = keys.size() / options.producer_threads;

    const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    for (std::size_t tid = 0; tid < options.producer_threads; ++tid) {
      const std::size_t from = tid * chunk;
      const std::size_t to =
          (tid + 1 == options.producer_threads) ? keys.size() : from + chunk;
      workers.push_back(std::async(std::launch::async,
                                   [&tree, &keys, tid](std::size_t begin,
                                                       std::size_t end) {
                                     for (std::size_t i = begin; i < end; ++i) {
                                       tree.buffered_insert(keys[i], tid);
                                     }
                                   },
                                   from, to));
    }
    for (std::size_t i = 0; i < workers.size(); ++i) {
      workers[i].get();
    }
    const std::chrono::steady_clock::time_point buffered_stop =
        std::chrono::steady_clock::now();

    tree.drain_pending();

    const std::chrono::steady_clock::time_point stop =
        std::chrono::steady_clock::now();

    std::size_t found = 0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (tree.global_contains(keys[i])) {
        ++found;
      }
    }

    const std::chrono::microseconds buffered_elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(buffered_stop - start);
    const std::chrono::microseconds total_elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
    const f3tree::FutureBTreeStats stats = tree.stats();

    std::cout << "future_mode=" << options.evaluator_mode << '\n';
    std::cout << "future_placement=" << options.placement_mode << '\n';
    std::cout << "future_buffer_insert_us=" << buffered_elapsed.count() << '\n';
    std::cout << "future_total_us=" << total_elapsed.count() << '\n';
    std::cout << "future_checkpoint_count=" << stats.checkpoint_count << '\n';
    std::cout << "future_async_checkpoint_count=" << stats.async_checkpoint_count << '\n';
    std::cout << "future_checkpoint_time_us=" << stats.checkpoint_time_us << '\n';
    std::cout << "future_replayed_ops=" << stats.replayed_ops << '\n';
    std::cout << "future_replayed_nodes=" << stats.replayed_nodes << '\n';
    std::cout << "future_topology_nodes=" << stats.placement_topology_nodes << '\n';
    std::cout << "future_bound_evaluators=" << stats.bound_evaluator_threads << '\n';
    std::cout << "future_bound_producers=" << stats.bound_producer_threads << '\n';
    std::cout << "future_hard_numa_bound=" << stats.hard_numa_memory_bound << '\n';
    if (total_elapsed.count() > 0) {
      std::cout << "future_ops_per_sec="
                << (keys.size() * 1000000.0 / static_cast<double>(total_elapsed.count())) << '\n';
    }
    std::cout << "future_found=" << found << "/" << keys.size() << '\n';
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "future_benchmark error: " << ex.what() << '\n';
    return 1;
  }
}
