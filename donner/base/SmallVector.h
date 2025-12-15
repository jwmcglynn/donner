#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <new>
#include <ostream>
#include <utility>

namespace donner {

/**
 * A vector with small-size optimization.
 *
 * This vector can store a small number of elements on the stack. If the number of elements exceeds
 * the stack capacity, it will allocate memory on the heap. This can reduce memory allocations for
 * small vectors.
 *
 * @tparam T Type of elements stored in the vector.
 * @tparam DefaultSize Number of elements that can be stored on the stack.
 */
template <typename T, std::size_t DefaultSize>
class SmallVector {
public:
  using value_type = T;              //!< Type of elements stored in the vector.
  using size_type = std::size_t;     //!< Type used to represent the size of the vector.
  using reference = T&;              //!< Reference to an element in the vector.
  using const_reference = const T&;  //!< Const reference to an element in the vector.
  using iterator = T*;               //!< Iterator to an element in the vector.
  using const_iterator = const T*;   //!< Const iterator to an element in the vector.

  /**
   * Constructs an empty SmallVector.
   */
  SmallVector() noexcept : size_(0), capacity_(DefaultSize), isLong_(false) {}

  /**
   * Constructs a SmallVector with the elements from the initializer list.
   *
   * @param init Initializer list to initialize the elements of the vector.
   */
  SmallVector(std::initializer_list<T> init) : size_(0), capacity_(DefaultSize), isLong_(false) {
    ensureCapacity(init.size());

    for (const auto& value : init) {
      push_back(value);
    }
  }

  /**
   * Destructor for SmallVector.
   */
  ~SmallVector() {
    clear();
    if (isLong_) {
      ::operator delete(data_.longData);
    }
  }

  /**
   * Copy constructor.
   *
   * @param other The SmallVector to copy from.
   */
  SmallVector(const SmallVector& other) : size_(0), capacity_(DefaultSize), isLong_(false) {
    ensureCapacity(other.size_);
    for (std::size_t i = 0; i < other.size_; ++i) {
      push_back(other[i]);
    }
  }

  /**
   * Copy assignment operator.
   *
   * @param other The SmallVector to copy from.
   * @return Reference to this SmallVector.
   */
  SmallVector& operator=(const SmallVector& other) {
    if (this != &other) {
      clear();
      ensureCapacity(other.size_);
      for (std::size_t i = 0; i < other.size_; ++i) {
        push_back(other[i]);
      }
    }
    return *this;
  }

  /**
   * Move constructor.
   *
   * @param other The SmallVector to move from.
   */
  SmallVector(SmallVector&& other) noexcept
      : size_(other.size_), capacity_(other.capacity_), isLong_(other.isLong_) {
    if (isLong_) {
      data_.longData = other.data_.longData;
      other.data_.longData = nullptr;
    } else {
      for (size_t i = 0; i < size_; ++i) {
        new (reinterpret_cast<T*>(&data_.shortData[i]))
            T(std::move(*reinterpret_cast<T*>(&other.data_.shortData[i])));
        if constexpr (!std::is_trivially_destructible_v<T>) {
          reinterpret_cast<T*>(&other.data_.shortData[i])->~T();
        }
      }
    }
    other.size_ = 0;
    other.capacity_ = DefaultSize;
    other.isLong_ = false;
  }

  /**
   * Move assignment operator.
   *
   * @param other The SmallVector to move from.
   * @return Reference to this SmallVector.
   */
  SmallVector& operator=(SmallVector&& other) noexcept {
    if (this != &other) {
      clear();
      if (isLong_) {
        ::operator delete(data_.longData);
      }

      size_ = other.size_;
      capacity_ = other.capacity_;
      isLong_ = other.isLong_;
      if (isLong_) {
        data_.longData = other.data_.longData;
        other.data_.longData = nullptr;
      } else {
        for (size_t i = 0; i < size_; ++i) {
          new (reinterpret_cast<T*>(&data_.shortData[i]))
              T(std::move(*reinterpret_cast<T*>(&other.data_.shortData[i])));
          if constexpr (!std::is_trivially_destructible_v<T>) {
            reinterpret_cast<T*>(&other.data_.shortData[i])->~T();
          }
        }
      }
      other.size_ = 0;
      other.capacity_ = DefaultSize;
      other.isLong_ = false;
    }
    return *this;
  }

  /**
   * Adds an element to the end of the vector.
   *
   * @param value The value to add.
   */
  void push_back(const T& value) {
    ensureCapacity(size_ + 1);

    new (data() + size_) T(value);
    ++size_;
  }

  /**
   * Constructs an element in-place at the end of the vector.
   *
   * @tparam Args Types of the arguments to forward to the constructor.
   * @param args Arguments to forward to the constructor of the element.
   * @return Reference to the constructed element.
   */
  template <typename... Args>
  T& emplace_back(Args&&... args) {
    ensureCapacity(size_ + 1);

    new (data() + size_) T(std::forward<Args>(args)...);
    ++size_;
    return data()[size_ - 1];
  }

