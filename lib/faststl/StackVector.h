//
// Created by Hayden Rivas on 10/11/25.
//

#pragma once

#include <cstdint>
#include <vector>
#include <stdexcept>

namespace mythril {
	// StackVector<T> is a vector implementation that requires a max size,
	// as it essentially just wraps a c style array
	// it is not reccomended to use large MaxSize values unless necessary as that memory is allocated immediately
	template <typename T, size_t MaxSize>
	class StackVector {
	public:
		// constructors
		// two others are used to copy vector into the vector
		constexpr StackVector() noexcept : _count(0) {}
		StackVector(std::initializer_list<T> init) : _count(0) {
			if (init.size() > MaxSize) {
				throw std::length_error("std::initializer_list size exceeds FastVector capacity");
			}
			_count = init.size();
			std::copy(init.begin(), init.end(), _data);
		}
		explicit StackVector(const std::vector<T>& vec) : _count(0) {
			if (vec.size() > MaxSize) {
				throw std::length_error("std::vector size exceeds FastVector capacity");
			}
			_count = vec.size();
			std::copy(vec.begin(), vec.end(), _data);
		}
		explicit StackVector(std::vector<T>&& vec) : _count(0) {
			if (vec.size() > MaxSize) {
				throw std::length_error("std::vector size exceeds FastVector capacity");
			}
			_count = vec.size();
			std::move(vec.begin(), vec.end(), _data);
		}
		// copy
		StackVector(const StackVector& other) : _count(0) {
			if (other.size() > MaxSize) {
				throw std::length_error("FastVector size exceeds FastVector capacity");
			}
			_count = other.size();
			std::copy(other.begin(), other.end(), _data);
		}
		// move
		// StackVector(StackVector&& other) noexcept(std::is_nothrow_move_constructible<T>::value) : _count(0) {
		// 	if (other.size() > MaxSize) {
		// 		throw std::length_error("FastVector size exceeds FastVector capacity");
		// 	}
		// 	_count = other.size();
		// 	std::move(other.begin(), other.end(), _data);
		// 	other.clear();
		// }
		// destructor
		~StackVector() {
			clear();
		}

		// assignment
		// copy
		StackVector& operator=(const std::vector<T>& vec) {
			if (vec.size() > MaxSize) {
				throw std::length_error("std::vector size exceeds FastVector capacity");
			}
			_count = vec.size();
			std::copy(vec.begin(), vec.end(), _data);
			return *this;
		}
		// move
		StackVector& operator=(std::vector<T>&& vec) {
			if (vec.size() > MaxSize) {
				throw std::length_error("std::vector size exceeds FastVector capacity");
			}
			_count = vec.size();
			std::move(vec.begin(), vec.end(), _data);
			return *this;
		}

		// addition of elements
		bool push_back(const T& element) noexcept {
			if (_count < MaxSize) {
				_data[_count++] = element;
				return true;
			}
			return false;
		}
		bool push_back(T&& element) noexcept {
			if (_count < MaxSize) {
				_data[_count++] = std::move(element);
				return true;
			}
			return false;
		}
		template <typename... Args>
		bool emplace_back(Args&&... args) {
			if (_count < MaxSize) {
				_data[_count++] = T(std::forward<Args>(args)...);
				return true;
			}
			return false;
		}
		bool insert(size_t index, const T& element) noexcept {
			if (_count >= MaxSize || index > _count) {
				return false;
			}
			for (size_t i = _count; i > index; --i) {
				_data[i] = std::move(_data[i - 1]);
			}
			_data[index] = element;
			++_count;
			return true;
		}
		bool insert(size_t index, T&& element) noexcept {
			if (_count >= MaxSize || index > _count) {
				return false;
			}
			for (size_t i = _count; i > index; --i) {
				_data[i] = std::move(_data[i - 1]);
			}
			_data[index] = std::move(element);
			++_count;
			return true;
		}

		// subtraction of elements
		void pop_back() {
			if (_count > 0) {
				--_count;
				return;
			}
		}


		// fast removal
		void remove(size_t index) noexcept {
			if (index < _count) {
				_data[index] = std::move(_data[--_count]);
			}
		}
		// order preserving removal
		void erase(size_t index) noexcept {
			if (index >= _count) return;
			for (size_t i = index; i < _count - 1; ++i) {
				_data[i] = std::move(_data[i + 1]);
			}
			--_count;
		}
		void erase(T* ptr) noexcept {
			auto index = static_cast<size_t>(ptr - _data);
			erase(index);
		}
		// will erase all elements with given value
		void erase_value(const T& value) noexcept {
			for (size_t i = 0; i < _count; ++i) {
				if (_data[i] == value) {
					erase(i);
				}
			}
		}

		// indice retrieval
		constexpr T& operator[](size_t index) noexcept { return _data[index]; }
		constexpr const T& operator[](size_t index) const noexcept { return _data[index]; }
		// safe retrieval
		T& at(size_t index) {
			if (index >= _count) {
				throw std::out_of_range("StackVector::at: index out of range");
			}
			return _data[index];
		}
		const T& at(size_t index) const {
			if (index >= _count) {
				throw std::out_of_range("StackVector::at: index out of range");
			}
			return _data[index];
		}
		// total retrieval
		T* data() noexcept { return _data; }
		const T* data() const noexcept { return _data; }

		// helpers
		size_t size() const noexcept { return _count; }
		constexpr size_t capacity() const noexcept { return MaxSize; }
		bool empty() const noexcept { return _count == 0; }
		void clear() noexcept { _count = 0; }
		void swap(size_t first_index, size_t second_index) noexcept {
			T temp = _data[first_index];
			_data[first_index] = _data[second_index];
			_data[second_index] = temp;
		}

		// iteration
		template <typename F>
		void for_each(F&& func) noexcept(noexcept(func(std::declval<T&>()))) {
			for (size_t i = 0; i < _count; ++i) {
				func(_data[i]);
			}
		}
		// iterators
		T* begin() { return _data; }
		T* end() { return _data + _count; }
		const T* begin() const { return _data; }
		const T* end() const { return _data + _count; }

	private:
		T _data[MaxSize];
		size_t _count;
	};
}