#pragma once

#include <string>

namespace f3tree {

inline bool supported_platform() {
#if defined(__linux__) && defined(__x86_64__)
  return true;
#else
  return false;
#endif
}

inline std::string unsupported_platform_message() {
  return "F3-tree benchmarks target Linux/x86-64 with a GCC-compatible toolchain.";
}

}  // namespace f3tree
