#include "f3tree/workload.h"

#include <fstream>
#include <random>
#include <stdexcept>

namespace f3tree {

std::vector<Key> make_sequential_keys(std::size_t count) {
  std::vector<Key> keys(count);
  for (std::size_t i = 0; i < count; ++i) {
    keys[i] = static_cast<Key>(i + 1);
  }
  return keys;
}

std::vector<Key> make_shuffled_keys(std::size_t count, std::uint64_t seed) {
  std::vector<Key> keys = make_sequential_keys(count);
  std::mt19937_64 rng(seed);
  for (std::size_t i = keys.size(); i > 1; --i) {
    const std::size_t j = static_cast<std::size_t>(rng() % i);
    const Key tmp = keys[i - 1];
    keys[i - 1] = keys[j];
    keys[j] = tmp;
  }
  return keys;
}

std::vector<Key> load_keys_from_file(const std::string& path, std::size_t count) {
  std::ifstream input(path.c_str());
  if (!input) {
    throw std::runtime_error("failed to open input file: " + path);
  }

  std::vector<Key> keys;
  keys.reserve(count);
  Key key = 0;
  while (keys.size() < count && input >> key) {
    keys.push_back(key);
  }

  if (keys.size() != count) {
    throw std::runtime_error("input file does not contain enough keys: " + path);
  }

  return keys;
}

std::vector<Key> load_or_generate_keys(const std::string& path,
                                       std::size_t count,
                                       std::uint64_t seed) {
  if (!path.empty()) {
    return load_keys_from_file(path, count);
  }
  return make_shuffled_keys(count, seed);
}

bool write_keys_to_file(const std::string& path, const std::vector<Key>& keys) {
  std::ofstream output(path.c_str());
  if (!output) {
    return false;
  }

  for (std::size_t i = 0; i < keys.size(); ++i) {
    output << keys[i] << '\n';
  }
  return true;
}

}  // namespace f3tree
