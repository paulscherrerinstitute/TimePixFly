#pragma once

#ifndef XES_DATA_MANAGER_H
#define XES_DATA_MANAGER_H

/*!
\file
Provide functionality to manage partial XES data per thread
*/

#include <mutex>
#include <condition_variable>
#include <limits>
#include <chrono>
#include <stdexcept>

#include "Poco/Exception.h"

#include "global.h"
#include "timing.h"
#include "xes_data_writer.h"

/*!
\brief XES data manager functionality
*/
namespace xes {
    using namespace std::chrono_literals;

    /*!
    \brief XES data manager
    */
    struct Manager final {
        /*!
        \brief "undefined" period
        */
        static constexpr period_type none = std::numeric_limits<period_type>::min();

        /*!
        \brief Per thread cache entry
        */
        struct alignas(256) CacheEntry final {
            period_type period = none;  //!< Period
            Data* data = nullptr;       //!< Pointer to per thread XES data
        };

        /*!
        \brief Cache indexed by thread id (=chip id)
        */
        std::vector<CacheEntry> dataCache;

        /*!
        \brief Per period XES data
        */
        struct Period final {
            std::atomic_uint ready;         //!< Ready for per thread XES data aggregation?
            std::atomic<period_type> period;//!< Period ("none" for undefined)
            std::vector<Data> threadData;   //!< Per thread XES data

            /*!
            \brief Constructor
            */
            Period()
                : ready{0}, period{none}
            {}

            /*!
            \brief Copy constructor
            \param other Other value
            */
            Period(const Period& other)
                : ready{other.ready.load()}, period{other.period.load()}
            {}

            /*!
            \brief Assignment
            \param other Other value
            \return this
            */
            Period& operator=(const Period& other)
            {
                ready.store(other.ready.load());
                period.store(other.period.load());
                return *this;
            }
        };

        /*!
        \brief Pool of period data
        This pool has to big enough to hold period data for periods that
        - receive data from analysis threads
        - are written to disk
        */
        std::vector<Period> periodData;

        std::vector<Period*> periodQueue;   //!< Ready period data queue pointing into data pool

        std::mutex thread_lock;             //!< Protect parallel data access
        std::condition_variable action_required; //!< Signal for data aggregate+write thread
        bool stopWriter = false;            //!< Stop data aggregate+write thread
        std::thread writerThread;           //!< Data aggregate+write thread
        std::unique_ptr<xes::Writer> writer; //!< Writer for file or tcp

        const Detector& detector;           //!< Detector reference
        Logger& logger;                     //!< Logger reference

