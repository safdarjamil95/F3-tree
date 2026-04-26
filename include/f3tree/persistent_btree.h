#pragma once

#include <memory>

#include "f3tree/config.h"

namespace f3tree {

class PersistentBTree {
 public:
  explicit PersistentBTree(const RuntimeConfig& config);
  ~PersistentBTree();

  PersistentBTree(const PersistentBTree&) = delete;
  PersistentBTree& operator=(const PersistentBTree&) = delete;

  void insert(Key key);
  bool contains(Key key) const;
  void erase(Key key);

  const RuntimeConfig& config() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace f3tree
