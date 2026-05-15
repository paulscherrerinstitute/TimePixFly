/*!
\file
Event analysis code
*/

#include <iostream>

#include "config_file.h"
#include "analysis.h"
#include "processing.h"

// anonymous namespace to prevent symbol visibility
namespace {
        using std::string;
        using std::ios;
        using std::chrono::high_resolution_clock;
        using std::chrono::milliseconds;
        using clock = high_resolution_clock;    //!< Clock object

        Logger& logger = Logger::get("Tpx3App");        //!< Poco logger object

        std::unique_ptr<TimeRoi> troiptr;       //!< Pointer to time ROI object, created by init()
        std::unique_ptr<Analysis> analysis;    //!< Analysis object

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

                analysis.reset(new Analysis{*troiptr});
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