        /*!
        \brief Constructor
        \param detector_ Detector data reference
        \param uri Output file:name (without period and .xes), or tcp:host:port
        \param nPeriods How many periods receive/emit data in parallel (see periodData member)
        */
        inline Manager(const Detector& detector_, const std::string& uri, unsigned nPeriods)
            : writer(xes::Writer::from_uri(uri)), detector(detector_), logger(Logger::get("Tpx3App"))
        {
            logger << "xes::Manager connecting to " << writer->dest() << log_info;
            const unsigned nThreads = detector.layout.chip.size();
            dataCache.resize(nThreads);
            periodData.resize(nPeriods, Period{});
            for (auto& pd : periodData) {
                pd.threadData.resize(nThreads);
                for (auto& d : pd.threadData)
                    d.Init(detector);
            }

            writerThread = std::thread([this]() {
                double t_wait = .0;
                double t_aggregate = .0;
                double t_write = .0;
                Timer clock;

                try {
                    writer->start(detector);

                    while (true) {
                        Period* period;
                        {
                            clock.set();
                            std::unique_lock lock(thread_lock);
                            while (true) {
                                if (stopWriter) {
                                    writer->stop(std::string(global::no_error));
                                    goto stop;
                                }
                                if (periodQueue.size() > 0)
                                    break;
                                action_required.wait(lock);
                            }
                            // periodQueue.size() > 0
                            period = periodQueue.back();
                            periodQueue.pop_back();
                            t_wait += clock.elapsed();
                            clock.set();
                        }

                        logger << "output: aggregate and write data for period " << period->period << log_debug;
                        Data* data = nullptr;
                        for (auto& d : period->threadData) {
                            if (data) {
                                *data += d;
                                d.Reset();
                            } else {
                                data = &d;
                            }
                        }
                        t_aggregate += clock.elapsed();
                        clock.set();

                        writer->write(*data, period->period);
                        //->SaveToFile(outFileName+"-"+std::to_string(period->period));
                        data->Reset();
                        period->ready.store(0);
                        period->period.store(none);

                        t_write += clock.elapsed();
                    } // while (true)
                } catch (std::exception& ex) {
                    writer->stop(std::string("writer: ") + ex.what());
                    logger << "writer thread exception: " << ex.what() << log_fatal;
                    global::set_error(std::string("writer: ") + ex.what());
                    stopWriter = true;
                    return;
                } catch (...) {
                    writer->stop("writer: unknown exception");
                    logger << "writer thread: unknown exception" << log_fatal;
                    global::set_error("writer: unknown exception");
                    stopWriter = true;
                    return;
                }
                std::exit((EXIT_FAILURE));  // unreachable

            stop:
                logger << "output wait: " << t_wait << "s, aggregate: " << t_aggregate << "s, write: " << t_write << 's' << log_notice;
            });
        }

        Manager() = delete;
        Manager(const Manager&) = delete;
        Manager& operator=(const Manager&) = delete;
        Manager(Manager&&) = delete;
        Manager& operator=(Manager&&) = delete;

        /*!
        \brief Destructor
        */
        ~Manager()
        {
            {
                std::unique_lock lock(thread_lock);
                stopWriter = true;
            }
            action_required.notify_all();
            writerThread.join();
        }

        /*!
        \brief Get XES data for period
        Retrieve per thread XES data for the purpose of filling in the histogram.
        \param threadNo Analysis thread number (=chip number)
        \param period   Period
        \return Reference to per thread XES period data
        */
        Data& DataForPeriod(unsigned threadNo, period_type period) noexcept
        {
            CacheEntry& cached = dataCache[threadNo];
            if (cached.period == period)
                return *cached.data;

            Period* firstNone;
            period_type expect;
            do {
                expect = none;
                while(true) {
                    firstNone = nullptr;
                    for (auto& pd : periodData) {
                        if (!firstNone && (pd.period == none)) {
                            firstNone = &pd;
                        } else if (pd.period == period) {
                            cached.period = period;
                            cached.data = &pd.threadData[threadNo];
                            return *cached.data;
                        }
                    }
                    if (firstNone)
                        break;
                    // xes::Manager too slow/unbalanced
                    std::this_thread::sleep_for(1ms);
                }
            } while (! firstNone->period.compare_exchange_weak(expect, period));
            cached.period = period;
            cached.data = &firstNone->threadData[threadNo];
            return *cached.data;
        }

        /*!
        \brief Return XES data for period
        Return per thread XES data for period that will not receive more events.
        This activates the aggregate+write thread for the period data when all
        analysis threads have returned their data.
        \param threadNo Thread number (=chip number)
        \param period   Period
        */
        void ReturnData(unsigned threadNo, period_type period)
        {
            if (stopWriter)
                throw Poco::RuntimeException(global::instance->last_error);

            dataCache[threadNo].period = none;
            Period* periodPtr = nullptr;
            for (auto& pd : periodData) {
                if (pd.period == period) {
                    periodPtr = &pd;
                    break;
                }
            }
            assert(periodPtr);
            if (++(periodPtr->ready) == periodPtr->threadData.size()) {
                {
                    std::unique_lock lock(thread_lock);
                    periodQueue.push_back(periodPtr);
                }
                action_required.notify_one();
            }
        }
    };

} // xes namespace

#endif
