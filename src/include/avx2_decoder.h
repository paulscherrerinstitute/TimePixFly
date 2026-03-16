#pragma once

#ifndef AVX2_DECODER_H
#define AVX2_DECODER_H

/*!
\file
Provide code for decoding Timepix3 raw stream data using AVX2 instrinsics
*/

#include <immintrin.h>
#include <popcntintrin.h>

/*!
\brief Decoder code for AVX2
See \ref decoder.h
*/
namespace avx2 {
    /*!
    \brief Extract TOA position
    \param events Event vector
    \return TOA flat pixel position vector
    */
    [[gnu::const]]
    inline __m256i toapos(__m256i events) noexcept
    {
        const static __m256i dcol_mask = _mm256_set1_epi64x(0x0fe00ull);
        const static __m256i spix_mask = _mm256_set1_epi64x(0x001f8ull);
        const static __m256i pix1_mask = _mm256_set1_epi64x(0x4ull);
        const static __m256i pix2_mask = _mm256_set1_epi64x(0x3ull);
        const auto encoded = _mm256_srli_epi64(events, 44);
        auto dcol = _mm256_and_si256(encoded, dcol_mask); // dcol << 8
        auto spix = _mm256_srli_epi64(_mm256_and_si256(encoded, spix_mask), 1);
        spix = _mm256_add_epi64(spix, _mm256_and_si256(encoded, pix2_mask));
        const auto pix = _mm256_and_si256(encoded, pix1_mask);
        dcol = _mm256_add_epi64(dcol, _mm256_slli_epi64(pix, 6));
        return _mm256_or_epi64(spix, dcol);
    }

    /*!
    \brief Extract TOA clock
    \param events Event vector
    \return TOA clock vector
    */
    [[gnu::const]]
    inline __m256i toaclk(__m256i events) noexcept
    {
        static const __m256i spidr_mask = _mm256_set1_epi64x(0xffffull);
        static const __m256i toa_mask = _mm256_set1_epi64x(0x3fff0ull);
        static const __m256i ftoa_mask = _mm256_set1_epi64x(0xfull);
        const auto spidr = _mm256_slli_epi64(_mm256_and_si256(events, spidr_mask), 18);
        const auto toa = _mm256_and_si256(_mm256_srli_epi64(events, 26), toa_mask);
        auto clk = _mm256_add_epi64(spidr, toa);
        const auto ftoa = _mm256_and_si256(_mm256_srli_epi64(events, 16), ftoa_mask);
        return _mm256_sub_epi64(clk, ftoa);
    }

    /*!
    \brief Extract TDC clock
    \param events Event vector
    \return TDC clock vector
    */
    [[gnu::const]]
    inline __m256i tdcclk(__m256i events) noexcept
    {
        static const __m256i six = _mm256_set1_epi64x(0xc0ull);
        static const __m256i fine_mask = _mm256_set1_epi64x(0x1e0ull);
        static const __m256i ts_mask = _mm256_set1_epi64x(0x1ffffffffeull);
        const auto fine = _mm256_and_si256(events, fine_mask);
        const auto lastbit = _mm256_srli_epi64(_mm256_cmpgt_epi64(fine, six), 63);
        const auto ts = _mm256_and_si256(_mm256_srli_epi64(events, 8), ts_mask);
        return _mm256_or_si256(ts, lastbit);
    }

    /*!
    \brief Decode raw event vector
    \param events Event vector to be decoded
    \param toa_or_tdc One bit per vector element used as a flag
    \return Decoded event_t event vector
    */
    [[gnu::const]]
    inline __m256i decode(__m256i events, int& toa_or_tdc) noexcept
    {
        static const __m256i toa_type = _mm256_set1_epi64x(0xbull);
        static const __m256i tdc_type = _mm256_set1_epi64x(0x6ull);
        const auto type = _mm256_srli_epi64(events, 60);
        const auto toa_mask = _mm256_cmpeq_epi64(type, toa_type);
        const auto tdc_mask = _mm256_cmpeq_epi64(type, tdc_type);
        const auto mask = _mm256_or_si256(toa_mask, tdc_mask);
        toa_or_tdc = _mm256_movemask_pd(_mm256_castsi256_pd(mask));
        int num_toa = _mm_popcnt_u64(_mm256_movemask_pd(_mm256_castsi256_pd(toa_mask)));
        int num_tdc = _mm_popcnt_u64(_mm256_movemask_pd(_mm256_castsi256_pd(tdc_mask)));
        __m256i res = _mm256_setzero_si256();
        __m256i toa_ev = res;
        if (num_toa) {
            toa_ev = toaclk(events);
            auto toa_pos = toapos(events);
            toa_ev = _mm256_or_si256(toa_ev, _mm256_slli_epi64(toa_pos, 48));
        }
        __m256i tdc_ev = res;
        if (num_tdc) {
            tdc_ev = _mm256_set1_epi64x(1ull << 47);    // is_tdc flag
            tdc_ev = _mm256_or_si256(tdc_ev, tdcclk(events));
        }
        res = _mm256_castpd_si256(_mm256_blendv_pd(
            _mm256_castsi256_pd(res),
            _mm256_castsi256_pd(toa_ev),
            _mm256_castsi256_pd(toa_mask)
        ));
        return _mm256_castpd_si256(_mm256_blendv_pd(
            _mm256_castsi256_pd(res),
            _mm256_castsi256_pd(tdc_ev),
            _mm256_castsi256_pd(tdc_mask)
        ));
    }
} // namespace avx2

#endif // ifndef AVX2_DECODER_H