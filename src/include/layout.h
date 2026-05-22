#pragma once

#ifndef LAYOUT_H
#define LAYOUT_H

/*!
\file
Detector layout description
*/

#include <vector>
#include <ostream>

constexpr static unsigned chip_size = 256;  //!< Width and length in pixels of quadratic TPX3 chip

/*!
\brief Chip position within detector area
*/
struct chip_position final {
    unsigned x; //!< X pixel coordinate
    unsigned y; //!< Y pixel coordinate
};

/*!
\brief Detector layout
*/
struct detector_layout final {
    unsigned width;                     //!< Width in pixels of detector area
    unsigned height;                    //!< Height in pixels of detector area
    std::vector<chip_position> chip;    //!< Position of each chip by chip number
};

/*!
\brief Print out layout
\param out Output stream reference
\param layout Layout
\return Output stream reference
*/
inline std::ostream& operator<<(std::ostream& out, const detector_layout& layout)
{
    out << "layout:" << layout.width << 'x' << layout.height;
    auto numChips = layout.chip.size();
    for (decltype(numChips) i=0; i<numChips; i++)
        out << '-' << i << '@' << layout.chip[i].x << ',' << layout.chip[i].y;
    return out;
}

#endif
