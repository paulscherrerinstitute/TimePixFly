#ifndef DECODER_H
#define DECODER_H

// Provide code for decoding Timepix3 raw stream data

struct AsiRawStreamDecoder final {
    // def get_bits(data, high, low):
    //     num = (high - low) + 1
    //     mask = (1 << num) - 1  # Trick: 2**N - 1 gives N consecutive ones
    //     maskShifted = mask << low
        
    //     return (data & maskShifted) >> low
    [[gnu::const]]
    inline static unsigned getBits(uint64_t data, unsigned high, unsigned low) noexcept
    {
        unsigned nbits = (high - low) + 1;
        uint64_t mask = (1UL << nbits) - 1UL;
        // uint64_t maskShifted = mask << low;
        // return (data & maskShifted) >> low;
        return (data >> low) & mask;
    }

    // def calculate_XY(data):
    [[gnu::const]]
    inline static std::pair<uint64_t, uint64_t> calculateXY(uint64_t data) noexcept
    {
        //     # See Timepix manual
        //     encoded = data >> 44
        uint64_t encoded = data >> 44;
        //     # doublecolumn * 2
        //     dcol = (encoded & 0x0FE00) >> 8
        uint64_t dcol = (encoded & 0x0FE00UL) >> 8;
        //     # superpixel * 4
        //     spix = (encoded & 0x001F8) >> 1 # (16+28+3-2)
        uint64_t spix = (encoded & 0x001F8UL) >> 1;
        //     # pixel
        //     pix = (encoded & 0x00007)
        uint64_t pix = encoded & 0x00007UL;
        //     return (dcol + pix // 4), (spix + (pix & 0x3))
        return std::make_pair(dcol + pix / 4UL, spix + (pix & 0x3UL));
    }

    // def clock_to_float(count, clock=640e6):
    //     # Count is on a 640 MHz clock.
    //     # Return the time in seconds.
    //     return float(count) / clock
    [[gnu::const]]
    inline static float clockToFloat(int64_t count, double clock=640e6) noexcept
    {
        return count / clock;
    }

    // def matches_nibble(data, nibble):
    //     return (data >> 60) == nibble
    [[gnu::const]]
    inline static bool matchesNibble(uint64_t data, unsigned nibble) noexcept
    {
        return (data >> 60) == nibble;
    }

    [[gnu::const]]
    inline static bool matchesByte(uint64_t data, unsigned byte) noexcept
    {
        return (data >> 56) == byte;
    }
    
    // def get_TDC_clock(tdc):
    [[gnu::const]]
    inline static uint64_t getTdcClock(uint64_t tdc) noexcept
    {
        uint64_t tdcCoarse = (tdc >> 9) & 0x7ffffffffUL;
        //     tdcCoarse = (tdc >> 9) & 0x7ffffffff
        //     # fractional counts, values 1-12, 0.26 ns
        //     fract = (tdc >> 5) & 0xf        
        uint64_t fract = (tdc >> 5) & 0xfUL;
        //     # Bug: fract is sometimes 0 for older firmware but it should be 1 <= fract <= 12
        //     assert 1 <= fract <= 12, f"Incorrect fractional TDC part {fract}, corrupt data: {tdc}"
        assert((1 <= fract) && (fract <= 12));
        //     # tdc in 640 MHz units (1.5625)
        //     return (tdcCoarse << 1) | ((fract-1) // 6)
        return (tdcCoarse << 1) | ((fract - 1) / 6);
    }

    // def get_TOA_clock(data):
    [[gnu::const]]
    inline static int64_t getToaClock(uint64_t data) noexcept
    {
        //     # ftoa is on a 640 MHz clock
        //     # toa is on a 40 MHz clock
        //     ftoa = get_bits(data, 19, 16)
        //     toa = get_bits(data, 43, 30)
        //     coarse = get_bits(data, 15, 0)        
        //     return (((coarse << 14) + toa) << 4) - ftoa
        int64_t ftoa = getBits(data, 19, 16);
        int64_t toa = getBits(data, 43, 30);
        int64_t coarse = getBits(data, 15, 0);
        return (((coarse << 14) + toa) << 4) - ftoa;
    }

    // def get_TOT_clock(data):
    //     return get_bits(data, 29, 20)
    [[gnu::const]]
    inline static uint64_t getTotClock(uint64_t data) noexcept
    {
        return getBits(data, 29, 20);
    }
};  // AsiRawStreamDecoder

#endif // DECODER_H
