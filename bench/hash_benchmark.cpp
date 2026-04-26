#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include "f3tree/config.h"
#include "f3tree/buffered_hash_index.h"

namespace {

struct Options {
  std::size_t key_count = 100000;
};

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-n" && i + 1 < argc) {
      options.key_count = static_cast<std::size_t>(std::stoull(argv[++i]));
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
    f3tree::BufferedHashIndex index(options.key_count == 0 ? 1 : options.key_count);

    const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < options.key_count; ++i) {
      index.upsert(static_cast<f3tree::Key>(i + 1), static_cast<int>(i % 8));
    }
    std::size_t hits = 0;
    for (std::size_t i = 0; i < options.key_count; ++i) {
      if (index.contains(static_cast<f3tree::Key>(i + 1))) {
        ++hits;
      }
    }
    const std::chrono::steady_clock::time_point stop =
        std::chrono::steady_clock::now();

    const std::chrono::microseconds elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
    std::cout << "hash_elapsed_us=" << elapsed.count() << '\n';
    std::cout << "hash_hits=" << hits << "/" << options.key_count << '\n';
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "hash_benchmark error: " << ex.what() << '\n';
    return 1;
  }
}
