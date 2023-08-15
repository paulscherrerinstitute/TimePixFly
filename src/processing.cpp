#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <vector>
#include <numeric>
#include <algorithm>
#include <cmath>

#include "logging.h"
#include "decoder.h"

// anonymous namespace to prevent symbol visibility
namespace {
        using period_type = int64_t;
        
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

        using u16 = uint16_t;
        using u64 = uint64_t;

        constexpr int DetSize = 256;    //!< Number of detector pixels per row/column

        Logger& logger = Logger::get("Tpx3App");

        /*!
        * \brief Per pixel energy aggregation data
        */
        struct Pixel final {
                /*!
                * \brief Per energy point data
                * For each energy point in the spectrum to which the pixel contributes
                */
                struct Data final {
                int EnergyPoint;        //!< Pixel contributes to this energy point
                float Weight;           //!< With this weight
                };
                std::vector<Data> parts;    //!< One part per energy point
        };

        /*!
        * \brief Constant detector data
        */
        struct Detector final {
                static constexpr int NumPixels = DetSize * DetSize;

                // if TOAMode is false then TOT is used for binnig (counts as a
                // function of energy and TOT as output)
                static constexpr bool TOAMode = true;

                static constexpr u16 TOTRoiStart = 1;
                static constexpr u16 TOTRoiEnd = 100;

                u64 TRoiStart = TOAMode ? 0 : TOTRoiStart;
                u64 TRoiStep = 1;
                u64 TRoiN = TOAMode ? 5000 : 100;
                u64 TRoiEnd = TRoiStart + TRoiStep * TRoiN;

                int NumEnergyPoints;
                std::vector<Pixel> AllPixels;

                vector<float> FlatField;        //<! Flat field indexed by [calib_pix * NumCalibFactors + calib_factor]
                vector<float> FlatKinetics;     //<! Flat kinetics indexed by [time_point * NumCalibFactors + calib_factor]
                int NumCalibPix;
                int NumCalibTPoints;
                int NumCalibFactors = 0;

                u64 ToFlat([[maybe_unused]] unsigned chipIndex, u64 x, u64 y) const noexcept
                {
                        return x * DetSize + y;
                }

                // In steps of 1.5625 ns
                void SetTimeROI(int tRoiStart, int tRoiStep, int tRoiN) noexcept
                {
                        TRoiStart = tRoiStart;
                        TRoiStep = tRoiStep;
                        TRoiN = tRoiN;

                        TRoiEnd = TRoiStart + TRoiStep * TRoiN;
                }

                void LoadCalibration(const string &NameFlatField = "FlatField.txt",
                                const string &NameFlatKinetics = "FlatKinetics.txt")
                {
                        std::ifstream FlatFieldFile(NameFlatField);
                        if (!FlatFieldFile)
                                assert((false && "LoadCalibration (open FlatFieldFile) failed"));
                        std::ifstream FlatKineticsFile(NameFlatKinetics);
                        if (!FlatKineticsFile)
                                assert((false && "LoadCalibration (open FlatKineticsFile) failed"));

                        char c;
                        int NumCalibFactors1, NumCalibFactors2;
                        if (!(FlatFieldFile >> c >> NumCalibPix >> NumCalibFactors1))
                                assert((false && "read from FlatFieldFile failed"));
                        if (!(FlatKineticsFile >> c >> NumCalibTPoints >> NumCalibFactors2))
                                assert((false && "read from FlatKineticsFile failed"));

                        if (NumCalibFactors1 != NumCalibFactors2) {
                                cout << "Problem with calibration files. Different "
                                        "number of factors in kinetics and flat field\n";
                                exit((EXIT_FAILURE));
                        }
                        assert((NumCalibFactors1 >= NumCalibFactors) && "NumCalibFactors cannot be bigger than effective number of calibration factors");

                        float dummy;
                        int i, j, k;

                        FlatKinetics.resize(NumCalibTPoints * NumCalibFactors);
                        for (i=0, k=0; i<NumCalibTPoints; ++i) {
                                for (j=0; j<NumCalibFactors; ++j, ++k)
                                        FlatKineticsFile >> FlatKinetics[k];
                                for (; j<NumCalibFactors1; ++j)     // rest is not used
                                        FlatKineticsFile >> dummy;
                        }

                        FlatField.resize(NumCalibPix * NumCalibFactors);
                        for (i=0, k=0; i<NumCalibPix; ++i) {
                                for (j=0; j<NumCalibFactors; ++j, ++k)
                                        FlatFieldFile >> FlatField[k];
                                for (; j<NumCalibFactors1; ++j)     // rest is not used
                                        FlatFieldFile >> dummy;
                        }
                }

