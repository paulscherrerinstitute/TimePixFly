/*!
\file
Event analysis code
*/

#include <iostream>

#include "config_file.h"
#include "xes_data_manager.h"
#include "decoder.h"
#include "processing.h"

// anonymous namespace to prevent symbol visibility
namespace {
        using Decode = AsiRawStreamDecoder;     //!< Raw stream decoder object

        using std::string;
        using std::ios;
        using std::vector;
        using std::chrono::high_resolution_clock;
        using std::chrono::milliseconds;
        using clock = high_resolution_clock;    //!< Clock object

        Logger& logger = Logger::get("Tpx3App");        //!< Poco logger object

        std::unique_ptr<TimeRoi> troiptr;       //!< Pointer to time ROI object, created by init()

        /*!
        \brief Analysis data and operations
        \tparam TOAMode TOA Mode
        */
        template<bool TOAMode>
        struct Analysis final {

                using Data = xes::Data;                 //!< XES data type
                xes::Manager dataManager;               //!< XES data manager

                u8 active = 0;                          //!< active data (the histogram that is beeing built up)

                std::vector<period_type> save_point;    //!< Next period for which a file is written, per chip
                const TimeRoi& time_roi;               //!< Reference to constant time ROI data
                const PixelMap& pix_map;                //!< Reference to pixel mapping
                const float TRoiStep_inv;               //!< 1. / TRoiStep

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
                \brief Reset histogram to zero
                \param data Histogram
                */
                inline void Reset(Data& data)
                {
                        logger << "Reset()" << log_trace;
                        data.Reset();
                }

                /*!
                \brief Add one event to histogram
                TOT must be within (TOTRoiStart,TOTRoiEnd) for this event
                \param data             Histogram
                \param index            Abstract pixel index of event
                \param TimePoint        Clock tick relative to period interval start
                */
                inline void Register(Data& data, PixelIndex index, int TimePoint) const noexcept
                {

//                        logger << "Register(" << (int)dataIndex << ", " << index.chip << ':' << index.flat_pixel << ", " << TimePoint << ", " << TOT << ')' << log_trace;
                        auto map_range = pix_map[index];

                        // const float clb = detector.Calibrate(PixelIndex, TimePoint);
                        for (const auto& part : map_range) {
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//This line takes most of the time of Register (and 50% of time of ProcessEvent)
                                //if (detector.energy_points.npoints!=15) std::cout<<"!!!!!!! ";
                                //std::cout<<part.energy_point<<" ";
                                //in the example detector.energy_points.npoints is always 15
                                // in the example part.energy_point is from 0 to 14 defined by the event coordinate
                                // in the example part.weight is 1;
                                // TimePoint is from 0 to ~2500
                                //std::cout<<TimePoint<<" ";

                                //int iii=index.flat_pixel;

                                //std::cout<<iii<<" pep "<<part.energy_point<<"\n";
                                data.TDSpectra[TimePoint * pix_map.npoints + part.energy_point] += part.weight; // / clb;
                                //data.Energy += part.weight;   // DEBUG ENERGY
                        }
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
                        //      logger << index.chip << ": " << TOT << " outside of ToT ROI " << detector.TOTRoiStart << '-' << detector.TOTRoiEnd << log_debug;

                }

                // /*
                // \brief Add one event to histogram
                // \param data             Histogram
                // \param index            Abstract pixel index of event
                // \param TimePoint        Clock tick relative to period interval start
                // \param TOT              Event TOT value
                // */
                // inline void RegisterXAS(Data& data, PixelIndex index, int TimePoint) noexcept //, u16 TOT) noexcept
                // {
                //         data.TDSpectra[TimePoint * 15] += 1;
                // }



                /*!
                \brief Analyse event and add it to histogram if appropriate
                \param data             Histogram
                \param index            Abstract pixel index of event
                \param reltoa           Event TOA relative to period interval start
                */
                inline void Analyse_ignore_tot(Data& data, PixelIndex index, int64_t reltoa) const noexcept
                {
  //                      logger << "Analyse(" << (int)dataIndex << ", " << index.chip << ':' << index.flat_pixel << ", " << reltoa << ", " << tot << ')' << log_trace;

                        data.Total++;

                        // const u64 FullToA = TOAMode ? reltoa : tot;

                        if (reltoa < (int64_t)time_roi.TRoiStart) {
                                data.BeforeRoi++;
//                                logger << index.chip << ": " << FullToA << " before ToA ROI " << detector.TRoiStart << log_debug;
                        } else if (reltoa >= (int64_t)time_roi.TRoiEnd) {
                                data.AfterRoi++;
//                                logger << index.chip << ": " << FullToA << " after ToA ROI " << detector.TRoiEnd << log_debug;
                        } else { // if ((tot > detector.TOTRoiStart) && (tot < detector.TOTRoiEnd)) {
                                // not ideal here. Does not work if tot step is
                                // not 1

                                // if constexpr (TOAMode == true) {
                                        //have changed here in order to check speed in the XAS mode when information about pixels can be ignored
                                        const int TP = static_cast<int>((reltoa - time_roi.TRoiStart) * TRoiStep_inv);
                                        Register(data, index, TP);
                                        //RegisterXAS(data, index, TP, tot);

                                // } else {
                                //         const int TOTP = tot;
                                //         Register(data, index, TOTP);
                                        //RegisterXAS(data, index, TOTP, tot);
                                // }
                        }
                } // end Analyse()

