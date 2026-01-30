#pragma once

#ifndef DECODER_H
#define DECODER_H

/*!
\file
Provide code for decoding Timepix3 raw stream data
*/

#include <cassert>
#include <cstdint>
#include <utility>

/*!
\brief Decoder object for ASI Raw Data Stream
See the ASI SERVAL manual "TPX3 raw file format" chapter.
*/
struct AsiRawStreamDecoder final {
    /*!
    \brief Chunk header
    */
    struct Header {
        uint64_t id: 32;        //!< Header ID "TPX3"
        uint64_t chip: 8;       //!< Chip index
        uint64_t reserved: 8;   //!< Reserved
        uint64_t size: 16;      //!< Chunk size
    };
    static_assert(sizeof(Header) == sizeof(uint64_t));

    static constexpr uint64_t chunk_id = 861425748UL; //!< 'TPX3' as uint64_t

    /*!
    \brief Packet ID
    */
    struct PacketID {
        uint64_t count: 48;     //!< Packet counter
        uint64_t reserved: 8;   //!< Reserved
        uint64_t type: 8;       //!< PacketID=0x50
    };
    static_assert(sizeof(PacketID) == sizeof(uint64_t));

    /*!
    \brief TOA event
    */
    struct TOA {
        uint64_t spidr: 16;     //!< SPIDR time (0.4096ms)
        uint64_t FToA: 4;       //!< FToA (-1.5625ns)
        uint64_t ToT: 10;       //!< ToT (25ns)
        uint64_t ToA: 14;       //!< ToA (25ns)
        uint64_t PixAddr: 16;   //!< PixAddr
        uint64_t type: 4;       //!< TOA=0xb
    };
    static_assert(sizeof(TOA) == sizeof(uint64_t));

    /*!
    \brief TDC event
    */
    struct TDC {
        uint64_t reserved: 5;   //!< Reserved
        uint64_t fine_ts: 4;    //!< Fine timestamp 1-12 (260.4166ps)
        uint64_t ts: 35;        //!< Timestamp (3.125ns)
        uint64_t Tcount: 12;    //!< Trigger count
        uint64_t action: 4;     //!< TDC1 rise=0xf fall=0xa, TDC2 rise=0xe fall=0xb
        uint64_t type: 4;       //!< TDC=0x6
    };
    static_assert(sizeof(TDC) == sizeof(uint64_t));

    /*!
    \brief Event type
    */
    struct Type {
        uint64_t data: 60;      //!< Event data
        uint64_t id: 4;         //!< Event type id
    };
    static_assert(sizeof(Type) == sizeof(uint64_t));

    /*!
    \brief Event types of interest
    */
    union alignas(uint64_t) Event {
        Header header;      //!< Packet header (must be first in packet)
        PacketID packet_id; //!< Packet ID (must be after header event)
        TOA toa;            //!< TOA
        TDC tdc;            //!< TDC
        Type type;          //!< TDC=0x6 or TOA=0xb
        uint64_t raw;       //!< Event raw data
    };
    static_assert(sizeof(Event) == sizeof(uint64_t));

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
    inline static unsigned getBits(uint64_t data, unsigned high, unsigned low) noexcept
    {
        const unsigned nbits = (high - low) + 1;
        const uint64_t mask = (1UL << nbits) - 1UL;
        // uint64_t maskShifted = mask << low;
        // return (data & maskShifted) >> low;
        return (data >> low) & mask;
    }

    /*!
        \brief Extract position information from event
        \param data 64bit value - event data
        \return X, Y relative to module
    */
    [[gnu::const]]
    inline static std::pair<uint64_t, uint64_t> calculateXY(uint64_t data) noexcept
    {
        //     # See Timepix manual
        //     encoded = data >> 44
        const uint64_t encoded = data >> 44;
        //     # doublecolumn * 2
        //     dcol = (encoded & 0x0FE00) >> 8
        const uint64_t dcol = (encoded & 0x0FE00UL) >> 8;
        //     # superpixel * 4
        //     spix = (encoded & 0x001F8) >> 1 # (16+28+3-2)
        const uint64_t spix = (encoded & 0x001F8UL) >> 1;
        //     # pixel
        //     pix = (encoded & 0x00007)
        const uint64_t pix = encoded & 0x00007UL;
        //     return (dcol + pix // 4), (spix + (pix & 0x3))
        return std::make_pair(dcol + pix / 4UL, spix + (pix & 0x3UL));
    }

    /*!
    \brief Flat pixel relative to module/chip
    \param event TOA event data
    \return Flat pixel index relative to module/chip
    */
    [[gnu::const]]
    inline static uint16_t flatPixel(TOA event) noexcept
    {
        const uint32_t encoded = event.PixAddr;
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
    inline static bool matchesNibble(uint64_t data, unsigned nibble) noexcept
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
    inline static bool matchesByte(uint64_t data, unsigned byte) noexcept
    {
        return (data >> 56) == byte;
    }
    
    /*!
    \brief Extract TDC clock from TDC event
    \param event TDC event
    \return Clock ticks counter
    */
    [[gnu::const]]
    inline static uint64_t getTdcClock(TDC event) noexcept
    {
        // const uint64_t tdcCoarse = (tdc >> 9) & 0x7ffffffffUL;
        // //     tdcCoarse = (tdc >> 9) & 0x7ffffffff
        // //     # fractional counts, values 1-12, 0.26 ns
        // //     fract = (tdc >> 5) & 0xf
        // const uint64_t fract = (tdc >> 5) & 0xfUL;
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
    inline static uint64_t getToaClock(TOA event) noexcept
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
    inline static uint64_t getTotClock(uint64_t data) noexcept
    {
        // return getBits(data, 29, 20) << 4;
        auto event = reinterpret_cast<const TOA*>(&data);
        return event->ToT << 4;
    }
};  // AsiRawStreamDecoder

#endif // DECODER_H
