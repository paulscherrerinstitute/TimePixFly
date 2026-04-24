#pragma once

/*!
\file
Provide data structures for dealing with the pixel to energy point mapping
*/

#ifndef PIXEL_MAP_H
#define PIXEL_MAP_H

#include <stdexcept>
#include "pixel_index.h"

/*!
\brief Partial energy point mapping
*/
struct MapDest final {
        unsigned energy_point;                  //!< pixel contributes to this energy point
        float weight;                           //!< with this weight
};

/*!
\brief Inequality test
\param a First
\param b Second
\return true iff a != b
*/
inline bool operator!=(const MapDest& a, const MapDest& b) noexcept
{
    return (a.energy_point != b.energy_point)
        || (a.weight != b.weight);
}

/*!
\brief Pixel to energy point mapping
This map can be produced by `PixelIndexToEp::to_map()`
*/
struct PixelMap final {
    unsigned pixels_per_chip = 0;               //!< Number of pixels per chip
    unsigned npoints = 0;                       //!< Number of energy points

    /*!
    \brief List of mappings
    The `flat_pix` list indexes this list
    */
    std::vector<MapDest> mapping;

    /*!
    \brief Index per flat pixel
    This list contains an index into `mapping` for every flat pixel per chip.
    The indices in this list must not be decreasing. At the end, a sentinel entry
    pointing to one past the end of `mapping` is present.
    For an entry `i`, the range of mappings is `[flat_pix[i] .. flat_pix[i+1][`
    The flat pixel `p` of chip `c`, `i = c * pixels_per_chip + p`
    */
    std::vector<unsigned> indices;

    /*!
    \brief Pixel to energy point mapping range
    This represents the energy point range for a pixel
    */
    struct Range final {
        PixelMap& pmap;                         //!< Reference to the mapping                       
        unsigned start;                         //!< Beginning of range
        unsigned sentinel;                      //!< One past end of range

        /*!
        \brief Start of range
        \return Iterator pointing to the start element
        */
        inline MapDest* begin()
        {
            return &pmap.mapping[start];
        }

        /*!
        \brief One past end of range
        \return Iterator pointing to one past the last element
        */
        inline MapDest* end()
        {
            return pmap.mapping.data() + sentinel;
        }

        /*!
        \brief Start of range
        \return Iterator pointing to the start element
        */
        inline const MapDest* begin() const
        {
            return &pmap.mapping[start];
        }

        /*!
        \brief One past end of range
        \return Iterator pointing to one past the last element
        */
        inline const MapDest* end() const
        {
            return pmap.mapping.data() + sentinel;
        }

        /*!
        \brief Is this an empty range?
        \return True if this is an empty range
        */
        inline bool empty() const
        {
            return start == sentinel;
        }

        /*!
        \brief How many elements does this range have?
        \return Number of elements in this range
        */
        inline unsigned size() const
        {
            return sentinel - start;
        }
    };

    /*!
    \brief Map abstract pixel index to pixel mapping range
    \param index Abstract pixel index
    \return Flat pixel to energy point mapping range
    */
    inline const Range operator[](const PixelIndex& index) const
    {
        auto base = index.chip * pixels_per_chip + index.flat_pixel;
        return {const_cast<PixelMap&>(*this), indices[base], indices[base + 1]};
    }

    /*!
    \brief Map abstract pixel index to pixel mapping range
    \param index Abstract pixel index
    \return Flat pixel to energy point mapping range
    */
    inline const Range at(const PixelIndex& index)
    {
            auto base = index.chip * pixels_per_chip + index.flat_pixel;
            return {*this, indices.at(base), indices.at(base + 1)};
    }
};

/*!
\brief Write in json format to output stream
\param out Output stream
\param pmap The pixel to energy point mapping
\return Output stream
*/
std::ostream& operator<<(std::ostream& out, const PixelMap& pmap);

#endif
