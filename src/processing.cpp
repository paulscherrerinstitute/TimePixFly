#include <chrono>
#include <fstream>
#include <iostream>
#include <vector>
#include <array>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <mutex>

#include "logging.h"
#include "decoder.h"
#include "pixel_index.h"
#include "energy_points.h"

using period_type = int64_t;

#include "processing.h"

#include "Poco/Util/IniFileConfiguration.h"

// anonymous namespace to prevent symbol visibility
namespace {       
        using Decode = AsiRawStreamDecoder;

        using std::string;

        using std::cout;
        using std::endl;
        using std::ios;

        using std::vector;

        using std::chrono::duration_cast;
        using std::chrono::high_resolution_clock;
        using std::chrono::milliseconds;
        using std::chrono::duration;
        using clock = high_resolution_clock;

        using std::remove;

        using std::exit;

        using u8 = uint8_t;
        using u16 = uint16_t;
        using u64 = uint64_t;

        Logger& logger = Logger::get("Tpx3App");

        const period_type save_interval = 131000;       // ~1s for TDC frequency 131kHz

        struct ConfigFile final : public Poco::Util::IniFileConfiguration {
                inline ConfigFile(const std::string& path)
                        : IniFileConfiguration{path}
                {}

                inline ~ConfigFile() noexcept = default;
        };

        /*!
        * \brief Constant detector data
        */
        struct Detector final {
                const detector_layout& layout;
                int DetWidth;
                int NumPixels;

                // if TOAMode is false then TOT is used for binnig (counts as a
                // function of energy and TOT as output)
                static constexpr bool TOAMode = true;

                static constexpr u16 TOTRoiStart = 1;
                static constexpr u16 TOTRoiEnd = 100;

                u64 TRoiStart = TOAMode ? 0 : TOTRoiStart;
                u64 TRoiStep = 1;
                u64 TRoiN = TOAMode ? 5000 : 100;
                u64 TRoiEnd = TRoiStart + TRoiStep * TRoiN;

                PixelIndexToEp energy_points;

                // In steps of 1.5625 ns
                void SetTimeROI(int tRoiStart, int tRoiStep, int tRoiN) noexcept
                {
                        TRoiStart = tRoiStart;
                        TRoiStep = tRoiStep;
                        TRoiN = tRoiN;

                        TRoiEnd = TRoiStart + TRoiStep * TRoiN;
                }

                [[gnu::const]] unsigned NumChips() const noexcept
                {
                        return layout.chip.size();
                }