                /*!
                * \brief Calibrate pixel response
                * \param PixelIndex Flat index of the pixel
                * \param TimePoint Time point of pixel response
                * \return Calibrated pixel response
                */
                inline float Calibrate(int PixelIndex, int TimePoint) const noexcept
                {
                        if ((NumCalibFactors <= 0) || (TimePoint > NumCalibTPoints))
                                return 1.f;
                        const float* const ff = &FlatField[PixelIndex * NumCalibFactors];
                        const float* const fk = &FlatKinetics[TimePoint * NumCalibFactors];
                        const float Coeff = std::inner_product(ff, ff+NumCalibFactors, fk, .0f);
                        return Coeff == .0f ? 1.f : Coeff;
                }

                void SetROI(int ROIStart, int ROIEnd, bool VertOrient, int BinSize)
                {
                        NumEnergyPoints = DetSize / BinSize;
                        AllPixels.resize(NumPixels);
                        for (int i=0; i<NumPixels; ++i) {
                                Pixel& pixel = AllPixels[i];
                                const int Line = i / DetSize;
                                const int Column = i - Line * DetSize;
                                if (VertOrient) {
                                        if ((Line >= ROIStart) && (Line < ROIEnd))
                                                pixel.parts.emplace_back(Pixel::Data{Column / BinSize, 1.f});
                                } else {
                                        if ((Column >= ROIStart) && (Column < ROIEnd))
                                                pixel.parts.emplace_back(Pixel::Data{Line / BinSize, 1.f});
                                }
                        }
                }

                void SetCircularROI(float CenterLine, float CenterColumn, float *RingSizes, int NumRings) {
                        NumEnergyPoints=NumRings;
                        AllPixels.resize(NumPixels);

                        //Here if the center of pixel is in the ring then the whole pixel is assigned to this ring
                        //In the furure I have to implement that one pixel is partially assigned to a few rings
                        for (int i = 0; i < NumPixels; i++) {
                                Pixel& pixel = AllPixels[i];
                                int Line = i / DetSize;
                                int Column = i - Line * DetSize;
                                //check math
                                float R=sqrt(pow((Line-CenterLine),2)+pow((Column-CenterColumn),2));
                                float PreviousSize=0.0;
                                int EnergyPointIndex=-1;
                                for (int j=0; (j<NumRings)&&(EnergyPointIndex==-1); j++) {
                                        if ((R>=PreviousSize)&&(R<RingSizes[j])) EnergyPointIndex=j;
                                        PreviousSize=RingSizes[j];
                                }
                                //If some pixels are outside of the largest ring they will be assigned to the largest ring
                                //so the last point of the spectrum can be weird...
                                //Largest ring can be also as large as the detector (distance to the corner)
                                //in this case it will be zero pixels outside of the ring
                                if (EnergyPointIndex==-1)
                                EnergyPointIndex=NumEnergyPoints-1;

                                //Weight=1.0 is completely wrong. Should take into account the trigonometry (geometry of the spectrometer),
                                //the number of pixels in the ring etc
                                pixel.parts.emplace_back(Pixel::Data{EnergyPointIndex, 1.f});
                        }
                }
        };  // end type Detector

