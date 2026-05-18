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

#include "global.h"
#include "timing.h"
#include "xes_data_writer.h"
#include "thread_naming.h"
#include "thread_signal.h"

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
            inline ModuleData(const ModuleData& other) noexcept
              : write{}, lock_ready{}, lock_empty{},
                ready{other.ready}, empty{other.empty}, fill{other.fill},
                pool{other.pool}, cache{}, final{other.final}
              {}
            
            /*!
            \brief Reset the module data to a well defined start state
            */
            inline void reset() noexcept
            {
                const auto& troi = global::instance->time_roi;
                // assume locks are unlocked
                while (!ready.empty())
                    ready.pop();
                empty.clear();
                fill.clear();
                cache = {none, nullptr};
                final = false;
                for (auto& data : pool) {
                    data.Init(troi);
                    data.Reset();
                    empty.push_back(&data);
                }
            }
        };

        std::vector<ModuleData> module_data;    //!< Per module data
        std::atomic_uint histo_submitted = 0u;  //!< Number of submitted histograms

        std::atomic_bool stopWriter = false;    //!< Stop data aggregate+write thread
        std::thread writerThread;               //!< Data aggregate+write thread
        std::unique_ptr<xes::Writer> writer;    //!< Writer for file or tcp

        thread_signal::single<thread_signal::no_shutdown> writer_shutdown;  //!< Shutdown now signal
        thread_signal::single<thread_signal::no_shutdown> writer_finished;  //!< Reader finished sigal
        thread_signal::single<thread_signal::with_shutdown> start_writer{writer_shutdown};  //!< Writer start signal

        const TimeRoi time_roi;                 //!< Time ROI
        Logger& logger;                         //!< Logger reference

        /*!
        \brief Constructor
        \param time_roi_ Detector data reference
        \param uri Output file:name (without period and .xes), or tcp:host:port
        */
        inline Manager()
            : logger(Logger::get("Tpx3App"))
        {
            const auto& gvars = *global::instance;
            const unsigned nThreads = gvars.layout.chip.size();
            module_data.resize(nThreads);

            writerThread = std::thread([this, nThreads]() {
                set_thread_name("tpx3app:writer");

                {   // set thread affinity
                    int writer_cpu = global::instance->cpu_affinity.writer_cpu;
                    if (writer_cpu >= 0) {
                        int rval = cpu_mask::set_affinity(cpu_mask::get_tid(), writer_cpu);
                        if (rval != 0)
                            logger << "writer: set affinity - " << cpu_mask::error(rval) << log_error;
                    }
                }

                logger << "writer start" << log_debug;

                do {
                    if (start_writer.wait_reset())
                        break;

                    double t_wait = .0;
                    double t_aggregate = .0;
                    double t_write = .0;
                    u64 n_toa = 0ul;
                    u64 n_before = 0ul;
                    u64 n_after = 0ul;
                    // float t_energy = .0f;   // DEBUG ENERGY
                    unsigned n_histo = 0u;
                    unsigned cyclic_start = 0u; // cycle through mdata to distribute burden evenly amongst threads

                    try {

                        writer->start(time_roi);

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
                                    while (__builtin_expect(!stopWriter, true)) {
                                        if (! mdata.ready.empty()) {
                                            d = mdata.ready.top();
                                            mdata.ready.pop();
                                            break;
                                        }
                                        if (mdata.final)
                                            break;
                                        mdata.write.wait_for(lock, 1s);
                                    }
                                    if (__builtin_expect(stopWriter, false)) {
                                        writer->stop("writer: external stop");
                                        goto regular_stop;
                                    }
                                } // d!=nullptr OR final
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

                            if (__builtin_expect(data == nullptr, false)) {
                                t_write += t1.elapsed();
                                writer->stop("");
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
                    writer.reset(nullptr);
                    writer_finished.send();
                    continue;

                regular_stop:
                    if (writer->data_counter == 0u)
                        global::set_error("no event data was collected");
                    writer.reset(nullptr);
                    writer_finished.send();
                    logger << "output histos: " << n_histo << " of " << ((double)histo_submitted / nThreads)
                        << " wait: " << t_wait << "s agg: " << t_aggregate << "s write: " << t_write << "s toas: " << n_toa
                        << " (after: " << n_after << " before: " << n_before << ") at " << (n_toa / t_write) << " toas/s" << log_notice;
                    // logger << "output energy: " << t_energy << log_debug;   // DEBUG ENERGY
                } while(true);

                logger << "writer shutdown" << log_debug;
            });
        }

        Manager(const Manager&) = delete;
        Manager& operator=(const Manager&) = delete;
        Manager(Manager&&) = delete;
        Manager& operator=(Manager&&) = delete;

        /*!
        \brief Destructor
        */
        inline ~Manager()
        {
            writer_shutdown.send();
            if (writerThread.joinable())
                writerThread.join();
        }

        /*!
        \brief Get XES data for period
        Retrieve per thread XES data for the purpose of filling in the histogram.
        \param threadNo Analysis thread number (=chip number)
        \param period   Period
        \return Reference to per thread XES period data
        */
        inline Data& DataForPeriod(unsigned threadNo, period_type period)
        {
            auto& mdata = module_data[threadNo];

            // try cache
            CacheEntry& cached = mdata.cache;
            if (cached.period == period)
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
            mdata.pool.emplace_front(time_roi);
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
        inline void ReturnData(unsigned threadNo, period_type period, bool final=false)
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
            for (auto& d: mdata.fill) {
                if (d->period >= period) {
                    data = d;
                    mdata.fill.erase(static_cast<decltype(mdata.fill)::const_iterator>(&d));
                    break;
                }
            }

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
                    mdata.pool.emplace_front(time_roi);
                    data = &mdata.pool.front();
                }
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

        /*!
        \brief Reset XES Manager to well defined state for start
        */
        inline void Reset()
        {
            const auto& gvars = *global::instance;
            {
                const std::string& uri = gvars.output_uri;
                writer = xes::Writer::from_uri(uri);
                logger << "xes::Manager connected to <" << writer->dest() << ">, output uri <" << uri << ">" << log_info;
            }
            const_cast<TimeRoi&>(time_roi) = gvars.time_roi;

            for (auto& mdata : module_data)
                mdata.reset();

            histo_submitted = 0u;
            stopWriter = false;
        }

        void shutdown()
        {
            writer_shutdown.send();
            writerThread.join();
        }

        void run_async()
        {
            start_writer.send();
        }

        void await()
        {
            for (auto& mdata: module_data) {
                std::lock_guard lock{mdata.lock_ready};
                mdata.final = true;
                mdata.write.notify_all();
            }
            writer_finished.wait_reset();
        }
    };

} // xes namespace

#endif
