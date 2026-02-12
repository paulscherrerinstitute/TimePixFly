#pragma once

#ifndef DECODER_H
#define DECODER_H

/*!
\file
Provide code for decoding Timepix3 raw stream data
*/

#include <cassert>
#include <utility>

#include "shared_types.h"

/*!
\brief Decoder object for ASI Raw Data Stream
See the ASI SERVAL manual "TPX3 raw file format" chapter.
*/
struct AsiRawStreamDecoder final {
    /*!
    \brief Chunk header
    */
    struct Header {
        u64 id: 32;        //!< Header ID "TPX3"
        u64 chip: 8;       //!< Chip index
        u64 reserved: 8;   //!< Reserved
        u64 size: 16;      //!< Chunk size
    };
    static_assert(sizeof(Header) == sizeof(u64));

    static constexpr u64 chunk_id = 861425748UL; //!< 'TPX3' as u64

    /*!
    \brief Packet ID
    */
    struct PacketID {
        u64 count: 48;     //!< Packet counter
        u64 reserved: 8;   //!< Reserved
        u64 type: 8;       //!< PacketID=0x50
    };
    static_assert(sizeof(PacketID) == sizeof(u64));

    /*!
    \brief TOA event
    */
    struct TOA {
        u64 spidr: 16;     //!< SPIDR time (0.4096ms)
        u64 FToA: 4;       //!< FToA (-1.5625ns)
        u64 ToT: 10;       //!< ToT (25ns)
        u64 ToA: 14;       //!< ToA (25ns)
        u64 PixAddr: 16;   //!< PixAddr
        u64 type: 4;       //!< TOA=0xb
    };
    static_assert(sizeof(TOA) == sizeof(u64));

    /*!
    \brief TDC event
    */
    struct TDC {
        u64 reserved: 5;   //!< Reserved
        u64 fine_ts: 4;    //!< Fine timestamp 1-12 (260.4166ps)
        u64 ts: 35;        //!< Timestamp (3.125ns)
        u64 Tcount: 12;    //!< Trigger count
        u64 action: 4;     //!< TDC1 rise=0xf fall=0xa, TDC2 rise=0xe fall=0xb
        u64 type: 4;       //!< TDC=0x6
    };
    static_assert(sizeof(TDC) == sizeof(u64));

    /*!
    \brief Event type
    */
    struct Type {
        u64 data: 60;      //!< Event data
        u64 id: 4;         //!< Event type id
    };
    static_assert(sizeof(Type) == sizeof(u64));

    /*!
    \brief Event types of interest
    */
    union alignas(u64) Event {
        Header header;      //!< Packet header (must be first in packet)
        PacketID packet_id; //!< Packet ID (must be after header event)
        TOA toa;            //!< TOA
        TDC tdc;            //!< TDC
        Type type;          //!< TDC=0x6 or TOA=0xb
        u64 raw;       //!< Event raw data
    };
    static_assert(sizeof(Event) == sizeof(u64));

    /*!
    \brief Extract bits from event

    Python equivalent:

    def get_bits(data, high, low):
         num = (high - low) + 1
         mask = (1 << num) - 1  # Trick: 2**N - 1 gives N consecutive ones
         maskShifted = mask << low
        
         return (data & maskShifted) >> low
    
    \param data 64bit value - event data
    \param high High High bit inclusive
    \param low Low bit inclusive
    \return Extracted bits, at most 32bits
    */
    [[gnu::const]]
    inline static unsigned getBits(u64 data, unsigned high, unsigned low) noexcept
    {
        const unsigned nbits = (high - low) + 1;
        const u64 mask = (1UL << nbits) - 1UL;
        // u64 maskShifted = mask << low;
        // return (data & maskShifted) >> low;
        return (data >> low) & mask;
    }

    /*!
        \brief Extract position information from event
        \param data 64bit value - event data
        \return X, Y relative to module
    */
    [[gnu::const]]
    inline static std::pair<u64, u64> calculateXY(u64 data) noexcept
    {
        //     # See Timepix manual
        //     encoded = data >> 44
        const u64 encoded = data >> 44;
        //     # doublecolumn * 2
        //     dcol = (encoded & 0x0FE00) >> 8
        const u64 dcol = (encoded & 0x0FE00UL) >> 8;
        //     # superpixel * 4
        //     spix = (encoded & 0x001F8) >> 1 # (16+28+3-2)
        const u64 spix = (encoded & 0x001F8UL) >> 1;
        //     # pixel
        //     pix = (encoded & 0x00007)
        const u64 pix = encoded & 0x00007UL;
        //     return (dcol + pix // 4), (spix + (pix & 0x3))
        return std::make_pair(dcol + pix / 4UL, spix + (pix & 0x3UL));
    }

