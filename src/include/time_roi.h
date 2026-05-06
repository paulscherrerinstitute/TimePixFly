#pragma once

#ifndef DETECTOR_H
#define DETECTOR_H

/*!
\file
Detector description
*/

#include "shared_types.h"
#include "logging.h"
#include "pixel_map.h"

/*!
\brief Constant detector data
*/
struct Detector final {
        const detector_layout& layout;  //!< Detector layout reference

        /*!
        \brief Histogramming mode

        If TOAMode is false then TOT is used for binnig (counts as a
        function of energy and TOT as output)
        */
        static constexpr bool TOAMode = true;

        // static constexpr u16 TOTRoiStart = 0;           //!< ROI start in terms of TOT
        // static constexpr u16 TOTRoiEnd = 64000;         //!< ROI end in terms of TOT

        // u64 TRoiStart = TOAMode ? 0 : TOTRoiStart;      //!< ROI start offset in clock ticks relative to interval start
        u64 TRoiStart = 0;                              //!< ROI start offset in clock ticks relative to interval start
        u64 TRoiStep = 1;                               //!< Histogram bin width in clock ticks
        // u64 TRoiN = TOAMode ? 5000 : 100;               //!< Number of histogram bins
        u64 TRoiN = 5000;                               //!< Number of histogram bins
        u64 TRoiEnd = TRoiStart + TRoiStep * TRoiN;     //!< ROI end offset in clock ticks relative to interval start

        const PixelMap& pix_map;                        //!< Abstract pixel index to energy point mapping

        /*!
        \brief Set region of interest within period interval

        Values are in steps of 1.5625 ns

        \param tRoiStart        Start clock tick
        \param tRoiStep         Step size
        \param tRoiN            Number of steps to end
        */
        inline void SetTimeROI(int tRoiStart, int tRoiStep, int tRoiN)
        {
                Logger& logger = Logger::get("Tpx3App");
                logger << "SetTimeROI(" << tRoiStart << ", " << tRoiStep << ", " << tRoiN << ')' << log_trace;

                if ((tRoiStep <= 0) && (tRoiN <= 0))
                        throw std::invalid_argument("TRoiStep and TRoiN must be positive");

                TRoiStart = tRoiStart;
                TRoiStep = tRoiStep;
                TRoiN = tRoiN;

                TRoiEnd = TRoiStart + TRoiStep * TRoiN;
                logger << "Detector TRoiStart=" << TRoiStart << " TRoiStep=" << TRoiStep << " TRoiN=" << TRoiN << " TRoiEnd=" << TRoiEnd << log_debug;
        }

        /*!
        \brief Constructor
        \param layout_ Detector layout reference
        \param pm Pixel index to energy point mapping
        */
        inline Detector(const detector_layout& layout_, const PixelMap& pm)
            : layout{layout_}, pix_map{pm}
        {}

        ~Detector() = default;
};  // end type Detector

#endif