  /**
   * Removes the last element from the vector.
   */
  void pop_back() noexcept {
    if (size_ > 0) {
      --size_;
      if constexpr (!std::is_trivially_destructible_v<T>) {
        data()[size_].~T();
      }
    }
  }

  /**
   * Inserts an element at the specified position.
   *
   * @param pos Iterator to the position where the element will be inserted.
   * @param value The value to insert.
   * @return Iterator to the inserted element.
   */
  iterator insert(const_iterator pos, const T& value) {
    size_t index = pos - begin();
    if (index > size_) {
      index = size_;
    }

    // Ensure we have capacity for one more element
    ensureCapacity(size_ + 1);

    // If we're not inserting at the end, shift elements to make room
    if (index < size_) {
      // Move existing elements forward by one position
      for (size_t i = size_; i > index; --i) {
        new (data() + i) T(std::move(data()[i - 1]));
        if constexpr (!std::is_trivially_destructible_v<T>) {
          data()[i - 1].~T();
        }
      }
    }

    // Place the new element
    new (data() + index) T(value);
    ++size_;

    return begin() + index;
  }

  /**
   * Clears the contents of the vector.
   */
  void clear() noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      for (std::size_t i = 0; i < size_; ++i) {
        data()[i].~T();
      }
    }
    size_ = 0;
  }

  /**
   * Gets the data stored in the vector.
   */
  T* data() noexcept {
    return isLong_ ? data_.longData : reinterpret_cast<T*>(data_.shortData.data());
  }

  /**
   * Gets the data stored in the vector (const version).
   */
  const T* data() const noexcept {
    return isLong_ ? data_.longData : reinterpret_cast<const T*>(data_.shortData.data());
  }

  /**
   * Returns the number of elements in the vector.
   */
  std::size_t size() const noexcept { return size_; }

  /**
   * Returns the capacity of the vector.
   */
  std::size_t capacity() const noexcept { return capacity_; }

  /**
   * Checks if the vector is empty.
   *
   * @return True if the vector is empty, false otherwise.
   */
  bool empty() const noexcept { return size_ == 0; }

  /**
   * Accesses the element at the specified index.
   *
   * @param index Index of the element to access.
   * @return Reference to the element at the specified index.
   */
  T& operator[](std::size_t index) noexcept(false) {
    assert(index < size_ && "Index out of bounds");
    return data()[index];
  }

  /**
   * Accesses the element at the specified index (const version).
   *
   * @param index Index of the element to access.
   * @return Const reference to the element at the specified index.
   */
  const T& operator[](std::size_t index) const noexcept(false) {
    assert(index < size_ && "Index out of bounds");
    return data()[index];
  }

  /**
   * Returns an iterator to the beginning of the vector.
   */
  iterator begin() noexcept { return data(); }

  /**
   * Returns an iterator to the end of the vector.
   */
  iterator end() noexcept { return data() + size_; }

  /**
   * Returns a const iterator to the beginning of the vector.
   */
  const_iterator begin() const noexcept { return data(); }

  /**
   * Returns a const iterator to the end of the vector.
   */
  const_iterator end() const noexcept { return begin() + size_; }

  /// Ostream output operator for SmallVector.
  friend std::ostream& operator<<(std::ostream& os, const SmallVector<T, DefaultSize>& vec) {
    os << "[";

    for (std::size_t i = 0; i < vec.size(); ++i) {
      if (i > 0) {
        os << ", ";  // Add a comma and space between elements
      }
      os << vec[i];
    }

    os << "]";

    return os;
  }

private:
  /**
   * Ensures that the vector has enough capacity to store the specified number of elements.
   *
   * @param newCapacity The new capacity to ensure.
   */
  void ensureCapacity(std::size_t newCapacity) {
    if (newCapacity <= capacity_) {
      return;
    }

    std::size_t newCapacityAdjusted = std::max(capacity_ * 2, newCapacity);
    T* newData = reinterpret_cast<T*>(::operator new(newCapacityAdjusted * sizeof(T)));

    T* oldData = isLong_ ? data_.longData : reinterpret_cast<T*>(data_.shortData.data());
    for (std::size_t i = 0; i < size_; ++i) {
      new (newData + i) T(std::move(oldData[i]));

      if constexpr (!std::is_trivially_destructible_v<T>) {
        oldData[i].~T();
      }
    }

    if (isLong_) {
      ::operator delete(data_.longData);
    }

    data_.longData = newData;
    isLong_ = true;
    capacity_ = newCapacityAdjusted;
  }

  std::size_t size_;      //!< Number of elements in the vector.
  std::size_t capacity_;  //!< Capacity of the vector.
  bool isLong_;           //!< True if the vector is using the long data storage.

  /**
   * Union to store the data for the vector.
   */
  union Data {
    alignas(T) std::array<std::byte, sizeof(T) * DefaultSize>
        shortData;  //!< Data storage for small vectors.
    T* longData;    //!< Data storage for large vectors.

    Data() : longData(nullptr) {}
    ~Data() {}

    Data(const Data&) = delete;
    Data& operator=(const Data&) = delete;
    Data(Data&&) = delete;
    Data& operator=(Data&&) = delete;
  };

  Data data_;  //!< Data storage for the vector.
};

}  // namespace donner
