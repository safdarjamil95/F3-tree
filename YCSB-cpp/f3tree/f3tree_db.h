#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "../core/db.h"
#include "f3tree/future_btree.h"
#include "f3tree/config.h"

namespace ycsbc {

class F3TreeDB : public DB {
 public:
  void Init() override;
  void Cleanup() override;

  Status Read(const std::string &table, const std::string &key,
              const std::vector<std::string> *fields,
              std::vector<Field> &result) override;

  Status Scan(const std::string &table, const std::string &key,
              int record_count, const std::vector<std::string> *fields,
              std::vector<std::vector<Field>> &result) override;

  Status Update(const std::string &table, const std::string &key,
                std::vector<Field> &values) override;

  Status Insert(const std::string &table, const std::string &key,
                std::vector<Field> &values) override;

  Status Delete(const std::string &table, const std::string &key) override;

 private:
  static f3tree::FutureBTree *tree_;
  static std::mutex           global_mutex_;
  static std::atomic<int>     instance_count_;
  static std::atomic<int>     next_producer_id_;

  std::size_t producer_id_{0};

  static f3tree::Key StringToKey(const std::string &key);
  static f3tree::RuntimeConfig BuildConfig(utils::Properties *props);
};

DB *NewF3TreeDB();

}  // namespace ycsbc
