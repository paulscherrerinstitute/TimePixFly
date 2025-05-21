#ifndef ENERGY_POINTS_H
#define ENERGY_POINTS_H

/*!
\file
Pixel to energy point mapping
*/

#include <istream>
#include <vector>

/*!
\brief Partial energy point mapping
*/
struct EpPart final {
        unsigned energy_point;                  //!< pixel contributes to this energy point
        float weight;                           //!< with this weight
};

/*!
\brief Flat pixel to energy point mapping
*/
struct FlatPixelToEp final {
        std::vector<EpPart> part;               //!< one part per energy point
};

/*!
\brief Per chip flat pixel to energy point mapping
*/
struct ChipToEp final {
        std::vector<FlatPixelToEp> flat_pixel;  //!< Per chip flat pixel to energy point mapping by chip number
};

/*!
\brief Abstract pixel index to energy point mapping
*/
struct PixelIndexToEp final {
        std::vector<ChipToEp> chip;     //!< Flat pixel to energy point mapping per chip
        unsigned npoints = 0;           //!< Number of energy points

        /*!
        \brief Map abstract pixel index to mutable flat pixel to energy point mapping
        \param index Abstract pixel index
        \return Flat pixel to energy point mapping reference
        */
        inline FlatPixelToEp& operator[](const PixelIndex& index)
        {
                assert(index.chip < chip.size());
                assert(index.flat_pixel < chip[index.chip].flat_pixel.size());
                return chip[index.chip].flat_pixel[index.flat_pixel];
        }

        /*!
        \brief Map abstract pixel index to immutable flat pixel to energy point mapping
        \param index Abstract pixel index
        \return Flat pixel to energy point mapping reference
        */
        inline const FlatPixelToEp& operator[](const PixelIndex& index) const
        {
                assert(index.chip < chip.size());
                assert(index.flat_pixel < chip[index.chip].flat_pixel.size());
                return chip[index.chip].flat_pixel[index.flat_pixel];
        }

        /*!
        \brief Checked mapping of abstract pixel index to mutable flat pixel to energy point mapping
        \param index Abstract pixel index
        \return Flat pixel to energy point mapping reference
        */
        inline FlatPixelToEp& at(const PixelIndex& index)
        {
                return chip.at(index.chip).flat_pixel.at(index.flat_pixel);
        }

        static constexpr unsigned FILE_STREAM = 0u;     //!< Parse from file stream
        static constexpr unsigned JSON_STREAM = 1u;     //!< Parse from json stream

        /*!
        \brief Parse from input stream
        \param pmap Result
        \param in Input stream
        \param type Input type, see constants above
        */
        static void from(PixelIndexToEp& pmap, std::istream& in, unsigned type=FILE_STREAM);

};

/*!
\brief Write in json format to output stream
\param out Output stream
\param pmap The pixel to energy point mapping
\return Output stream
*/
std::ostream& operator<<(std::ostream& out, const PixelIndexToEp& pmap);

#endif // ENERGY_POINTS_H
