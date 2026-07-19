#pragma once

#include <memory>
#include <utility>
#include <vector>

namespace hp {

// Copying this container only shares immutable storage. Replacing its contents
// publishes a new vector, so readers can iterate without copying or observing
// partial updates.
template <typename T>
class SharedImmutableVector final {
 public:
  using Storage = std::vector<T>;
  using const_iterator = typename Storage::const_iterator;
  using const_reverse_iterator = typename Storage::const_reverse_iterator;

  SharedImmutableVector() noexcept : storage_(EmptyStorage()) {}
  SharedImmutableVector(const SharedImmutableVector&) noexcept = default;
  SharedImmutableVector(SharedImmutableVector&&) noexcept = default;
  SharedImmutableVector& operator=(const SharedImmutableVector&) noexcept = default;
  SharedImmutableVector& operator=(SharedImmutableVector&&) noexcept = default;

  explicit SharedImmutableVector(Storage values)
      : storage_(std::make_shared<const Storage>(std::move(values))) {}

  SharedImmutableVector& operator=(Storage values) {
    storage_ = std::make_shared<const Storage>(std::move(values));
    return *this;
  }

  [[nodiscard]] bool empty() const noexcept { return storage_->empty(); }
  [[nodiscard]] size_t size() const noexcept { return storage_->size(); }
  [[nodiscard]] const void* identity() const noexcept { return storage_.get(); }
  [[nodiscard]] const_iterator begin() const noexcept { return storage_->begin(); }
  [[nodiscard]] const_iterator end() const noexcept { return storage_->end(); }
  [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
    return storage_->rbegin();
  }
  [[nodiscard]] const_reverse_iterator rend() const noexcept {
    return storage_->rend();
  }

  friend bool operator==(const SharedImmutableVector& left,
                         const SharedImmutableVector& right) noexcept {
    return left.storage_ == right.storage_;
  }

 private:
  static const std::shared_ptr<const Storage>& EmptyStorage() {
    static const auto empty = std::make_shared<const Storage>();
    return empty;
  }

  std::shared_ptr<const Storage> storage_;
};

}  // namespace hp
