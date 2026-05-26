#pragma once

#ifndef XES_DATA_H
#define XES_DATA_H

/*!
\file
Provide data container for XES data
*/

#include <string>
#include <fstream>
#include "aligned_allocator.h"
#include "global.h"
#include "time_roi.h"

namespace xes {
    /*!
    \brief TDSpectra data aggregated over one data saving period
    */
    struct Data final {
            using element_type = float;     //!< Element type
            using histo_type = std::vector<element_type, aligned_allocator<element_type>>; //!< Histogram type
            histo_type TDSpectra;           //!< Result spectra indexed by [time_point * NumEnergyPoints + energy_point]

            int BeforeRoi = 0;              //!< Number of events before roi
            int AfterRoi = 0;               //!< Number of events after roi
            int Total = 0;                  //!< Total events handled
            // float Energy = .0;   // DEBUG ENERGY

            period_type period = 0;         //!< Last seen period for this data

            /*!
            \brief Create TDSpectra data container
            The size of the container will be be troi.TRoiN * pix_map->npoints
            \param troi Time ROI
            */
            inline explicit Data(const TimeRoi& troi)
                : TDSpectra(troi.TRoiN * global::instance->pix_map->npoints)
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
            inline Data& operator+=(const Data& data) noexcept
            {
                assert(data.TDSpectra.size() == TDSpectra.size());
                for (histo_type::size_type i=0; i<TDSpectra.size(); i++)
                    TDSpectra[i] += data.TDSpectra[i];
                BeforeRoi += data.BeforeRoi;
                AfterRoi += data.AfterRoi;
                Total += data.Total;
                return *this;
            }

            /*!
            \brief Aggregate rhs partial TDSpectra into this one and reset rhs
            \param rhs Right hand side partial TDSpectra
            */
            inline void addResetRhs(Data& rhs) noexcept
            {
                assert(rhs.TDSpectra.size() == TDSpectra.size());
                for (histo_type::size_type i=0; i<TDSpectra.size(); i++) {
                    TDSpectra[i] += rhs.TDSpectra[i];
                    rhs.TDSpectra[i] = histo_type::value_type{};
                }
                BeforeRoi += rhs.BeforeRoi;
                AfterRoi += rhs.AfterRoi;
                Total += rhs.Total;
                // Energy += rhs.Energy;   // DEBUG ENERGY
                rhs.BeforeRoi = rhs.AfterRoi = rhs.Total = 0;
                // rhs.Energy = .0;   // DEBUG ENERGY
                rhs.period = 0;
            }

            /*!
            \brief Initialize TDSpectra
            The size of the container will be be troi.TRoiN * pix_map->npoints
            \param troi Time ROI
            */
            inline void Init(const TimeRoi& troi)
            {
                TDSpectra.resize(troi.TRoiN * global::instance->pix_map->npoints);
            }

            /*!
            \brief Reset TDSpectra to zero
            */
            inline void Reset() noexcept
            {
                std::fill(TDSpectra.begin(), TDSpectra.end(), histo_type::value_type{});
                BeforeRoi = AfterRoi = Total = 0;
                // Energy = .0;   // DEBUG ENERGY
                period = 0;
            }
    };

} // namespace xes

/*!
\brief Less operator for XES Data
*/
template<>
struct std::less<xes::Data> final {
    /*!
    \brief Call
    \param lhs Left hand side XES data
    \param rhs Right hand side XES data
    \return True if lhs is less than rhs period
    */
    inline bool operator()(const xes::Data& lhs, const xes::Data& rhs) const noexcept
    {
        return lhs.period < rhs.period;
    }
};

#endif
