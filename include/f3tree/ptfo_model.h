#pragma once

#include <cstddef>

namespace f3tree {

// Per-thread PTFO bookkeeping used by the FutureBTree wrapper layer.
struct PTFOThreadState {
  std::size_t node_count = 0;
  std::size_t entry_count = 0;
  std::size_t home_node = 0;
};

}  // namespace f3tree
