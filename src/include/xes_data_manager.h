#pragma once

#ifndef XES_DATA_MANAGER_H
#define XES_DATA_MANAGER_H

/*!
\file
Provide functionality to manage partial XES data per thread
*/

#include <cstddef>
#include <ostream>
#include <condition_variable>
#include <limits>
#include <queue>
#include <forward_list>

#include "Poco/Exception.h"

#include "logging.h"
#include "global.h"
#include "timing.h"
#include "xes_data_writer.h"
#include "thread_naming.h"

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
        \brief Per module Data
        */
        struct ModuleData final {
            std::condition_variable write;      //!< Signal that data is ready for writing
            std::mutex lock_ready;              //!< Protect ready
            std::mutex lock_empty;              //!< Protect empty
            std::priority_queue<Data*> ready;   //!< Histograms ready for writer thread
            std::vector<Data*> empty;           //!< Empty, initialized histograms
            std::vector<Data*> fill;            //!< Histograms beeing filled up
            std::forward_list<Data> pool;       //!< Pool of histograms, pointed to by ready and empty
            CacheEntry cache;                   //!< Last used histogram
            bool final=false;                   //!< No more data

            inline ModuleData() = default;      //!< Constructor

            /*!
            \brief Copy constructor
            This is NOT a proper copy constructor! Locks, condition variable, and cache are default initialized.
            \param other Copy from
            */
            inline ModuleData(const ModuleData& other)              //!< Copy constructor
              : write{}, lock_ready{}, lock_empty{},
                ready{other.ready}, empty{other.empty}, fill{other.fill},
                pool{other.pool}, cache{}, final{other.final}
              {}
        };

        std::vector<ModuleData> module_data;    //!< Per module data
        std::atomic_uint histo_submitted = 0u;  //!< Number of submitted histograms

        std::atomic_bool stopWriter = false;    //!< Stop data aggregate+write thread
        std::thread writerThread;               //!< Data aggregate+write thread
        std::unique_ptr<xes::Writer> writer;    //!< Writer for file or tcp

        const Detector& detector;               //!< Detector reference
        Logger& logger;                         //!< Logger reference

        /*!
        \brief Constructor
        \param detector_ Detector data reference
        \param uri Output file:name (without period and .xes), or tcp:host:port
        */
        inline Manager(const Detector& detector_, const std::string& uri)
            : writer(xes::Writer::from_uri(uri)), detector(detector_), logger(Logger::get("Tpx3App"))
        {
            logger << "xes::Manager connecting to <" << writer->dest() << ">, output uri <" << uri << ">" << log_info;
            const unsigned nThreads = detector.layout.chip.size();
            module_data.resize(nThreads);

            writerThread = std::thread([this, nThreads]() {
                double t_wait = .0;
                double t_aggregate = .0;
                double t_write = .0;
                u64 n_toa = 0ul;
                u64 n_before = 0ul;
                u64 n_after = 0ul;
                // float t_energy = .0f;   // DEBUG ENERGY
                unsigned n_histo = 0u;
                unsigned cyclic_start = 0u; // cycle through mdata to distribute burden evenly amongst threads

                set_thread_name("tpx3app:writer");

                try {
                    {   // set thread affinity
                        int writer_cpu = global::instance->cpu_affinity.writer_cpu;
                        if (writer_cpu >= 0) {
                            int rval = cpu_mask::set_affinity(cpu_mask::get_tid(), writer_cpu);
                            if (rval != 0)
                                logger << "writer: set affinity - " << cpu_mask::error(rval) << log_error;
                        }
                    }

                    writer->start(detector);

                    while (true) {
                        Timer t1{};
                        Data* data{nullptr};
                        ModuleData* md{nullptr};
                        cyclic_start = (cyclic_start + 1) % nThreads;

                        for (unsigned i=0; i<nThreads; i++) {
                            Data* d{nullptr};
                            auto& mdata = module_data[(cyclic_start + i) % nThreads];

                            Timer t2{};
                            {
                                std::unique_lock lock{mdata.lock_ready};
                                while (!stopWriter) {
                                    if (! mdata.ready.empty()) {
                                        d = mdata.ready.top();
                                        mdata.ready.pop();
                                        break;
                                    }
                                    if (mdata.final)
                                        break;
                                    mdata.write.wait_for(lock, 1s);
                                }
                                if (stopWriter)
                                    goto regular_stop;
                            } // d!=nullptr OR f
                            t_wait += t2.elapsed_reset();

                            if (d != nullptr) {
                                if (data != nullptr) {
                                    if (d->period > data->period)
                                        data->period = d->period;
                                    assert(data->period != 0);
                                    data->addResetRhs(*d);
                                    t_aggregate += t2.elapsed_reset();
                                    {
                                        std::lock_guard lock{mdata.lock_empty};
                                        mdata.empty.push_back(d);
                                    }
                                    t_wait += t2.elapsed();
                                } else {
                                    data = d;
                                    md = &mdata;
                                }
                            }
                        } // for all threads

                        if (data == nullptr) {
                            t_write += t1.elapsed();
                            goto regular_stop;
                        }

                        // logger << "writer: period " << data->period << log_debug;
                        writer->write(*data);
                        n_toa += data->Total;
                        n_before += data->BeforeRoi;
                        n_after += data->AfterRoi;
                        // t_energy += data->Energy;   // DEBUG ENERGY
                        n_histo++;
                        data->Reset();
                        {
                            std::lock_guard lock{md->lock_empty};
                            md->empty.push_back(data);
                        }

                        t_write += t1.elapsed();
                    } // while (true)
                } catch (std::exception& ex) {
                    try {
                        writer->stop(std::string("writer: ") + ex.what());
                    } catch (...) {}    // ignore exceptions
                    logger << "writer thread exception: " << ex.what() << log_fatal;
                    global::set_error(std::string("writer: ") + ex.what());
                } catch (...) {
                    try {
                        writer->stop("writer: unknown exception");
                    } catch (...) {}    // ignore exceptions
                    logger << "writer thread: unknown exception" << log_fatal;
                    global::set_error("writer: unknown exception");
                }

                // exception stop
                stopWriter = true;
                return;

            regular_stop:
                if (writer->data_counter == 0u)
                    global::set_error("no event data was collected");
                logger << "output histos: " << n_histo << " of " << ((double)histo_submitted / nThreads)
                       << " wait: " << t_wait << "s agg: " << t_aggregate << "s write: " << t_write << "s toas: " << n_toa
                       << " (after: " << n_after << " before: " << n_before << ") at " << (n_toa / t_write) << " toas/s" << log_notice;
                // logger << "output energy: " << t_energy << log_debug;   // DEBUG ENERGY
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
            for (auto& mdata: module_data) {
                std::lock_guard lock{mdata.lock_ready};
                mdata.final = true;
                mdata.write.notify_all();
            }
            writerThread.join();
        }

        /*!
        \brief Get XES data for period
        Retrieve per thread XES data for the purpose of filling in the histogram.
        \param threadNo Analysis thread number (=chip number)
        \param period   Period
        \return Reference to per thread XES period data
        */
        Data& DataForPeriod(unsigned threadNo, period_type period)
        {
            auto& mdata = module_data[threadNo];

            // try cache
            CacheEntry& cached = mdata.cache;
            if (__builtin_expect(cached.period == period, true))
                return *cached.data;

            if (__builtin_expect(stopWriter, 0))
                throw Poco::RuntimeException(global::instance->last_error);

            Data* data = nullptr;

            // try the histogram data beeing filled up
            for (auto& d: mdata.fill) {
                if (d->period == period) {
                    data = d;
                    goto cache_return;
                }
            }

            // try grabbing empty histogram data
            {
                std::lock_guard lock_empty{mdata.lock_empty};
                if (! mdata.empty.empty()) {
                    data = mdata.empty.back();
                    mdata.empty.pop_back();
                }
            }
            if (data != nullptr)
                goto fill_cache_return;

            // create a new histogram
            // NOTE: this operation MUST NOT change memory location of other data
            mdata.pool.emplace_front(detector);
            data = &mdata.pool.front();

        fill_cache_return:
            data->period = period;
            mdata.fill.push_back(data);

        cache_return:
            cached.period = period;
            cached.data = data;
            return *cached.data;
        }

        /*!
        \brief Return XES data for period
        Return per thread XES data for period that will not receive more events.
        This activates the aggregate+write thread for the period data when all
        analysis threads have returned their data.
        \param threadNo Thread number (=chip number)
        \param period   Period
        \param final    Final forced write at end of measurement
        */
        void ReturnData(unsigned threadNo, period_type period, bool final=false)
        {
            assert(period != 0);
            if (__builtin_expect(stopWriter, false))
                throw Poco::RuntimeException(global::instance->last_error);

            auto& mdata = module_data[threadNo];
            Data* data{nullptr};

            // clean cache
            CacheEntry& cached = mdata.cache;
            if (cached.period == period)
                cached.period = none;

            // find the histogram data beeing filled up
            for (auto& d: mdata.fill) { // use auto& here to get address of vector content below!
                if (d->period >= period) {
                    data = d;
                    mdata.fill.erase(static_cast<decltype(mdata.fill)::const_iterator>(&d));
                    break;
                }
            }

            // create empty data if none found
            if (__builtin_expect(data == nullptr, false)) {
                {
                    std::lock_guard lock_empty{mdata.lock_empty};
                    if (! mdata.empty.empty()) {
                        data = mdata.empty.back();
                        mdata.empty.pop_back();
                    }
                }

                if (data == nullptr) {
                    // create a new histogram
                    // NOTE: this operation MUST NOT change memory location of other data
                    mdata.pool.emplace_front(detector);
                    data = &mdata.pool.front();
                }
                // throw Poco::RuntimeException(std::string{"returned data not found for period "} + std::to_string(period));
            }

            // add to ready queue
            histo_submitted++;
            data->period = period;
            {
                std::lock_guard lock{mdata.lock_ready};
                mdata.ready.push(data);
                mdata.final = final;
                mdata.write.notify_one();
            }
        }
    };

} // xes namespace

#endif
