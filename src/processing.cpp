/*!
\file
Event analysis code
*/

#include <chrono>
#include <fstream>
#include <iostream>
#include <vector>
#include <array>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <memory>

#include "shared_types.h"
#include "logging.h"
#include "decoder.h"
#include "pixel_index.h"
#include "energy_points.h"
#include "shared_types.h"
#include "processing.h"
#include "detector.h"
#include "xes_data.h"
#include "xes_data_manager.h"

#include "Poco/Util/IniFileConfiguration.h"

// anonymous namespace to prevent symbol visibility
namespace {
        using Decode = AsiRawStreamDecoder;     //!< Raw stream decoder object

        using std::string;

        using std::cout;
        using std::endl;
        using std::ios;

        using std::vector;

        using std::chrono::duration_cast;
        using std::chrono::high_resolution_clock;
        using std::chrono::milliseconds;
        using std::chrono::duration;
        using clock = high_resolution_clock;    //!< Clock object

        using std::remove;
        using std::exit;

        Logger& logger = Logger::get("Tpx3App");        //!< Poco logger object

        const period_type save_interval = 131000;       //!< Histogram saving period: ~1s for TDC frequency 131kHz

        /*!
        \brief Processing configuration file object
        */
        struct ConfigFile final : public Poco::Util::IniFileConfiguration {
                /*!
                \brief Constructor
                \param path INI style configuration file path
                */
                inline ConfigFile(const std::string& path)
                        : IniFileConfiguration{path}
                {}

                inline ~ConfigFile() noexcept = default;
        };

        std::unique_ptr<Detector> detptr;       //!< Pointer to detector object, created by init()

        /*!
        \brief Parse object of type T in a string
        \param s   String having a representation of T at pos
        \param pos Position in the string from where to parse
        \return Parsed object of type Tualified-id before ‘>’ token
        */
        template <typename T>
        T parse(std::string_view& s, std::string_view::size_type pos)
        {
                std::istringstream iss(s.substr(pos).data());
                T t;
                iss >> t;
                if (!iss)
                        throw std::ios_base::failure("failed to parse XESPoints file data");
                return t;
        }

        /*!
        \brief Read region of interest related to are (pixel to energy point mapping)

        The file contains lines in the form

        chip flatPixel energyPoint0 weight0 [energyPoint1 weight1 ...]

        \param energy_points    Set this mapping to what was defined in XESPointsFile
        \param layout           The detector layout
        \param XESPointsFile    Name of the file that defines the pixel to energy point mapping
        */
        void readAreaROI(PixelIndexToEp& energy_points, const detector_layout& layout, const std::string& XESPointsFile)
        {
                logger << "readAreaROI(" << XESPointsFile << ')' << log_trace;
                const auto numPixels = chip_size * chip_size;
                const auto numChips = layout.chip.size();

                energy_points.chip.resize(numChips);
                for (auto& chip: energy_points.chip) {
                        chip.flat_pixel.resize(numPixels);
                }

                constexpr size_t bufSize = 1024;
                char buf[bufSize] = {0};
                std::string_view::size_type posN[bufSize] = {0};
                std::ifstream ifs(XESPointsFile);
                if (! ifs)
                        throw std::ios_base::failure(std::string{"failed to open "} + XESPointsFile);

                for (unsigned line=1;;line++) {
                        // i, j, XESEnergyIndex[i,j,k]..., XESWeight [i,j,k]...
                        if (! ifs.getline(buf, bufSize).good()) {
                                if (! ifs.eof())
                                        throw std::ios_base::failure(std::string{"failed to parse XESPoints file at line "} + std::to_string(line));
                                break;
                        }
                        std::string_view s(buf);
                        std::string_view::size_type pos = 0;
                        unsigned count = 0;
                        posN[count] = pos;
                        while ((pos = s.find(',', pos)) != std::string_view::npos) {
                                count++;
                                pos++;
                                posN[count] = pos;
                        }
                        count += 1;
                        if (count < 2)
                                throw std::invalid_argument("invalid XESPoints file line (count < 2)");
                        if ((count % 2) != 0)
                                throw std::invalid_argument("invalid XESPoints file line (count % 2 != 0)");
                        unsigned k = parse<unsigned>(s, posN[0]);       // chip
                        if (k >= numChips)
                                throw std::invalid_argument("invalid chip number in XESPoints file");
                        unsigned l = parse<unsigned>(s, posN[1]);       // flatPixel
                        if (l >= numPixels)
                                throw std::invalid_argument("invalid pixel number in XESPoints file");
                        FlatPixelToEp& pixel = energy_points.at(PixelIndex::from(k, l));
                        const unsigned numEnergyPoints = (count - 2u) / 2u;
                        for (unsigned m=0; m<numEnergyPoints; m++) {
                                EpPart part;
                                part.energy_point = parse<unsigned>(s, posN[2+m]);
                                energy_points.npoints = std::max(energy_points.npoints, part.energy_point);
                                pixel.part.push_back(std::move(part));
                        }
                        for (unsigned m=0; m<numEnergyPoints; m++)
                                pixel.part[m].weight = parse<float>(s, posN[2+numEnergyPoints+m]);
                }

                energy_points.npoints += 1;
                logger << "num energy points: " << energy_points.npoints << log_debug;

	/*
	//check that it was read correctly:
	for (int ii=0;ii<numPixels; ii++) {
	    int ep1=energy_points.at(PixelIndex::from(0, ii)).part[0].energy_point;
	    float wp1=energy_points.at(PixelIndex::from(0, ii)).part[0].weight;


	    std::cout<<"ep1wp1="<<ep1<<"  "<<wp1<<"\n";
	    }
	*/
        }