        /*!
        * \brief Analysis data and operations
        */
        struct Analysis final {
                const Detector& detector;           //!< Reference to constant Detector data

                vector<float> TDSpectra;            //!< Result spectra indexed by [time_point * NumEnergyPoints + energy_point]

                int BeforeRoi;                      //!< Number of events before roi
                int AfterRoi;                       //!< Number of events after roi
                int Total;                          //!< Total events handled
                u16 TOTMin;                         //!< Minimal energy encountered in handled events
                u16 TOTMax;                         //!< Maximum energy encountered in handled events

                duration<double> processor_wait_time; //!< Waiting time for ready data buffers

                /*!
                * \brief Constructor
                * \param det Constant detector data
                */
                inline Analysis(const Detector& det)
                : detector(det),
                  TDSpectra(det.TRoiN * det.NumEnergyPoints)
                {}

                /*!
                * \brief Save spectra to .xes file
                * The extension .xes will be appended to the output file path.
                * \param OutFileName Path to output file without .xes extension
                */
                inline void SaveToFile(const string& OutFileName) const
                {
                        const clock::time_point t01 = clock::now();
                        const string OutFileName1 = OutFileName + ".xes";
                        std::ofstream OutFile(OutFileName1);

                        const int NumEnergyPoints = detector.NumEnergyPoints;
                        for (int i=0; i<NumEnergyPoints; ++i) {
                                for (u64 j=0; j<detector.TRoiN; ++j) {
                                        OutFile << TDSpectra[j * NumEnergyPoints + i] << " ";
                                }
                                OutFile << "\n";
                        }
                        const clock::time_point t02 = clock::now();
                        auto duration1 = duration_cast<milliseconds>(t02 - t01).count();
                        cout << "conversion time " << duration1 << " ms ";
                        OutFile.close();
                        if (OutFile.fail())
                                assert((false && "Detector::SaveToFile failed"));
                }

                /*!
                * \brief Initialize analysis data
                */
                inline void Refresh() noexcept
                {
                        BeforeRoi = 0;
                        AfterRoi = 0;
                        Total = 0;
                        std::fill(TDSpectra.begin(), TDSpectra.end(), .0f);
                }

                /*!
                * \brief Register calibrated event in energy spectrum
                * \param PixelIndex Flat pixel index
                * \param TOT Pixel energy
                */
                inline void Register(int PixelIndex, int TimePoint, u16 TOT) noexcept
                {
                        if ((TOT > detector.TOTRoiStart) && (TOT < detector.TOTRoiEnd)) {
                                const Pixel& CurrentPixel = detector.AllPixels[PixelIndex];
                                if (! CurrentPixel.parts.empty()) {
                                        const float clb = detector.Calibrate(PixelIndex, TimePoint);
                                        for (const auto& part : CurrentPixel.parts)
                                                TDSpectra[TimePoint * detector.NumEnergyPoints + part.EnergyPoint] += part.Weight / clb;
                                }
                        }
                }

                /*!
                * \brief Analyse a single event
                * \param event Event
                */
                inline void Analyse(uint64_t index, int64_t toa, int64_t tot) noexcept
                {
                        //----------------------------------------------------------------------
                        // parsing one data line
                        //----------------------------------------------------------------------
                        // double fulltoa = toa*25.0 - ftoa*25.0/16.0;
                        // double ftoaC=ftoa*1.0;

                        Total++;

                        // temporary here
                        if (tot < TOTMin)
                                TOTMin = tot;
                        if (tot > TOTMax)
                                TOTMax = tot;
                        // end of temporary

                        // WrONG SIGN FOR THE TEST Should be:  16*toa-ftoa
                        const u64 FullToA = detector.TOAMode ? toa : tot;

                        if (FullToA < detector.TRoiStart)
                                BeforeRoi++;
                        else if (FullToA >= detector.TRoiEnd) {
                                AfterRoi++;
                        } else {
                                const int TP = static_cast<int>((FullToA - detector.TRoiStart) / detector.TRoiStep);
                                // not ideal here. Does not work if tot step is
                                // not 1
                                if (detector.TOAMode == true) {
                                        Register(index, TP, tot);
                                } else {
                                        const int TOTP = tot;
                                        Register(index, TOTP, tot);
                                }
                        }
                } // end Analyse()

