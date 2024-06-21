#pragma once

#include <array>
#include <vector>
#include <cstddef>
#include <algorithm>
#include <initializer_list>
#include <utility>

namespace donner {

template<typename T, std::size_t DefaultSize>
class SmallVector {
public:
    SmallVector() : size_(0), capacity_(DefaultSize), isLong_(false) {}

    SmallVector(std::initializer_list<T> init) : SmallVector() {
        for (const auto& value : init) {
            push_back(value);
        }
    }

    ~SmallVector() {
        if (isLong_) {
            delete[] data_.longData;
        }
    }

    SmallVector(const SmallVector& other) : size_(other.size_), capacity_(other.capacity_), isLong_(other.isLong_) {
        if (isLong_) {
            data_.longData = new T[capacity_];
            std::copy(other.data_.longData, other.data_.longData + size_, data_.longData);
        } else {
            std::copy(other.data_.shortData.begin(), other.data_.shortData.begin() + size_, data_.shortData.begin());
        }
    }

    SmallVector& operator=(const SmallVector& other) {
        if (this != &other) {
            if (isLong_) {
                delete[] data_.longData;
            }

            size_ = other.size_;
            capacity_ = other.capacity_;
            isLong_ = other.isLong_;

            if (isLong_) {
                data_.longData = new T[capacity_];
                std::copy(other.data_.longData, other.data_.longData + size_, data_.longData);
            } else {
                std::copy(other.data_.shortData.begin(), other.data_.shortData.begin() + size_, data_.shortData.begin());
            }
        }
        return *this;
    }

    SmallVector(SmallVector&& other) noexcept : size_(other.size_), capacity_(other.capacity_), isLong_(other.isLong_), data_(other.data_) {
        other.size_ = 0;
        other.capacity_ = DefaultSize;
        other.isLong_ = false;
        other.data_.longData = nullptr; // Prevent double-free
    }

    SmallVector& operator=(SmallVector&& other) noexcept {
        if (this != &other) {
            if (isLong_) {
                delete[] data_.longData;
            }

            size_ = other.size_;
            capacity_ = other.capacity_;
            isLong_ = other.isLong_;
            data_ = other.data_;

            other.size_ = 0;
            other.capacity_ = DefaultSize;
            other.isLong_ = false;
            other.data_.longData = nullptr; // Prevent double-free
        }
        return *this;
    }

    void push_back(const T& value) {
        ensureCapacity(size_ + 1);
        if (isLong_) {
            data_.longData[size_] = value;
        } else {
            data_.shortData[size_] = value;
        }
        ++size_;
    }

    void clear() {
        size_ = 0;
    }

    std::size_t size() const {
        return size_;
    }

    bool empty() const {
        return size_ == 0;
    }

    T& operator[](std::size_t index) {
        return isLong_ ? data_.longData[index] : data_.shortData[index];
    }

    const T& operator[](std::size_t index) const {
        return isLong_ ? data_.longData[index] : data_.shortData[index];
    }

private:
    void ensureCapacity(std::size_t newCapacity) {
        if (newCapacity <= capacity_) return;

        if (!isLong_ && newCapacity <= DefaultSize) {
            capacity_ = DefaultSize;
        } else {
            isLong_ = true;
            capacity_ = std::max(capacity_ * 2, newCapacity);
            T* newData = new T[capacity_];
            if (size_ > 0) {
                std::copy(begin(), end(), newData);
            }
            if (size_ > DefaultSize) {
                delete[] data_.longData;
            }
            data_.longData = newData;
        }
    }

    T* begin() {
        return isLong_ ? data_.longData : data_.shortData.data();
    }

    T* end() {
        return begin() + size_;
    }

    const T* begin() const {
        return isLong_ ? data_.longData : data_.shortData.data();
    }

    const T* end() const {
        return begin() + size_;
    }

    std::size_t size_;
    std::size_t capacity_;
    bool isLong_;

    union Data {
        std::array<T, DefaultSize> shortData;
        T* longData;

        Data() {}
        ~Data() {}
    } data_;
};

} // namespace donner
