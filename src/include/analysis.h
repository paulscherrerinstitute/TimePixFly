#pragma once

#ifndef ANALYSIS_H
#define ANALYSIS_H

#include <vector>
#include "xes_data_manager.h"

/*!
\brief Analysis data and operations
\tparam TOAMode TOA Mode
*/
class Analysis final {

    using Data = xes::Data;                 //!< XES data type
    xes::Manager dataManager;               //!< XES data manager

    std::vector<period_type> save_point;    //!< Next period for which a file is written, per chip
    const TimeRoi& time_roi;               //!< Reference to constant time ROI data
    const PixelMap& pix_map;                //!< Reference to pixel mapping
    const float TRoiStep_inv;               //!< 1. / TRoiStep

    /*!
    \brief Add one event to histogram

    TOT must be within (TOTRoiStart,TOTRoiEnd) for this event
    \param data             Histogram
    \param index            Abstract pixel index of event
    \param TimePoint        Clock tick relative to period interval start
    */
    inline void Register(Data& data, PixelIndex index, int TimePoint) const noexcept
    {
            auto map_range = pix_map[index];
            for (const auto& part : map_range) {
                data.TDSpectra[TimePoint * pix_map.npoints + part.energy_point] += part.weight;
            }
    }

  public:
    /*!
    \brief Constructor
    \param det Constant detector data
    \param uri Output file:name (without period and .xes), or tcp:host:port
    */
    inline explicit Analysis(const TimeRoi& troi)
        : dataManager{},
          save_point(global::instance->layout.chip.size(), global::instance->save_interval),
          time_roi{troi}, pix_map{*global::instance->pix_map},
          TRoiStep_inv{1.f/time_roi.TRoiStep}
    {
            dataManager.Reset();
    }

    /*!
    \brief Analyse event and add it to histogram if appropriate
    \param data             Histogram
    \param index            Abstract pixel index of event
    \param reltoa           Event TOA relative to period interval start
    */
    inline void Analyse_ignore_tot(Data& data, PixelIndex index, int64_t reltoa) const noexcept
    {
        data.Total++;

        if (reltoa < (int64_t)time_roi.TRoiStart) {
            data.BeforeRoi++;
        } else if (reltoa >= (int64_t)time_roi.TRoiEnd) {
            data.AfterRoi++;
        } else {
            const int TP = (reltoa - time_roi.TRoiStart) * TRoiStep_inv;
            Register(data, index, TP);
        }
    }

    /*!
    \brief Purge period interval change from memory

    - If `period` is bigger than next `save_point`, save data in any case.
    - If current `period` <= `no_save`, don't save data.

    ATTENTION: `save_interval` must be big enough in order to not wrap around the data array too quickly!

    \param chipIndex        Chip number
    \param period           Interval change at start of this period will be purged
    \param final            Final forged purge at measurement end
    */
    inline void PurgePeriod(unsigned chipIndex, period_type period, bool final=false)
    {
        if (!final) {
            period_type& sp = save_point[chipIndex];
            if (period < sp)
                return;

            dataManager.ReturnData(chipIndex, sp);
            sp += global::instance->save_interval;
        } else {
            dataManager.ReturnData(chipIndex, period, true);
        }
    }

    /*!
    \brief Process event
    \param chipIndex        Chip that detected the event
    \param period           Period number of the event
    \param relative_toa     TOA event with time in clock ticks relative to start of `period`
    */
    inline void ProcessEvent(unsigned chipIndex, const period_type period, toa_event relative_toa)
    {
        period_type sp = save_point[chipIndex];
        if (period >= sp)
            sp += global::instance->save_interval;

        auto index = PixelIndex::from(chipIndex, relative_toa.px);
        Analyse_ignore_tot(dataManager.DataForPeriod(chipIndex, sp), index, relative_toa.ts);
    }
}; // end type Analysis

#endif // ifndef ANALYSIS_H