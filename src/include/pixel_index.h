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
    unsigned chip;          //!< Chip number
    unsigned flat_pixel;    //!< Flat pixel index relative to chip

    /*!
    \brief Get abstract pixel index for chip and pixel coordinate pair
    \param chip_index   Chip number
    \param xy           Pixel coordinate pair relative to chip
    \return Abstract pixel index for chip and coordinate pair
    */
    static inline PixelIndex from(unsigned chip_index, std::pair<uint64_t, uint64_t> xy)
    {
        return PixelIndex{chip_index, (unsigned)(xy.first * chip_size + xy.second)};
    }

    /*!
    \brief Get abstract pixel index for chip and flat pixel index
    \param chip_index   Chip number
    \param flat_pixel   Flat pixel index relative to chip
    \return Abstract pixel index for chip and flat pixel index
    */
    static inline PixelIndex from(unsigned chip_index, unsigned flat_pixel)
    {
        return PixelIndex{chip_index, flat_pixel};
    }
};

#endif // PIXEL_INDEX_H