    /*!
    \brief Flat pixel relative to module/chip
    \param event TOA event data
    \return Flat pixel index relative to module/chip
    */
    [[gnu::const]]
    inline static u16 flatPixel(TOA event) noexcept
    {
        const u32 encoded = event.PixAddr;
        const auto dcol = (encoded & 0xFE00) >> 8;
        const auto spix = (encoded & 0x01F8) >> 1;
        const auto pix = encoded & 0x7;
        return ((dcol + pix / 4u) << 8) + (spix + (pix & 0x3));
    }

    /*!
    \brief Convert clock ticks counter value to seconds
    \param count Clock counter value in units of clock ticks
    \return Clock value in seconds
    */
    [[gnu::const]]
    inline static float clockToFloat(int64_t count) noexcept
    {
        static constexpr double clock_to_sec = 640e-6;  // 1 clock tick = 1.5625ns
        return count * clock_to_sec;
    }

    /*!
    \brief Compare high nibble with value

    Python equivalent:

    def matches_nibble(data, nibble):
        return (data >> 60) == nibble

    \param data 64bit value - event data
    \param nibble Nibble value
    \return True iff there is a match
    */
    [[gnu::const]]
    inline static bool matchesNibble(u64 data, unsigned nibble) noexcept
    {
        return (data >> 60) == nibble;
    }

    /*!
    \brief Compare high byte with value
    \param data 64bit value - event data
    \param byte Byte value
    \return True iff there is a match
    */
    [[gnu::const]]
    inline static bool matchesByte(u64 data, unsigned byte) noexcept
    {
        return (data >> 56) == byte;
    }
    
    /*!
    \brief Extract TDC clock from TDC event
    \param event TDC event
    \return Clock ticks counter
    */
    [[gnu::const]]
    inline static u64 getTdcClock(TDC event) noexcept
    {
        // const u64 tdcCoarse = (tdc >> 9) & 0x7ffffffffUL;
        // //     tdcCoarse = (tdc >> 9) & 0x7ffffffff
        // //     # fractional counts, values 1-12, 0.26 ns
        // //     fract = (tdc >> 5) & 0xf
        // const u64 fract = (tdc >> 5) & 0xfUL;
        // //     # Bug: fract is sometimes 0 for older firmware but it should be 1 <= fract <= 12
        // //     assert 1 <= fract <= 12, f"Incorrect fractional TDC part {fract}, corrupt data: {tdc}"
        // assert((1 <= fract) && (fract <= 12));
        // //     # tdc in 640 MHz units (1.5625)
        // //     return (tdcCoarse << 1) | ((fract-1) // 6)
        // return (tdcCoarse << 1) | ((fract - 1) / 6);
        return (event.ts << 1) | ((event.fine_ts - 1u) / 6u);
    }

    /*!
    \brief Extract TOA clock from TOA event
    \param event TOA event
    \return Clock ticks counter
    */
    [[gnu::const]]
    inline static u64 getToaClock(TOA event) noexcept
    {
        //     # ftoa is on a 640 MHz clock
        //     # toa is on a 40 MHz clock
        //     ftoa = get_bits(data, 19, 16)
        //     toa = get_bits(data, 43, 30)
        //     coarse = get_bits(data, 15, 0)        
        //     return (((coarse << 14) + toa) << 4) - ftoa
        // const int64_t ftoa = getBits(data, 19, 16);
        // const int64_t toa = getBits(data, 43, 30);
        // const int64_t coarse = getBits(data, 15, 0);
        // return (((coarse << 14) + toa) << 4) - ftoa;
        return (((event.spidr << 14) + event.ToA) << 4) - event.FToA;
    }

    /*!
    \brief Extract TOT clock from TOA event

    Python equivalent:

    def get_TOT_clock(data):
        return get_bits(data, 29, 20)

    \param data Raw TOA event
    \return Clock ticks counter
    */
    [[gnu::const]]
    inline static u64 getTotClock(u64 data) noexcept
    {
        // return getBits(data, 29, 20) << 4;
        auto event = reinterpret_cast<const TOA*>(&data);
        return event->ToT << 4;
    }
};  // AsiRawStreamDecoder

#endif // DECODER_H
