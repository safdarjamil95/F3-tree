#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "f3tree/persistent_btree.h"
#include "f3tree/workload.h"

namespace {

struct Options {
  std::size_t key_count = 100000;
  std::size_t thread_count = 4;
  unsigned long write_latency_ns = 0;
  std::uint64_t seed = 1;
  std::string input_path;
};

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-n" && i + 1 < argc) {
      options.key_count = static_cast<std::size_t>(std::stoull(argv[++i]));
    } else if (arg == "-t" && i + 1 < argc) {
      options.thread_count = static_cast<std::size_t>(std::stoull(argv[++i]));
    } else if (arg == "-w" && i + 1 < argc) {
      options.write_latency_ns = static_cast<unsigned long>(std::stoul(argv[++i]));
    } else if (arg == "-i" && i + 1 < argc) {
      options.input_path = argv[++i];
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
    if (options.thread_count == 0) {
      throw std::runtime_error("thread count must be greater than zero");
    }
    const std::vector<f3tree::Key> keys =
        f3tree::load_or_generate_keys(options.input_path, options.key_count,
                                      options.seed);

    f3tree::RuntimeConfig config;
    config.producer_threads = options.thread_count;
    config.evaluator_threads = 1;
    config.write_latency_ns = options.write_latency_ns;
    config.hash_capacity = options.key_count == 0 ? 1 : options.key_count;

    f3tree::PersistentBTree tree(config);
    std::vector<std::future<void> > workers;
    const std::size_t chunk = keys.size() / options.thread_count;

    const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    for (std::size_t tid = 0; tid < options.thread_count; ++tid) {
      const std::size_t from = tid * chunk;
      const std::size_t to =
          (tid + 1 == options.thread_count) ? keys.size() : from + chunk;
      workers.push_back(std::async(std::launch::async,
                                   [&tree, &keys](std::size_t begin, std::size_t end) {
                                     for (std::size_t i = begin; i < end; ++i) {
                                       tree.insert(keys[i]);
                                     }
                                   },
                                   from, to));
    }
    for (std::size_t i = 0; i < workers.size(); ++i) {
      workers[i].get();
    }
    const std::chrono::steady_clock::time_point stop =
        std::chrono::steady_clock::now();

    std::size_t found = 0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (tree.contains(keys[i])) {
        ++found;
      }
    }

    const std::chrono::microseconds elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
    std::cout << "baseline_insert_us=" << elapsed.count() << '\n';
    std::cout << "baseline_found=" << found << "/" << keys.size() << '\n';
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "baseline_benchmark error: " << ex.what() << '\n';
    return 1;
  }
}
