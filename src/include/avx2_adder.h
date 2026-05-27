#pragma once

#ifndef AVX2_ADDER_H
#define AVX2_ADDER_H

/*!
\file
Provide code for adding XES data using AVX2 instrinsics
*/

// #pragma message("using avx2 adder")

#include <immintrin.h>

namespace avx2 {

    template<typename T>
    inline constexpr unsigned num_elements() noexcept
    {
        return sizeof(__m256) / sizeof(T);
    }

    template<typename T>
    inline constexpr unsigned mask() noexcept
    {
        return num_elements<T>() - 1u;
    }

    template<typename T>
    inline constexpr unsigned rest(unsigned size) noexcept
    {
        return size & mask<T>();
    }

    template<typename T>
    inline constexpr unsigned num_vecs(unsigned size) noexcept
    {
        return size / num_elements<T>();
    }

    inline void reset(__m256* data, unsigned size) noexcept
    {
        const auto nvecs = num_vecs<float>(size);
        const auto plus = rest<float>(size);
        const auto zero = _mm256_setzero_ps();

        for (unsigned i=0; i<nvecs; data++, i++)
            _mm256_store_ps((float*)data, zero);

        float* fdata = (float*)data;
        for (unsigned i=0; i<plus; fdata++, i++)
            *fdata = .0f;
    }

    inline void addReset(__m256* src, __m256* dst, unsigned size) noexcept
    {
        const auto nvecs = num_vecs<float>(size);
        const auto plus = rest<float>(size);
        
        for (unsigned i=0; i<nvecs; src++, dst++, i++) {
            auto s = _mm256_load_ps((float*)src);
            auto d = _mm256_load_ps((float*)dst);
            d = _mm256_add_ps(s, d);
            s = _mm256_setzero_ps();
            _mm256_store_ps((float*)dst, d);
            _mm256_store_ps((float*)src, s);
        }
        
        float* fsrc = (float*)src;
        float* fdst = (float*)dst;
        for (unsigned i=0; i<plus; fsrc++, fdst++, i++) {
            *fdst += *fsrc;
            *fsrc = .0f;
        }
    }

} // namespace avx2

#endif // ifndef AVX2_ADDER_H
