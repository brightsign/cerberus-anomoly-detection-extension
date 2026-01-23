#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace wvm {

template<typename T>
class TsQueue {
public:
  explicit TsQueue(size_t max_size) : max_size_(max_size) {}

  void stop() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stopped_ = true;
    }
    cv_not_empty_.notify_all();
    cv_not_full_.notify_all();
  }

  bool push(T&& item) {
    std::unique_lock<std::mutex> lk(m_);
    cv_not_full_.wait(lk, [&]{ return stopped_ || q_.size() < max_size_; });
    if (stopped_) return false;
    q_.emplace_back(std::move(item));
    cv_not_empty_.notify_one();
    return true;
  }

  std::optional<T> pop() {
    std::unique_lock<std::mutex> lk(m_);
    cv_not_empty_.wait(lk, [&]{ return stopped_ || !q_.empty(); });
    if (q_.empty()) return std::nullopt;
    T item = std::move(q_.front());
    q_.pop_front();
    cv_not_full_.notify_one();
    return item;
  }

private:
  size_t max_size_;
  std::mutex m_;
  std::condition_variable cv_not_empty_;
  std::condition_variable cv_not_full_;
  std::deque<T> q_;
  bool stopped_ = false;
};

} // namespace wvm