                void PurgePeriod(unsigned chipIndex, period_type period)
                {
                        logger << "purgePeriod(" << chipIndex << ", " << period << ')' << log_trace;
                        logger << chipIndex << ": purge period " << period << log_info;
                }

                void ProcessEvent(unsigned chipIndex, const period_type period, int64_t toaclk, uint64_t event)
                {
                        logger << "processEvent(" << chipIndex << ", " << period << ", " << toaclk << ", " << std::hex << event << std::dec << ')' << log_trace;
                        const uint64_t totclk = Decode::getTotClock(event);
                        const float toa = Decode::clockToFloat(toaclk);
                        const float tot = Decode::clockToFloat(totclk, 40e6);
                        const std::pair<uint64_t, uint64_t> xy = Decode::calculateXY(event);
                        logger << chipIndex << ": event: " << period << " (" << xy.first << ' ' << xy.second << ") " << toa << ' ' << tot
                        << " (" << toaclk << ' ' << totclk << std::hex << event << std::dec << ')' << log_info;
                        // auto index = detector.ToFlat(chipIndex, xy.first, xy.second);
                        // Analyse(index, toaclk, totclk);
                }

        }; // end type Analysis

        std::vector<Analysis> analysis;                 //!< space for as many Analysis objects as there are threads

} // anonymous namespace

namespace processing {

        void init()
        {
                // using std::getline;
                // using std::ws;

                // std::ifstream ProcessingFile("Processing.inp");

                // string Comment;
                // string FileInputPath, FileOutputPath, ShortFileName;

                // getline(ProcessingFile, Comment);
                // getline(ProcessingFile, FileInputPath);
                // getline(ProcessingFile, Comment);
                // getline(ProcessingFile, FileOutputPath);
                // getline(ProcessingFile, Comment);
                // getline(ProcessingFile, ShortFileName);

                // int NumCalibFactors, FileIndexBegin, FileIndexEnd, FileIndexStep;

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
                // int TRStart, TRStep, TRN;
                // bool DeleteAfter;

                // getline(ProcessingFile, Comment);
                // ProcessingFile >> DetROIStart >> ws;
                // getline(ProcessingFile, Comment);
                // ProcessingFile >> DetROIEnd >> ws;
                // getline(ProcessingFile, Comment);
                // ProcessingFile >> BinVerticalOrientation >> ws;
                // getline(ProcessingFile, Comment);
                // ProcessingFile >> BinSize >> ws;
                // getline(ProcessingFile, Comment);
                // ProcessingFile >> TRStart >> ws;
                // getline(ProcessingFile, Comment);
                // ProcessingFile >> TRStep >> ws;
                // getline(ProcessingFile, Comment);
                // ProcessingFile >> TRN >> ws;
                // getline(ProcessingFile, Comment);
                // ProcessingFile >> DeleteAfter >> ws;

                // ProcessingFile.close();
                // if (ProcessingFile.fail())
                //         assert((false && "failed to parse ProcessingFile"));

                Detector K;
                // K.SetTimeROI(TRStart, TRStep, TRN);
                // K.SetROI(DetROIStart, DetROIEnd, BinVerticalOrientation, BinSize);

                // K.NumCalibFactors = NumCalibFactors;
                // if (NumCalibFactors > 0) {
                //         const string NameFlatField = FileInputPath + "FlatField.txt";
                //         const string NameFlatKinetics = FileInputPath + "FlatKinetics.txt";
                //         K.LoadCalibration(NameFlatField, NameFlatKinetics);
                // }

                analysis.emplace_back(Analysis(K));
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
