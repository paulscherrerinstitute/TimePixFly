#ifndef PIXEL_INDEX_H
#define PIXEL_INDEX_H

/*!
\file
Pixel index abtraction
*/

#include "layout.h"

/*!
\brief Abstract pixel index
*/
struct PixelIndex final {
    unsigned chip;          // chip
    unsigned flat_pixel;    // flat pixel within chip

    static PixelIndex from(unsigned chip_index, std::pair<uint64_t, uint64_t> xy)
    {
        return PixelIndex{chip_index, (unsigned)(xy.first * chip_size + xy.second)};
    }

    static PixelIndex from(unsigned chip_index, unsigned flat_pixel)
    {
        return PixelIndex{chip_index, flat_pixel};
    }
};

#endif // PIXEL_INDEX_H