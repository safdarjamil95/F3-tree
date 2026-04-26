#pragma once

// BufferedHashIndex is a standalone benchmark harness used by
// bench/hash_benchmark.cpp. It is NOT part of the FutureBTree read/write
// path — per-producer buffered visibility is handled by the PTFO chain in
// core/btree.h and the per-producer lookup directory.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace f3tree {

enum class BufferedEntryState {
  kAbsent,
  kPresent,
  kTombstone,
};

struct BufferedEntry {
  std::int64_t key = 0;
  int owner = -1;
  BufferedEntryState state = BufferedEntryState::kAbsent;
  std::uint64_t sequence = 0;
};

class BufferedHashIndex {
 public:
  explicit BufferedHashIndex(std::size_t capacity = 1024);

  void upsert(std::int64_t key, int owner);
  void erase(std::int64_t key, int owner);
  bool contains(std::int64_t key) const;
  bool has_entry(std::int64_t key) const;
  int owner_for(std::int64_t key) const;
  BufferedEntryState state_for(std::int64_t key) const;
  std::vector<BufferedEntry> entries() const;
  void clear_entry(std::int64_t key);
  std::size_t size() const;
  std::size_t capacity() const;
  void clear();

 private:
  struct Slot {
    bool occupied = false;
    std::int64_t key = 0;
    int owner = -1;
    BufferedEntryState state = BufferedEntryState::kAbsent;
    std::uint64_t sequence = 0;
  };

  std::vector<Slot> slots_;
  std::size_t size_ = 0;
  std::uint64_t next_sequence_ = 1;

  std::size_t normalize(std::int64_t key) const;
  std::size_t find_slot(std::int64_t key) const;
  std::size_t find_or_allocate_slot(std::int64_t key);
};

}  // namespace f3tree
