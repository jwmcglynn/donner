#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <new>
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
  SmallVector() : size_(0), capacity_(DefaultSize), isLong_(false) {}

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
      std::move(other.data_.shortData.begin(), other.data_.shortData.begin() + size_,
                data_.shortData.begin());
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
        std::move(other.data_.shortData.begin(), other.data_.shortData.begin() + size_,
                  data_.shortData.begin());
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
    if (isLong_) {
      new (data_.longData + size_) T(value);
    } else {
      new (&reinterpret_cast<T&>(data_.shortData[size_])) T(value);
    }
    ++size_;
  }

  /**
   * Removes the last element from the vector.
   */
  void pop_back() {
    if (size_ > 0) {
      --size_;
      if (isLong_) {
        data_.longData[size_].~T();
      } else {
        reinterpret_cast<T*>(&data_.shortData[size_])->~T();
      }
    }
  }

  /**
   * Clears the contents of the vector.
   */
  void clear() {
    for (std::size_t i = 0; i < size_; ++i) {
      if (isLong_) {
        data_.longData[i].~T();
      } else {
        reinterpret_cast<T*>(&data_.shortData[i])->~T();
      }
    }
    size_ = 0;
  }

  /**
   * Returns the number of elements in the vector.
   */
  std::size_t size() const { return size_; }

  /**
   * Returns the capacity of the vector.
   */
  std::size_t capacity() const { return capacity_; }

  /**
   * Checks if the vector is empty.
   *
   * @return True if the vector is empty, false otherwise.
   */
  bool empty() const { return size_ == 0; }

  /**
   * Accesses the element at the specified index.
   *
   * @param index Index of the element to access.
   * @return Reference to the element at the specified index.
   */
  T& operator[](std::size_t index) {
    return isLong_ ? data_.longData[index] : reinterpret_cast<T&>(data_.shortData[index]);
  }

  /**
   * Accesses the element at the specified index (const version).
   *
   * @param index Index of the element to access.
   * @return Const reference to the element at the specified index.
   */
  const T& operator[](std::size_t index) const {
    return isLong_ ? data_.longData[index] : reinterpret_cast<const T&>(data_.shortData[index]);
  }

  /**
   * Returns an iterator to the beginning of the vector.
   */
  T* begin() { return isLong_ ? data_.longData : reinterpret_cast<T*>(data_.shortData.data()); }

  /**
   * Returns an iterator to the end of the vector.
   */
  T* end() { return begin() + size_; }

  /**
   * Returns a const iterator to the beginning of the vector.
   */
  const T* begin() const {
    return isLong_ ? data_.longData : reinterpret_cast<const T*>(data_.shortData.data());
  }

  /**
   * Returns a const iterator to the end of the vector.
   */
  const T* end() const { return begin() + size_; }

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

    if (size_ > 0) {
      if (isLong_) {
        std::move(data_.longData, data_.longData + size_, newData);
        for (std::size_t i = 0; i < size_; ++i) {
          data_.longData[i].~T();
        }
      } else {
        std::move(reinterpret_cast<T*>(data_.shortData.data()),
                  reinterpret_cast<T*>(data_.shortData.data()) + size_, newData);
        for (std::size_t i = 0; i < size_; ++i) {
          reinterpret_cast<T*>(data_.shortData.data())[i].~T();
        }
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
    std::array<std::aligned_storage_t<sizeof(T), alignof(T)>, DefaultSize>
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
