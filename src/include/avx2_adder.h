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

    /*!
    \brief Number of vector elements
    \return Number of elements in a vector
    \tparam T Vector element type
    */
    template<typename T>
    inline constexpr unsigned num_elements() noexcept
    {
        static_assert(sizeof(__m256) % sizeof(T) == 0,
                      "Type T size must divide __m256 size evenly");
        return sizeof(__m256) / sizeof(T);
    }

    /*!
    \brief Vector elements mask
    \return Mask for masking out bits that signify vector elements
    \tparam T Vector element type
    */
    template<typename T>
    inline constexpr unsigned mask() noexcept
    {
        return num_elements<T>() - 1u;
    }

    /*!
    \brief Number of leftover elements in last, non-filled vector

    Assumes that the vectors are aligned
    \param size Array size in elements
    \return Number of leftover elements after last full vector
    \tparam T Vector element type
    */
    template<typename T>
    inline constexpr unsigned rest(unsigned size) noexcept
    {
        return size & mask<T>();
    }

    /*!
    \brief Number full vectors

    Assumes that the vectors are aligned
    \param size Array size in elements
    \return Number of full vectors
    \tparam T Vector element type
    */
    template<typename T>
    inline constexpr unsigned num_vecs(unsigned size) noexcept
    {
        return size / num_elements<T>();
    }

    /*!
    \brief Set elements to zero
    \param data Pointer to first element
    \param size Number of elements
    */
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

    /*!
    \brief Add source to destination and reset source to zero
    \param src Pointer to first source element
    \param dst Pointer to first destination element
    \param size Number of elements
    */
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
