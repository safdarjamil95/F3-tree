//
//  rate_limit.h
//  YCSB-cpp
//
//  Copyright (c) 2023 Youngjae Lee <ls4154.lee@gmail.com>.
//

#ifndef YCSB_C_RATE_LIMIT_H_
#define YCSB_C_RATE_LIMIT_H_

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

namespace ycsbc {

namespace utils {

// Token bucket rate limiter for single client.
// Units: rate_ = tokens/sec, bucket_ = tokens.
class RateLimiter {
 public:
  RateLimiter(int64_t r, int64_t b)
      : rate_(static_cast<double>(r)),
        bucket_(static_cast<double>(b)),
        tokens_(0.0),
        last_(Clock::now()) {}

  inline void Consume(int64_t n) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (rate_ <= 0.0) {
      return;
    }

    RefillLocked();

    // check tokens
    tokens_ -= static_cast<double>(n);

    // sleep
    if (tokens_ < 0.0) {
      lock.unlock();
      const auto wait_time = Duration(-tokens_ / rate_);
      std::this_thread::sleep_for(wait_time);
    }
  }

  inline void SetRate(int64_t r) {
    std::lock_guard<std::mutex> lock(mutex_);

    RefillLocked();
    rate_ = static_cast<double>(r);
  }

 private:
  using Clock = std::chrono::steady_clock;
  using Duration = std::chrono::duration<double>;

  inline void RefillLocked() {
    auto now = Clock::now();
    Duration diff = now - last_;
    tokens_ = std::min(bucket_, tokens_ + diff.count() * rate_);
    last_ = now;
  }

  std::mutex mutex_;
  double rate_;
  double bucket_;
  double tokens_;
  Clock::time_point last_;
};

} // utils

} // ycsbc

#endif // YCSB_C_RATE_LIMIT_H_
