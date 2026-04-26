#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "f3tree/config.h"

namespace f3tree {

std::vector<Key> make_sequential_keys(std::size_t count);
std::vector<Key> make_shuffled_keys(std::size_t count, std::uint64_t seed);
std::vector<Key> load_keys_from_file(const std::string& path, std::size_t count);
std::vector<Key> load_or_generate_keys(const std::string& path,
                                       std::size_t count,
                                       std::uint64_t seed);
bool write_keys_to_file(const std::string& path, const std::vector<Key>& keys);

}  // namespace f3tree