                /*!
                \brief Purge period interval change from memory

                - If `period` is bigger than next `save_point`, save data in any case.
                - If current `period` <= `no_save`, don't save data.

                ATTENTION: `save_interval` must be big enough in order to not wrap around the data array too quickly!

                \param chipIndex        Chip number
                \param period           Interval change at start of this period will be purged
                \param final            Final forged purge at measurement end
                */
                void PurgePeriod(unsigned chipIndex, period_type period, bool final=false)
                {
//                        logger << "PurgePeriod(" << chipIndex << ", " << period << ')' << log_trace;
                        if (!final) {
                                period_type& sp = save_point[chipIndex];
                                if (period < sp)
                                        return;

                                dataManager.ReturnData(chipIndex, sp);
                                sp += global::instance->save_interval;
                        } else {
                                dataManager.ReturnData(chipIndex, period, true);
                        }
                        // logger << chipIndex << ": purge period " << period << log_info;
                }

                /*!
                \brief Process event
                \param chipIndex        Chip that detected the event
                \param period           Period number of the event
                \param relative_toa     TOA event with time in clock ticks relative to start of `period`
                */
                void ProcessEvent(unsigned chipIndex, const period_type period, toa_event relative_toa)
                {
//                        logger << "ProcessEvent(" << chipIndex << ", " << period << ", " << toaclk << ", " << relative_toaclk << ", " << std::hex << event << std::dec << ')' << log_trace;

                        period_type sp = save_point[chipIndex];
                        if (period >= sp)
                                sp += global::instance->save_interval;

                        //const uint64_t totclk = Decode::getTotClock(event);

//                          logger << chipIndex << ": event: " << period << " (" << xy.first << ' ' << xy.second << ") " << toa << ' ' << tot
//                        << " (" << toaclk << ' ' << totclk << std::hex << event << std::dec << ')' << log_info;

                        auto index = PixelIndex::from(chipIndex, relative_toa.px);
                        Analyse_ignore_tot(dataManager.DataForPeriod(chipIndex, sp), index, relative_toa.ts);

                }

        }; // end type Analysis

        std::unique_ptr<Analysis<TimeRoi::TOAMode>> analysis;    //!< Analysis object

} // anonymous namespace

namespace processing {

        void init()
        {
                const auto& gvars = *global::instance;

                if (gvars.save_interval <= 6000)
                        throw Poco::RuntimeException("save_interval below 6000");

                if (!gvars.server_mode) {
                        ConfigFile config{"Processing.ini"};

                        int TRStart = config.getInt("TRStart");
                        int TRStep = config.getInt("TRStep");
                        int TRN = config.getInt("TRN");
                        std::string output_uri = config.getString("OutputURI");

                        global::instance->TRoiStart = TRStart;
                        global::instance->TRoiStep = TRStep;
                        global::instance->TRoiN = TRN;
                        global::instance->output_uri = output_uri;

                        logger << "TRStart=" << TRStart << ", TRStep=" << TRStep << ", TRN=" << TRN
                               << ", Output=" << output_uri << log_info;

                        auto in = std::ifstream("XESPoints.inp");
                        std::unique_ptr<PixelIndexToEp> pmap{new PixelIndexToEp};
                        PixelIndexToEp::from(*pmap, in);
                        global::instance->pix_map = pmap->to_map();

                        troiptr.reset(new TimeRoi{});
                        troiptr->SetTimeROI(TRStart, TRStep, TRN);
                } else {
                        const auto& output_uri = gvars.output_uri;
                        auto TRoiStart = gvars.TRoiStart.load();
                        auto TRoiStep = gvars.TRoiStep.load();
                        auto TRoiN = gvars.TRoiN.load();
                        logger << "TRoiStart=" << TRoiStart << ", TRoiStep=" << TRoiStep << ", TRoiN=" << TRoiN
                               << ", Output=" << output_uri << log_info;

                        if (gvars.pix_map == nullptr)
                                throw Poco::RuntimeException("Pixelmap uninitialized");

                        troiptr.reset(new TimeRoi{});
                        troiptr->SetTimeROI(TRoiStart, TRoiStep, TRoiN);
                }

                analysis.reset(new Analysis<TimeRoi::TOAMode>{*troiptr});
        }

        void purgePeriod(unsigned chipIndex, period_type period, bool final)
        {
                analysis->PurgePeriod(chipIndex, period, final);
        }

        void processEvent(unsigned chipIndex, const period_type period, toa_event relative_toa)
        {
                analysis->ProcessEvent(chipIndex, period, relative_toa);
        }

        void stop()
        {
                analysis.reset(nullptr);
        }

} // namespace processing