        /*!
        \brief Analysis data and operations
        \tparam TOAMode TOA Mode
        */
        template<bool TOAMode>
        struct Analysis final {

                using Data = xes::Data;                 //!< XES data type
                xes::Manager dataManager;               //!< XES data manager

                u8 active = 0;                          //!< active data (the histogram that is beeing built up)

                                                        //TOTMin and TOTMax probably can be removed. Not really used. TOTROIStart are the ROI...
                u16 TOTMin = 0;                         //!< Minimum TOT value accepted
                u16 TOTMax = 64000;                     //!< Maximum TOT value accepted

                static constexpr period_type no_save = 2; //!< Don't save save data before this period
                std::vector<period_type> save_point;    //!< Next period for which a file is written
                const Detector& detector;               //!< Reference to constant Detector data
                const float TRoiStep_inv;               //!< 1. / TRoiStep

                /*!
                \brief Constructor
                \param det      Constant detector data
                \param OutFName Output file name
                */
                inline Analysis(const Detector& det, const std::string& OutFName)
                        : dataManager{det, OutFName, 3},
                          save_point(det.layout.chip.size(), no_save),
                          detector{det},
                          TRoiStep_inv{1.f/detector.TRoiStep}
                {}

                /*!
                \brief Reset histogram to zero
                \param data Histogram
                */
                inline void Reset(Data& data)
                {
                        logger << "Reset()" << log_trace;
                        data.Reset();
                }

                // /*!
                // \brief Save histogram to .xes file

                // The extension .xes will be appended to the output file path.

                // \param data             Histogram
                // \param OutFileName      Path to output file without .xes extension
                // */
                // inline void SaveToFile(Data& data, const string& OutFileName) const
                // {
                //         logger << "SaveToFile(" << OutFileName << ')' << log_trace;
                //         const auto t1 = clock::now();
                //         data.SaveToFile(OutFileName);
                //         const auto t2 = clock::now();
                //         auto save_time = duration_cast<milliseconds>(t2 - t1).count();
                //         logger << "save to " << OutFileName << ", time " << save_time << " ms" << log_debug;
                // }

                /*!
                \brief Add one event to histogram
                TOT must be within (TOTRoiStart,TOTRoiEnd) for this event
                \param data             Histogram
                \param index            Abstract pixel index of event
                \param TimePoint        Clock tick relative to period interval start
                */
                inline void Register(Data& data, PixelIndex index, int TimePoint) noexcept
                {


//                        logger << "Register(" << (int)dataIndex << ", " << index.chip << ':' << index.flat_pixel << ", " << TimePoint << ", " << TOT << ')' << log_trace;
			//std::cout<<"e"<<TOT<<"  "<<detector.TOTRoiStart<<"  "<<detector.TOTRoiEnd<<"\n";
                        //TOT is always 100 which is probably wrong. Check....
                        //std::cout<<"w";
                        const auto& flat_pixel = detector.energy_points[index];

                        //std::cout<<"q";
                        // const float clb = detector.Calibrate(PixelIndex, TimePoint);
                        for (const auto& part : flat_pixel.part) {
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
                                data.TDSpectra[TimePoint * detector.energy_points.npoints + part.energy_point] += part.weight; // / clb;
                        }
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
                        //      logger << index.chip << ": " << TOT << " outside of ToT ROI " << detector.TOTRoiStart << '-' << detector.TOTRoiEnd << log_debug;

                }

