#pragma once

#ifndef XES_DATA_H
#define XES_DATA_H

/*!
\file
Provide data container for XES data
*/

#include <string>
#include <fstream>
#include "detector.h"

namespace xes {
    /*!
    \brief TDSpectra data aggregated over one data saving period
    */
    struct Data final {
            const Detector* detector = nullptr; //!< This data refers to detector

    //  Modified type of vector to check speed in the XAS mode (when there is no division of pixels over a few points)
            using histo_type = std::vector<int>;    //!< Histogram type
            // using histo_type = std::vector<float>;
            histo_type TDSpectra;           //!< Result spectra indexed by [time_point * NumEnergyPoints + energy_point]

            int BeforeRoi = 0;              //!< Number of events before roi
            int AfterRoi = 0;               //!< Number of events after roi
            int Total = 0;                  //!< Total events handled

            /*!
            \brief Create TDSpectra data container
            The size of the container will be be det.TRoiN * det.energy_points.npoints
            \param det Detector data
            */
            inline explicit Data(const Detector& det)
                : detector(&det), TDSpectra(det.TRoiN * det.energy_points.npoints)
            {}

            inline Data() = default;                        //!< Default constructor
            inline Data(const Data&) = default;             //!< Copy constructor
            inline Data(Data&&) = default;                  //!< Move constructor
            inline ~Data() = default;                       //!< Destructor

            /*!
            \brief Assignment
            \return this
            */
            inline Data& operator=(const Data&) = default;

            /*!
            \brief Move assignment
            \return this
            */
            inline Data& operator=(Data&&) = default;

            /*!
            \brief Aggregate another partial TDSpectra into this one
            \param data other Data
            \return *this
            */
            inline Data& operator+=(const Data& data)
            {
                assert(data.TDSpectra.size() == TDSpectra.size());
                for (histo_type::size_type i=0; i<data.TDSpectra.size(); i++)
                    TDSpectra[i] += data.TDSpectra[i];
                return *this;
            }

            /*!
            \brief Initialize TDSpectra
            The size of the container will be be det.TRoiN * det.energy_points.npoints
            \param det Detector data
            */
            inline void Init(const Detector& det)
            {
                detector = &det;
                TDSpectra.resize(det.TRoiN * det.energy_points.npoints);
            }

            /*!
            \brief Reset TDSpectra to zero
            */
            inline void Reset()
            {
                std::fill(TDSpectra.begin(), TDSpectra.end(), 0);
                BeforeRoi = AfterRoi = Total = 0;
            }
    };

} // namespace xes

#endif
