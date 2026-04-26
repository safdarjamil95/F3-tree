#include "f3tree/future_btree.h"
#include <iostream>
int main() {
  f3tree::RuntimeConfig config;
  config.producer_threads = 4; config.evaluator_threads = 2; config.hash_capacity = 128;
  config.evaluator_policy = f3tree::EvaluatorPolicy::kDeterministicReplay;
  f3tree::FutureBTree tree(config);
  tree.warmup_insert(500);
  tree.buffered_insert(10, 0);
  tree.buffered_insert(11, 1);
  tree.buffered_insert(12, 2);
  tree.buffered_insert(13, 3);
  tree.erase_buffered(11, 0);   // cross-producer: producer 0 deletes key owned by producer 1
  tree.erase_buffered(500, 1);  // cross-producer: producer 1 deletes warmup key
  tree.buffered_insert(500, 2); // cross-producer: producer 2 re-inserts
  tree.erase_buffered(700, 3);
  std::cout << "before drain contains(11)=" << tree.contains(11) << "\n";
  tree.drain_pending();
  std::cout << "after drain:"
            << " global_contains(11)=" << tree.global_contains(11)
            << " contains(11)=" << tree.contains(11)
            << " global_contains(10)=" << tree.global_contains(10)
            << " global_contains(12)=" << tree.global_contains(12) << "\n";
  return 0;
}
