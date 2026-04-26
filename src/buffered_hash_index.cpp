#include "f3tree/buffered_hash_index.h"

namespace f3tree {

BufferedHashIndex::BufferedHashIndex(std::size_t capacity)
    : slots_(capacity == 0 ? 1 : capacity) {}

void BufferedHashIndex::upsert(std::int64_t key, int owner) {
  const std::size_t index = find_or_allocate_slot(key);
  const bool was_new =
      !slots_[index].occupied || slots_[index].state == BufferedEntryState::kAbsent;
  slots_[index].occupied = true;
  slots_[index].key = key;
  slots_[index].owner = owner;
  slots_[index].state = BufferedEntryState::kPresent;
  slots_[index].sequence = next_sequence_++;
  if (was_new) {
    ++size_;
  }
}

void BufferedHashIndex::erase(std::int64_t key, int owner) {
  const std::size_t index = find_or_allocate_slot(key);
  const bool was_new =
      !slots_[index].occupied || slots_[index].state == BufferedEntryState::kAbsent;
  slots_[index].occupied = true;
  slots_[index].key = key;
  slots_[index].owner = owner;
  slots_[index].state = BufferedEntryState::kTombstone;
  slots_[index].sequence = next_sequence_++;
  if (was_new) {
    ++size_;
  }
}

bool BufferedHashIndex::contains(std::int64_t key) const {
  return state_for(key) == BufferedEntryState::kPresent;
}

bool BufferedHashIndex::has_entry(std::int64_t key) const {
  return state_for(key) != BufferedEntryState::kAbsent;
}

int BufferedHashIndex::owner_for(std::int64_t key) const {
  const std::size_t index = find_slot(key);
  return index == slots_.size() || slots_[index].state == BufferedEntryState::kAbsent
             ? -1
             : slots_[index].owner;
}

BufferedEntryState BufferedHashIndex::state_for(std::int64_t key) const {
  const std::size_t index = find_slot(key);
  return index == slots_.size() ? BufferedEntryState::kAbsent
                                : slots_[index].state;
}

std::vector<BufferedEntry> BufferedHashIndex::entries() const {
  std::vector<BufferedEntry> buffered_entries;
  buffered_entries.reserve(size_);
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (!slots_[i].occupied || slots_[i].state == BufferedEntryState::kAbsent) {
      continue;
    }
    BufferedEntry entry;
    entry.key = slots_[i].key;
    entry.owner = slots_[i].owner;
    entry.state = slots_[i].state;
    entry.sequence = slots_[i].sequence;
    buffered_entries.push_back(entry);
  }
  return buffered_entries;
}

void BufferedHashIndex::clear_entry(std::int64_t key) {
  const std::size_t index = find_slot(key);
  if (index == slots_.size() || slots_[index].state == BufferedEntryState::kAbsent) {
    return;
  }

  slots_[index].owner = -1;
  slots_[index].state = BufferedEntryState::kAbsent;
  slots_[index].sequence = 0;
  if (size_ > 0) {
    --size_;
  }
}

std::size_t BufferedHashIndex::size() const { return size_; }

std::size_t BufferedHashIndex::capacity() const { return slots_.size(); }

void BufferedHashIndex::clear() {
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    slots_[i] = Slot();
  }
  size_ = 0;
  next_sequence_ = 1;
}

std::size_t BufferedHashIndex::normalize(std::int64_t key) const {
  const std::int64_t mod =
      static_cast<std::int64_t>(slots_.size() == 0 ? 1 : slots_.size());
  const std::int64_t normalized = key % mod;
  return static_cast<std::size_t>(normalized < 0 ? normalized + mod : normalized);
}

std::size_t BufferedHashIndex::find_slot(std::int64_t key) const {
  if (slots_.empty()) {
    return slots_.size();
  }

  std::size_t index = normalize(key);
  const std::size_t start = index;

  while (slots_[index].occupied) {
    if (slots_[index].key == key) {
      return index;
    }
    index = (index + 1) % slots_.size();
    if (index == start) {
      break;
    }
  }

  return slots_.size();
}

std::size_t BufferedHashIndex::find_or_allocate_slot(std::int64_t key) {
  const std::size_t existing = find_slot(key);
  if (existing != slots_.size()) {
    return existing;
  }

  std::size_t index = normalize(key);
  const std::size_t start = index;
  do {
    if (!slots_[index].occupied) {
      return index;
    }
    index = (index + 1) % slots_.size();
  } while (index != start);

  return slots_.size();
}

}  // namespace f3tree
