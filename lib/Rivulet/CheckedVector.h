#pragma once

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

#if defined(BOARD_HAS_PSRAM) && defined(ESP32)
#include <esp_heap_caps.h>
#endif

namespace rivulet {

// Only ordinary reader data lives here, never DMA buffers, stacks or ISR data.
// PSRAM-equipped boards try external RAM first; either allocator may fail without
// destroying the old allocation. Unlike vector::reserve under -fno-exceptions,
// callers receive false instead of aborting the firmware.
inline void* resizeReaderStorage(void* old, size_t bytes) {
#if defined(BOARD_HAS_PSRAM) && defined(ESP32)
  if (void* p = heap_caps_realloc(old, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return p;
#endif
  return std::realloc(old, bytes);
}

// Small fallible owner for trivially copyable IR/index records. Not a general
// STL replacement: no strings, custom destructors, or exception-based growth.
template <typename T>
class CheckedVector {
  static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
  static_assert(std::is_nothrow_default_constructible_v<T>);
 public:
  CheckedVector() = default;
  ~CheckedVector() { release(); }
  CheckedVector(const CheckedVector&) = delete;
  CheckedVector& operator=(const CheckedVector&) = delete;
  CheckedVector(CheckedVector&& other) noexcept { swap(other); }
  CheckedVector& operator=(CheckedVector&& other) noexcept {
    if (this != &other) { release(); swap(other); }
    return *this;
  }
  void swap(CheckedVector& other) noexcept {
    std::swap(data_, other.data_);
    std::swap(size_, other.size_);
    std::swap(capacity_, other.capacity_);
  }
  [[nodiscard]] bool reserve(size_t n) {
    if (n <= capacity_) return true;
    if (n > std::numeric_limits<size_t>::max() / sizeof(T)) return false;
    void* p = resizeReaderStorage(data_, n * sizeof(T));
    if (!p) return false;
    data_ = static_cast<T*>(p);
    capacity_ = n;
    return true;
  }
  [[nodiscard]] bool resize(size_t n) {
    if (!reserve(n)) return false;
    for (size_t i = size_; i < n; ++i) ::new (static_cast<void*>(data_ + i)) T{};
    size_ = n;
    return true;
  }
  [[nodiscard]] bool push_back(const T& value) {
    // A caller may append an existing element; preserve it across realloc.
    const T copy = value;
    if (size_ == capacity_) {
      const size_t max = std::numeric_limits<size_t>::max() / sizeof(T);
      if (size_ == max) return false;
      const size_t grown = capacity_ == 0 ? 8 : (capacity_ > max / 2 ? max : capacity_ * 2);
      if (!reserve(grown)) return false;
    }
    ::new (static_cast<void*>(data_ + size_)) T(copy);
    ++size_;
    return true;
  }
  // Source belongs to a different owner (used for per-line token scratch).
  [[nodiscard]] bool assignRange(const T* source, size_t count) {
    if (count != 0 && !source) return false;
    if (!reserve(count)) return false;
    for (size_t i = 0; i < count; ++i) ::new (static_cast<void*>(data_ + i)) T(source[i]);
    size_ = count;
    return true;
  }
  void pop_back() { if (size_ > 0) --size_; }
  void clear() { size_ = 0; }
  void release() {
    std::free(data_);
    data_ = nullptr;
    size_ = capacity_ = 0;
  }
  [[nodiscard]] size_t size() const { return size_; }
  [[nodiscard]] size_t capacity() const { return capacity_; }
  [[nodiscard]] bool empty() const { return size_ == 0; }
  T* data() { return data_; }
  const T* data() const { return data_; }
  T* begin() { return data_; }
  const T* begin() const { return data_; }
  T* end() { return data_ ? data_ + size_ : nullptr; }
  const T* end() const { return data_ ? data_ + size_ : nullptr; }
  T& operator[](size_t i) { return data_[i]; }
  const T& operator[](size_t i) const { return data_[i]; }
  T& back() { return data_[size_ - 1]; }
  const T& back() const { return data_[size_ - 1]; }
 private:
  T* data_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;
};
}  // namespace rivulet
