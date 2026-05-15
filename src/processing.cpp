/*!
\file
Event analysis code
*/

#include <fstream>

#include "global.h"
#include "logging.h"
#include "config_file.h"
#include "processing.h"

// anonymous namespace to prevent symbol visibility
namespace {
        Logger& logger = Logger::get("Tpx3App");        //!< Poco logger object
} // anonymous namespace

namespace processing {

        void init()
        {
                auto& gvars = *global::instance;
                auto& time_roi = gvars.time_roi;

                if (gvars.save_interval <= 6000)
                        throw Poco::RuntimeException("save_interval below 6000");

                if (!gvars.server_mode) {
                        ConfigFile config{"Processing.ini"};

                        int TRStart = config.getInt("TRStart");
                        int TRStep = config.getInt("TRStep");
                        int TRN = config.getInt("TRN");
                        std::string output_uri = config.getString("OutputURI");

                        auto& gvars = *global::instance;
                        time_roi.SetTimeROI(TRStart, TRStep, TRN);
                        gvars.output_uri = output_uri;

                        logger << "TRStart=" << TRStart << ", TRStep=" << TRStep << ", TRN=" << TRN
                               << ", Output=" << output_uri << log_info;

                        auto in = std::ifstream("XESPoints.inp");
                        std::unique_ptr<PixelIndexToEp> pmap{new PixelIndexToEp};
                        PixelIndexToEp::from(*pmap, in);
                        gvars.pix_map = pmap->to_map();
                } else {
                        auto TRoiStart = time_roi.TRoiStart;
                        auto TRoiStep = time_roi.TRoiStep;
                        auto TRoiN = time_roi.TRoiN;
                        logger << "TRoiStart=" << TRoiStart << ", TRoiStep=" << TRoiStep << ", TRoiN=" << TRoiN
                               << ", Output=" << gvars.output_uri << log_info;

                        if (gvars.pix_map == nullptr)
                                throw Poco::RuntimeException("Pixelmap uninitialized");
                }
        }

} // namespace processing
