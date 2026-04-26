#include "f3tree_db.h"

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

#include "../core/db_factory.h"

namespace ycsbc {

// ── shared state ────────────────────────────────────────────────────────────

f3tree::FutureBTree    *F3TreeDB::tree_            = nullptr;
std::mutex              F3TreeDB::global_mutex_;
std::atomic<int>        F3TreeDB::instance_count_{0};
std::atomic<int>        F3TreeDB::next_producer_id_{0};

// ── helpers ─────────────────────────────────────────────────────────────────

// YCSB keys are "user" + zero-padded decimal number (e.g. "user0000000001").
// Parse the numeric suffix directly to avoid hash collisions in the key space.
f3tree::Key F3TreeDB::StringToKey(const std::string &key) {
  const char *p = key.c_str();
  std::size_t n = key.size();
  std::size_t i = 0;
  while (i < n && (p[i] < '0' || p[i] > '9')) ++i;
  if (i == n) {
    // No digit suffix — fall back to hash (unusual workloads)
    return static_cast<f3tree::Key>((std::hash<std::string>{}(key) >> 1) + 1);
  }
  // Parse as uint64_t — YCSB uses utils::Hash which can produce values > INT64_MAX.
  uint64_t v = 0;
  while (i < n && p[i] >= '0' && p[i] <= '9') {
    v = v * 10 + static_cast<uint64_t>(p[i] - '0');
    ++i;
  }
  // Right-shift by 1 to fit in positive int64_t, then +1 to avoid the null sentinel (0).
  return static_cast<f3tree::Key>((v >> 1) + 1);
}

f3tree::RuntimeConfig F3TreeDB::BuildConfig(utils::Properties *props) {
  f3tree::RuntimeConfig cfg;

  int nthreads = std::stoi(props->GetProperty("threadcount", "1"));
  cfg.producer_threads = static_cast<std::size_t>(nthreads);

  cfg.evaluator_threads = static_cast<std::size_t>(
      std::stoi(props->GetProperty("f3tree.evaluator_threads", "1")));

  cfg.hash_capacity = static_cast<std::size_t>(
      std::stoul(props->GetProperty("f3tree.hash_capacity", "1024")));

  cfg.checkpoint_threshold_ops = static_cast<std::size_t>(
      std::stoul(props->GetProperty("f3tree.checkpoint_threshold_ops", "0")));

  cfg.checkpoint_interval_us = static_cast<std::uint64_t>(
      std::stoull(props->GetProperty("f3tree.checkpoint_interval_us", "0")));

  const std::string policy = props->GetProperty("f3tree.evaluator_policy", "f3n");
  if (policy == "f3k") {
    cfg.evaluator_policy = f3tree::EvaluatorPolicy::kF3K;
  } else {
    cfg.evaluator_policy = f3tree::EvaluatorPolicy::kF3N;
  }

  cfg.persistent_path = props->GetProperty("f3tree.persistent_path", "");

  if (!cfg.persistent_path.empty()) {
    cfg.persistent_pool_bytes = static_cast<std::uint64_t>(
        std::stoull(props->GetProperty("f3tree.persistent_pool_bytes",
                                       std::to_string(512ull * 1024 * 1024))));
  }

  return cfg;
}

// ── lifecycle ────────────────────────────────────────────────────────────────

void F3TreeDB::Init() {
  std::lock_guard<std::mutex> lock(global_mutex_);
  if (!tree_) {
    tree_ = new f3tree::FutureBTree(BuildConfig(props_));
  }
  producer_id_ = static_cast<std::size_t>(next_producer_id_.fetch_add(1));
  instance_count_.fetch_add(1);
}

void F3TreeDB::Cleanup() {
  std::lock_guard<std::mutex> lock(global_mutex_);
  if (instance_count_.fetch_sub(1) == 1) {
    tree_->drain_pending();
    delete tree_;
    tree_            = nullptr;
    next_producer_id_.store(0);
  }
}

// ── operations ───────────────────────────────────────────────────────────────

DB::Status F3TreeDB::Read(const std::string & /*table*/, const std::string &key,
                          const std::vector<std::string> * /*fields*/,
                          std::vector<Field> &result) {
  if (!tree_->contains(StringToKey(key))) return kNotFound;
  result.push_back({"value", key});
  return kOK;
}

DB::Status F3TreeDB::Scan(const std::string & /*table*/,
                          const std::string &key, int record_count,
                          const std::vector<std::string> * /*fields*/,
                          std::vector<std::vector<Field>> &result) {
  // Scan drains all pending PTFO state into the global B+-tree before reading.
  // See scan_range() in future_btree.h for the cost model.
  std::vector<f3tree::Key> found;
  std::size_t n = tree_->scan_range(StringToKey(key), record_count, found);
  result.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    result[i].push_back({"value", std::to_string(found[i])});
  }
  return kOK;
}

DB::Status F3TreeDB::Update(const std::string & /*table*/,
                            const std::string &key,
                            std::vector<Field> & /*values*/) {
  // F3-tree is a key-existence index; re-insert suffices for update semantics.
  tree_->buffered_insert(StringToKey(key), producer_id_);
  return kOK;
}

DB::Status F3TreeDB::Insert(const std::string & /*table*/,
                            const std::string &key,
                            std::vector<Field> & /*values*/) {
  tree_->buffered_insert(StringToKey(key), producer_id_);
  return kOK;
}

DB::Status F3TreeDB::Delete(const std::string & /*table*/,
                            const std::string &key) {
  tree_->erase_buffered(StringToKey(key), producer_id_);
  return kOK;
}

// ── registration ─────────────────────────────────────────────────────────────

DB *NewF3TreeDB() { return new F3TreeDB(); }

const bool kRegistered = DBFactory::RegisterDB("f3tree", NewF3TreeDB);

}  // namespace ycsbc
