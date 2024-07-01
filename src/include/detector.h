#pragma once

#ifndef DETECTOR_H
#define DETECTOR_H

/*!
\file
Detector description
*/

#include "shared_types.h"
#include "layout.h"
#include "logging.h"

/*!
\brief Constant detector data
*/
struct Detector final {
        const detector_layout& layout;  //!< Detector layout reference
        int DetWidth;                   //!< Detector width
        int NumPixels;                  //!< Detector number of pixels

        /*!
        \brief Histogramming mode
        
        If TOAMode is false then TOT is used for binnig (counts as a
        function of energy and TOT as output)
        */
        static constexpr bool TOAMode = true;

        static constexpr u16 TOTRoiStart = 0;           //!< ROI start in terms of TOT
        static constexpr u16 TOTRoiEnd = 64000;           //!< ROI end in terms of TOT

        u64 TRoiStart = TOAMode ? 0 : TOTRoiStart;      //!< ROI start offset in clock ticks relative to interval start
        u64 TRoiStep = 1;                               //!< Histogram bin width in clock ticks
        u64 TRoiN = TOAMode ? 5000 : 100;               //!< Number of histogram bins
        u64 TRoiEnd = TRoiStart + TRoiStep * TRoiN;     //!< ROI end offset in clock ticks relative to interval start

        PixelIndexToEp energy_points;   //!< Abstract pixel index to energy point mapping

        /*!
        \brief Set region of interest within period interval

        Values are in steps of 1.5625 ns

        \param tRoiStart        Start clock tick
        \param tRoiStep         Step size
        \param tRoiN            Number of steps to end
        */
        inline void SetTimeROI(int tRoiStart, int tRoiStep, int tRoiN) noexcept
        {
                Logger& logger = Logger::get("Tpx3App");
                logger << "SetTimeROI(" << tRoiStart << ", " << tRoiStep << ", " << tRoiN << ')' << log_trace;
                TRoiStart = tRoiStart;
                TRoiStep = tRoiStep;
                TRoiN = tRoiN;

                TRoiEnd = TRoiStart + TRoiStep * TRoiN;
                logger << "Detector TRoiStart=" << TRoiStart << " TRoiStep=" << TRoiStep << " TRoiN=" << TRoiN << " TRoiEnd=" << TRoiEnd << log_debug;
        }

        /*!
        \brief Get number of detector chips
        \return Number of detector chips
        */
        [[gnu::const]] inline unsigned NumChips() const noexcept
        {
                return layout.chip.size();
        }

        /*!
        \brief Constructor
        \param layout_ Detector layout reference
        */
        inline Detector(const detector_layout& layout_)
            : layout{layout_}, DetWidth(layout.width), NumPixels(layout.width * layout.height)
        {}

        ~Detector() = default;
};  // end type Detector

#endif
