//
// Created by Hayden Rivas on 1/3/26.
//

#pragma once

#include <cstddef>
#include <array>
#include <string>
#include <stdexcept>

namespace mythril {
	static constexpr size_t CalculateLength(const char *s) {
		size_t len = 0;
		while (s[len] != '\0') {
			++len;
		}
		return len;
	}

	static constexpr void constexpr_memcpy(char *dest, const char *src, size_t n) {
		for (size_t i = 0; i < n; ++i) {
			dest[i] = src[i];
		}
	}


	template<size_t MaxLength>
	class StackString {
	public:
		// constructors
		constexpr StackString() noexcept = default;

		constexpr StackString(const char *s) {
			assign(s);
		}

		template<size_t N>
		constexpr StackString(const char (&str)[N]) {
			static_assert(N <= MaxLength, "StackString literal too long");
			for (size_t i = 0; i < N - 1; ++i)
				_data[i] = str[i];
			_data[N - 1] = '\0';
			_length = N - 1;
		}

		constexpr StackString(const char *s, size_t len) {
			assign(s, len);
		}

		explicit StackString(const std::string &str) {
			assign(str);
		}

		constexpr ~StackString() = default;

		// move and copy
		constexpr StackString(StackString&& other) noexcept
		: _length(other._length) {
			constexpr_memcpy(_data, other._data, _length + 1);
			other.clear();
		}
		constexpr StackString& operator=(StackString&& other) noexcept {
			if (this != &other) {
				_length = other._length;
				constexpr_memcpy(_data, other._data, _length + 1);
				other.clear();
			}
			return *this;
		}

		// basic functions

		constexpr StackString &assign(const char *s) {
			const size_t len = CalculateLength(s);
			if (len > MaxLength)
				throw std::overflow_error("StackString::assign has exceeded its maxlength");
			constexpr_memcpy(_data, s, len);
			_data[len] = '\0';
			_length = len;
			return *this;
		}

		constexpr StackString &assign(const char *s, size_t len) {
			if (len > MaxLength)
				throw std::overflow_error("StackString::assign has exceeded its maxlength");
			constexpr_memcpy(_data, s, len);
			_data[len] = '\0';
			_length = len;
			return *this;
		}

		StackString &assign(const std::string &str) {
			const size_t len = str.size();
			if (len > MaxLength)
				throw std::overflow_error("StackString::assign has exceeded its maxlength");
			std::memcpy(_data, str.data(), len);
			_data[len] = '\0';
			_length = len;
			return *this;
		}

		constexpr StackString &append(const char *s) {
			const size_t len = CalculateLength(s);
			if (_length + len > MaxLength)
				throw std::overflow_error("StackString::append has exceeded its maxlength");
			constexpr_memcpy(_data + _length, s, len);
			_length += len;
			_data[_length] = '\0';
			return *this;
		}

		constexpr StackString &append(const char *s, const size_t size) {
			if (_length + size > MaxLength)
				throw std::overflow_error("StackString::append has exceeded its MaxLength");
			constexpr_memcpy(_data + _length, s, size);
			_length += size;
			_data[_length] = '\0';
			return *this;
		}

		StackString &append(const std::string &str) {
			const size_t len = str.size();
			if (_length + len > MaxLength)
				throw std::overflow_error("StackString::append has exceeded its MaxLength");
			std::memcpy(_data + _length, str.data(), len);
			_length += len;
			_data[_length] = '\0';
			return *this;
		}

		// operators
		// addition
		constexpr StackString operator+(const char* s) {
			StackString result = *this;
			result.append(s);
			return result;
		}
		template<size_t N>
		constexpr StackString operator+(const char (&s)[N]) {
			StackString result = *this;
			result.append(s);
			return result;
		}
		// addition assignment
		constexpr void operator+=(const char *s) { append(s); }
		template<size_t N>
		constexpr void operator+=(const char (&s)[N]) { append(s); }
		void operator+=(const std::string &s) { append(s); }

		// indice retrieval
		constexpr char &operator[](size_t index) noexcept {
			static_assert(index < MaxLength, "Index is beyond MaxLength");
			return _data[index];
		}
		constexpr const char &operator[](size_t index) const noexcept {
			static_assert(index < MaxLength, "Index is beyond MaxLength");
			return _data[index];
		}
		// bound checking indice retrieval
		constexpr char& at(size_t index) {
			if (index >= _length)
				throw std::out_of_range("StackString::at index out of range");
			return _data[index];
		}

		constexpr const char& at(size_t index) const {
			if (index >= _length)
				throw std::out_of_range("StackString::at index out of range");
			return _data[index];
		}

		template<size_t OtherMax>
		constexpr bool operator==(const StackString<OtherMax> &other) const noexcept {
			if (_length != other._length) return false;
			for (size_t i = 0; i < _length; ++i) {
				if (_data[i] != other._data[i]) return false;
			}
			return true;
		}
		template<size_t OtherMax>
		constexpr bool operator!=(const StackString<OtherMax> &other) const noexcept {
			return !(*this == other);
		}
		constexpr bool operator==(const char *s) const noexcept {
			size_t i = 0;
			while (i < _length && s[i] != '\0') {
				if (_data[i] != s[i]) return false;
				++i;
			}
			return i == _length && s[i] == '\0';
		}
		constexpr bool operator!=(const char *s) const noexcept {
			return !(*this == s);
		}
		bool operator==(const std::string &str) const noexcept {
			if (_length != str.size()) return false;
			for (size_t i = 0; i < _length; ++i) {
				if (_data[i] != str[i]) return false;
			}
			return true;
		}
		bool operator!=(const std::string &str) const noexcept {
			return !(*this == str);
		}

		// helpers
		constexpr size_t max_length() const noexcept { return MaxLength-1; }
		constexpr size_t available() const noexcept { return MaxLength-_length; }
		// includes null terminator
		constexpr size_t size() const noexcept { return _length + 1; }
		// is the actual # of characters
		constexpr size_t length() const noexcept { return _length; }
		constexpr bool empty() const noexcept { return _length == 0; }

		constexpr void clear() noexcept {
			_length = 0;
			_data[0] = '\0';
		}

		// retrieval
		// const and non const overrides
		constexpr char *data() noexcept { return _data; }
		constexpr char *c_str() noexcept { return data(); }
		constexpr const char *data() const noexcept { return _data; }
		constexpr const char *c_str() const noexcept { return data(); }

		// for iterators
		constexpr char* begin() noexcept { return _data; }
		constexpr char* end() noexcept { return _data + _length; }
		constexpr const char* begin() const noexcept { return _data; }
		constexpr const char* end() const noexcept { return _data + _length; }
		constexpr const char* cbegin() const noexcept { return _data; }
		constexpr const char* cend() const noexcept { return _data + _length; }

	private:
		// add one for null terminator
		char _data[MaxLength+1] = {};
		size_t _length = 0;
	};

	// reversed combinations
	template<size_t MaxLength>
	constexpr bool operator==(const char* lhs, const StackString<MaxLength>& rhs) noexcept {
		return rhs == lhs;
	}
	template<size_t MaxLength>
	constexpr bool operator!=(const char* lhs, const StackString<MaxLength>& rhs) noexcept {
		return rhs != lhs;
	}
	template<size_t MaxLength>
	bool operator==(const std::string& lhs, const StackString<MaxLength>& rhs) noexcept {
		return rhs == lhs;
	}

	template<size_t MaxLength>
	bool operator!=(const std::string& lhs, const StackString<MaxLength>& rhs) noexcept {
		return rhs != lhs;
	}
}
