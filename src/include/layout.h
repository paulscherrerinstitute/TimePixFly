#ifndef LAYOUT_H
#define LAYOUT_H

/*!
\file
Detector layout description
*/

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

#endif
