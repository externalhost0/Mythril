//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once
#include <cstdint>
#include <cstddef>
#include <limits>

template <typename T> constexpr T Invalid;

template<> constexpr inline uint8_t  Invalid<uint8_t>  = 0xFF;
template<> constexpr inline uint16_t Invalid<uint16_t> = 0xFFFF;
template<> constexpr inline uint32_t Invalid<uint32_t> = 0xFFFFFFFF;
template<> constexpr inline uint64_t Invalid<uint64_t> = 0xFFFFFFFFFFFFFFFF;

template<> constexpr inline int8_t  Invalid<int8_t>  = -1;
template<> constexpr inline int16_t Invalid<int16_t> = -1;
template<> constexpr inline int32_t Invalid<int32_t> = -1;
template<> constexpr inline int64_t Invalid<int64_t> = -1;

template<> constexpr inline float   Invalid<float>  = std::numeric_limits<float>::quiet_NaN();
template<> constexpr inline double  Invalid<double> = std::numeric_limits<double>::quiet_NaN();