                // /*!
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
                \param tot              Event TOT value
                */
                inline void Analyse(Data& data, PixelIndex index, int64_t reltoa, int64_t tot) noexcept
                {
  //                      logger << "Analyse(" << (int)dataIndex << ", " << index.chip << ':' << index.flat_pixel << ", " << reltoa << ", " << tot << ')' << log_trace;
                        //----------------------------------------------------------------------
                        // parsing one data line
                        //----------------------------------------------------------------------
                        // double fulltoa = toa*25.0 - ftoa*25.0/16.0;
                        // double ftoaC=ftoa*1.0;

                        data.Total++;

                        // temporary here
                        //if (tot < TOTMin)
                        //        TOTMin = tot;
                        //if (tot > TOTMax)
                        //        TOTMax = tot;
                        // end of temporary

                        const u64 FullToA = TOAMode ? reltoa : tot;

                        if (FullToA < detector.TRoiStart) {
                                data.BeforeRoi++;
//                                logger << index.chip << ": " << FullToA << " before ToA ROI " << detector.TRoiStart << log_debug;
                        } else if (FullToA >= detector.TRoiEnd) {
                                data.AfterRoi++;
//                                logger << index.chip << ": " << FullToA << " after ToA ROI " << detector.TRoiEnd << log_debug;
                        } else if ((tot > detector.TOTRoiStart) && (tot < detector.TOTRoiEnd)) {
                                // not ideal here. Does not work if tot step is
                                // not 1

                                if constexpr (TOAMode == true) {
                                        //have changed here in order to check speed in the XAS mode when information about pixels can be ignored
                                        const int TP = static_cast<int>((FullToA - detector.TRoiStart) * TRoiStep_inv);
                                        Register(data, index, TP);
                                        //RegisterXAS(data, index, TP, tot);

                                } else {
                                        const int TOTP = tot;
                                        Register(data, index, TOTP);
                                        //RegisterXAS(data, index, TOTP, tot);
                                }
                        }
                } // end Analyse()

                /*!
                \brief Purge period interval change from memory

                - If `period` is bigger than next `save_point`, save data in any case.
                - If current `period` <= `no_save`, don't save data.

                ATTENTION: `save_interval` must be big enough in order to not wrap around the data array too quickly!

                \param chipIndex        Chip number
                \param period           Interval change at start of this period will be purged
                */
                void PurgePeriod(unsigned chipIndex, period_type period)
                {
//                        logger << "PurgePeriod(" << chipIndex << ", " << period << ')' << log_trace;
//                        logger << chipIndex << ": purge period " << period << log_info;
                        period_type& sp = save_point[chipIndex];
                        if (period < sp)
                                return;

                        if (sp == no_save) {
                                sp += save_interval;
                                return;
                        }

                        dataManager.ReturnData(chipIndex, sp);
                        sp += save_interval;
                }

                /*!
                \brief Process event
                \param chipIndex        Chip that detected the event
                \param period           Period number of the event
                \param toaclk           Event TOA in clock ticks
                \param relative_toaclk  Event TOA in clock ticks relative to start of `period`
                \param event            Raw event
                */
                // void ProcessEvent(unsigned chipIndex, const period_type period, int64_t toaclk, int64_t relative_toaclk, uint64_t event)
                void ProcessEvent(unsigned chipIndex, const period_type period, int64_t relative_toaclk, uint64_t event)
                {
//                        logger << "ProcessEvent(" << chipIndex << ", " << period << ", " << toaclk << ", " << relative_toaclk << ", " << std::hex << event << std::dec << ')' << log_trace;

                        period_type sp = save_point[chipIndex];
                        if (period > sp)
                                sp += save_interval;

                        // substituted tot by constant to test speed since tot is typically ignored

                        const uint64_t totclk = Decode::getTotClock(event);
                        //const uint64_t totclk = 100;



                        //const float toa = Decode::clockToFloat(toaclk);
                        //const float tot = Decode::clockToFloat(totclk, 40e6);


                        // commented to test speed since xy is typically ignored for XAS (not for XES!)

                        const std::pair<uint64_t, uint64_t> xy = Decode::calculateXY(event);


//                          logger << chipIndex << ": event: " << period << " (" << xy.first << ' ' << xy.second << ") " << toa << ' ' << tot
//                        << " (" << toaclk << ' ' << totclk << std::hex << event << std::dec << ')' << log_info;

                        //can be replaced to test speed in XAS mode

                        auto index = PixelIndex::from(chipIndex, xy);
                        //auto index = PixelIndex::from(chipIndex, 10);



                        {
                                // std::lock_guard lock{histo_lock}; // <---- problematic lock
                                Analyse(dataManager.DataForPeriod(chipIndex, sp), index, relative_toaclk, totclk);
                        }

                }

        }; // end type Analysis