                Detector(const detector_layout& layout_)
                : layout{layout_}, DetWidth(layout.width), NumPixels(layout.width * layout.height)
                {}
        };  // end type Detector

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
        }

        std::mutex histo_lock;                      // Lock histogram access
        std::array<std::atomic_uint, 2> save_ok;    // counter for chips that reached the save point

        /*!
        * \brief Analysis data and operations
        */
        struct Analysis final {
                struct Data final {
                        vector<float> TDSpectra;        //!< Result spectra indexed by [time_point * NumEnergyPoints + energy_point]

                        int BeforeRoi = 0;              //!< Number of events before roi
                        int AfterRoi = 0;               //!< Number of events after roi
                        int Total = 0;                  //!< Total events handled
                };

                const std::string outFileName;          // output file name
                period_type save_point = 0;             // next period for which a file is written

                const Detector& detector;               //!< Reference to constant Detector data
                std::array<Data, 2> data;               // histogram data
                u8 active = 0;                          // active data
                u16 TOTMin = 0;                         //!< Minimal energy encountered in handled events
                u16 TOTMax = 0;                         //!< Maximum energy encountered in handled events

                /*!
                * \brief Constructor
                * \param det Constant detector data
                */
                inline Analysis(const Detector& det, const std::string& OutFName)
                : outFileName(OutFName), detector(det)
                {
                        for (auto& histo: data)
                                histo.TDSpectra.resize(det.TRoiN * det.energy_points.npoints);
                }

                inline void Reset(const u8 data_index)
                {
                        logger << "Reset(" << data_index << ')' << log_trace;
                        auto& d = data[data_index];
                        auto& tds = d.TDSpectra;
                        std::fill(tds.begin(), tds.end(), 0);
                        d.BeforeRoi = 0;
                        d.AfterRoi = 0;
                        d.Total = 0;
                }

                /*!
                * \brief Save spectra to .xes
                * The extension .xes will be appended to the output file path.
                * \param OutFileName Path to output file without .xes extension
                */
                inline void SaveToFile(const u8 data_index, const string& OutFileName) const
                {
                        logger << "SaveToFile(" << data_index << ", " << OutFileName << ')' << log_trace;
                        const auto t1 = clock::now();
                        std::ofstream OutFile(OutFileName + ".xes");

                        const int NumEnergyPoints = detector.energy_points.npoints;
                        for (int i=0; i<NumEnergyPoints; ++i) {
                                for (u64 j=0; j<detector.TRoiN; ++j) {
                                        OutFile << data[data_index].TDSpectra[j * NumEnergyPoints + i] << " ";
                                }
                                OutFile << "\n";
                        }
                        const auto t2 = clock::now();
                        if (OutFile.fail())
                                throw std::ios_base::failure("Detector::SaveToFile failed");
                        OutFile.close();
                        auto save_time = duration_cast<milliseconds>(t2 - t1).count();
                        logger << "save to " << OutFileName << ", time " << save_time << " ms" << log_debug;
                }

                inline void Register(const u8 data_index, PixelIndex index, int TimePoint, u16 TOT) noexcept
                {
                        if ((TOT > detector.TOTRoiStart) && (TOT < detector.TOTRoiEnd)) {
                                const auto& flat_pixel = detector.energy_points[index];
                                if (! flat_pixel.part.empty()) {
                                        // const float clb = detector.Calibrate(PixelIndex, TimePoint);
                                        for (const auto& part : flat_pixel.part)
                                                data[data_index].TDSpectra[TimePoint * detector.energy_points.npoints + part.energy_point] += part.weight; // / clb;
                                }
                        }
                }

                inline void Analyse(const u8 dataIndex, PixelIndex index, int64_t toa, int64_t tot) noexcept
                {
                        //----------------------------------------------------------------------
                        // parsing one data line
                        //----------------------------------------------------------------------
                        // double fulltoa = toa*25.0 - ftoa*25.0/16.0;
                        // double ftoaC=ftoa*1.0;

                        data[dataIndex].Total++;

                        // temporary here
                        if (tot < TOTMin)
                                TOTMin = tot;
                        if (tot > TOTMax)
                                TOTMax = tot;
                        // end of temporary

                        const u64 FullToA = detector.TOAMode ? toa : tot;

                        if (FullToA < detector.TRoiStart)
                                data[dataIndex].BeforeRoi++;
                        else if (FullToA >= detector.TRoiEnd) {
                                data[dataIndex].AfterRoi++;
                        } else {
                                const int TP = static_cast<int>((FullToA - detector.TRoiStart) / detector.TRoiStep);
                                // not ideal here. Does not work if tot step is
                                // not 1
                                if (detector.TOAMode == true) {
                                        Register(dataIndex, index, TP, tot);
                                } else {
                                        const int TOTP = tot;
                                        Register(dataIndex, index, TOTP, tot);
                                }
                        }
                } // end Analyse()

                void PurgePeriod(unsigned chipIndex, period_type period)
                {
                        logger << "PurgePeriod(" << chipIndex << ", " << period << ')' << log_trace;
                        logger << chipIndex << ": purge period " << period << log_info;
                        auto sp = save_point;
                        auto ac = active;
                        if (period >= sp) {
                                if ((save_ok[ac] += 1) == detector.NumChips()) {
                                        save_ok[ac].store(0);
                                        {
                                                std::lock_guard lock{histo_lock};
                                                save_point = sp + save_interval;
                                                active = ac ^ 1;
                                        }

                                        SaveToFile(ac, outFileName + std::to_string(sp) + ".tds");
                                }
                        }
                }

                void ProcessEvent(unsigned chipIndex, const period_type period, int64_t toaclk, uint64_t event)
                {
                        logger << "ProcessEvent(" << chipIndex << ", " << period << ", " << toaclk << ", " << std::hex << event << std::dec << ')' << log_trace;
                        const uint64_t totclk = Decode::getTotClock(event);
                        const float toa = Decode::clockToFloat(toaclk);
                        const float tot = Decode::clockToFloat(totclk, 40e6);
                        const std::pair<uint64_t, uint64_t> xy = Decode::calculateXY(event);
                        logger << chipIndex << ": event: " << period << " (" << xy.first << ' ' << xy.second << ") " << toa << ' ' << tot
                        << " (" << toaclk << ' ' << totclk << std::hex << event << std::dec << ')' << log_info;
                        auto index = PixelIndex::from(chipIndex, xy);
                        {
                                std::lock_guard lock{histo_lock};
                                Analyse((period > save_point ? active ^ 1 : active), index, toaclk, totclk);
                        }
                }

        }; // end type Analysis

        std::vector<Analysis> analysis;                 //!< space for as many Analysis objects as there are threads

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

                Detector K{layout};
                K.SetTimeROI(TRStart, TRStep, TRN);
                readAreaROI(K.energy_points, layout, "XESPoints.inp");

                analysis.emplace_back(Analysis{K, FileOutputPath + ShortFileName});
        }

        void purgePeriod(unsigned chipIndex, period_type period)
        {
                analysis[0].PurgePeriod(chipIndex, period);
        }

        void processEvent(unsigned chipIndex, const period_type period, int64_t toaclk, uint64_t event)
        {
                analysis[0].ProcessEvent(chipIndex, period, toaclk, event);
        }

} // namespace processing