        std::unique_ptr<Analysis<Detector::TOAMode>> analysis;    //!< Analysis object

} // anonymous namespace

namespace processing {

        void init(const detector_layout& layout)
        {
                ConfigFile config{"Processing.ini"};

                /*
                using std::getline;
                using std::ws;

                std::ifstream ProcessingFile("Processing.inp");

                string Comment;
                string FileInputPath, FileOutputPath, ShortFileName;

                getline(ProcessingFile, Comment);
                getline(ProcessingFile, FileInputPath);
                getline(ProcessingFile, Comment);
                getline(ProcessingFile, FileOutputPath);
                getline(ProcessingFile, Comment);
                getline(ProcessingFile, ShortFileName);

                // int NumCalibFactors; //, FileIndexBegin, FileIndexEnd, FileIndexStep;

                // getline(ProcessingFile, Comment);
                // cout << Comment << "\n";
                // ProcessingFile >> NumCalibFactors >> ws;
                // getline(ProcessingFile, Comment);
                // cout << Comment << "\n";
                // ProcessingFile >> FileIndexBegin >> ws;
                // getline(ProcessingFile, Comment);
                // ProcessingFile >> FileIndexEnd >> ws;
                // getline(ProcessingFile, Comment);
                // ProcessingFile >> FileIndexStep >> ws;

                // int DetROIStart, DetROIEnd, BinSize;
                // bool BinVerticalOrientation;
                int TRStart, TRStep, TRN;
                // bool DeleteAfter;

                // getline(ProcessingFile, Comment);
                // ProcessingFile >> DetROIStart >> ws;
                // getline(ProcessingFile, Comment);
                // ProcessingFile >> DetROIEnd >> ws;
                // getline(ProcessingFile, Comment);
                // ProcessingFile >> BinVerticalOrientation >> ws;
                // getline(ProcessingFile, Comment);
                // ProcessingFile >> BinSize >> ws;
                getline(ProcessingFile, Comment);
                ProcessingFile >> TRStart >> ws;
                getline(ProcessingFile, Comment);
                ProcessingFile >> TRStep >> ws;
                getline(ProcessingFile, Comment);
                ProcessingFile >> TRN >> ws;
                // getline(ProcessingFile, Comment);
                // ProcessingFile >> DeleteAfter >> ws;

                ProcessingFile.close();
                if (ProcessingFile.fail())
                        throw std::ios_base::failure("failed to parse ProcessingFile");
                */

                int TRStart = config.getInt("TRStart");
                int TRStep = config.getInt("TRStep");
                int TRN = config.getInt("TRN");

                // std::string FileInputPath = config.getString("FileInputPath");
                std::string FileOutputPath = config.getString("FileOutputPath");
                std::string ShortFileName = config.getString("ShortFileName");

                logger << "TRStart=" << TRStart << ", TRStep=" << TRStep << ", TRN=" << TRN
                       << ", FileOutputPath=" << FileOutputPath << ", ShortFileName=" << ShortFileName << log_info;

                detptr.reset(new Detector{layout});
                detptr->SetTimeROI(TRStart, TRStep, TRN);
                readAreaROI(detptr->energy_points, layout, "XESPoints.inp");

                analysis.reset(new Analysis<Detector::TOAMode>{*detptr, FileOutputPath + ShortFileName});
        }

        void purgePeriod(unsigned chipIndex, period_type period)
        {
                analysis->PurgePeriod(chipIndex, period);
        }

        // void processEvent(unsigned chipIndex, const period_type period, int64_t toaclk, int64_t relative_toaclk, uint64_t event)
        void processEvent(unsigned chipIndex, const period_type period, int64_t relative_toaclk, uint64_t event)
        {
                // analysis->ProcessEvent(chipIndex, period, toaclk, relative_toaclk, event);
                analysis->ProcessEvent(chipIndex, period, relative_toaclk, event);
        }

} // namespace processing
