#pragma once

#ifndef DATA_HANDLER_H
#define DATA_HANDLER_H  //!< Include flag

/*!
\file
Code for processing raw data stream
*/

#if defined(__AVX2__) && defined(AVX_DECODE)
    #define USE_AVX2_DECODE
#endif

#if defined(USE_AVX2_DECODE)
    #include "avx2_decoder.h"
#endif

#include "Poco/Net/StreamSocket.h"

#include "subreservation.h"
#include "logging.h"
#include "processing.h"
#include "thread_naming.h"
#include "timing.h"
#include "event_type.h"
#include "thread_signal.h"
#include "analysis.h"

namespace {
    using Poco::Net::StreamSocket;

    /*!
     \brief Heap ordering
     Make smaller the higher priority
     \param a First operand
     \param b Second operand
     \return True if and only if a > b
    */
    [[gnu::const]]
    inline bool operator<(const event_t& a, const event_t& b) noexcept
    {
        return a.ts > b.ts;
    }

    #if defined(USE_AVX2_DECODE)
        /*!
        \brief Iterator over events
        */
        class event_iterator final {
            static constexpr unsigned mask = ~0x3u;         //!< For alignment to 4
            const __m256i* data;                            //!< Raw event vector data
            alignas(sizeof(__m256i)) event_t current[4];    //!< Current event_t vector
            unsigned cur;                                   //!< Next position in current vector
            unsigned pos;                                   //!< Raw event position in data
            unsigned end;                                   //!< One past last raw event in data

        public:
            /*!
            \brief Constructor
            Set state and load current vector if cur is not zero
            \param reservation Read reservation
            \param subreservation Read subreservation
            */
            inline event_iterator(const iobuf::subreservation_t& subreservation) noexcept
            {
                assert(subreservation.content);
                data = (__m256i*)subreservation.content;
                pos = subreservation.pos;
                end = pos + subreservation.consume;
                unsigned start = pos & mask;
                cur = pos - start;
                pos = start;
                assert(pos + cur <= end);

                if (cur != 0)
                    _mm256_store_si256((__m256i*)current, avx2::decode(_mm256_load_si256(&data[pos >> 2])));
            }

            /*!
            \brief Get next event, if possible
            \param event Set this to the next event, if possible
            \return It was possible
            */
            inline bool next(event_t& event) noexcept
            {
                // Load if cur == 0
                assert(cur < 4);

                while (pos + cur < end) {

                    if (cur == 0)
                        _mm256_store_si256((__m256i*)current, avx2::decode(_mm256_load_si256(&data[pos >> 2])));

                    event = current[cur];

                    if (++cur == 4) {
                        pos += 4;
                        cur = 0;
                    }

                    if (event_t::valid(event))
                        return true;
                }

                return false;
            }
        }; // event_iterator
    #else // not defined(USE_AVX2_DECODE)
        /*!
        \brief Iterator over events
        */
        class event_iterator final {
            const AsiRawStreamDecoder::Event* data; //!< Raw event vector data
            unsigned pos;                           //!< Raw event position in data
            unsigned end;                           //!< One past last raw event in data

        public:
            /*!
            \brief Constructor
            Set state and load current vector if cur is not zero
            \param subreservation Read subreservation
            */
            inline event_iterator(const iobuf::subreservation_t& subreservation) noexcept
            {
                assert(subreservation.content);
                pos = subreservation.pos;
                end = pos + subreservation.consume;
                data = (AsiRawStreamDecoder::Event*)subreservation.content;
            }

            /*!
            \brief Get next event, if possible
            \param event Set this to the next event, if possible
            \return It was possible
            */
            inline bool next(event_t& event) noexcept
            {
                while (pos < end) {
                    const auto& ev = data[pos++];
                    if (ev.type.id == 0xb) {
                        event = { AsiRawStreamDecoder::getToaClock(ev.toa), 0ull, AsiRawStreamDecoder::flatPixel(ev.toa) };
                        return true;
                    } else if (ev.type.id == 0x6) {
                        event = { AsiRawStreamDecoder::getTdcClock(ev.tdc), 1ull, 0ull };
                        return true;
                    }
                }

                return false;
            }
        }; // event_iterator
    #endif // not defined(USE_AVX2_DECODE)

} // namespace

/*!
\brief Handler object for processing a raw data stream
*/
template<typename Decode>
class DataHandler final {
    #if SERVER_VERSION >= 320
        uint64_t DATA_OFFSET = 8;               //!< Start offset of event data within raw event data packet
    #else
        uint64_t DATA_OFFSET = 0;               //!< Start offset of event data within raw event data packet
    #endif

    StreamSocket dataStream;                    //!< Raw event data stream receiving end
    Logger& logger;                             //!< Poco::Logger object for logging
    iobuf::collection_t databuf;                //!< IO data buffer pool
    const size_t reorderSize;                   //!< Size of reorder queue (heap)
    std::thread readerThread;                   //!< Raw event data stream reader thread
    std::vector<std::thread> analyserThreads;   //!< Per chip event analyzer threads
    Analysis& analysis;                         //!< Analysis object
    // std::mutex coutMutex;                    //!< Output mutex for debugging
    std::mutex memberMutex;                     //!< Protection for member variables
    std::atomic<bool> stopOperation = false;    //!< Stop requested flag

    thread_signal::single<thread_signal::no_shutdown> all_shutdown;     //!< Shutdown now signal

    thread_signal::single<thread_signal::no_shutdown> reader_finished;  //!< Reader finished sigal
    thread_signal::single<thread_signal::with_shutdown> start_reader{all_shutdown}; //!< Reader start signal

    thread_signal::multi<thread_signal::send> analysis_finished;        //!< All analysis threads are ready signal
    thread_signal::multi<thread_signal::reset_with_shutdown> start_analysis; //!< Analysis start signal

    /*!
    \brief Check stop requested flag
    \return True if stop was requested
    */
    inline bool stop() const noexcept
    {
        return stopOperation.load(std::memory_order_consume);
    }

    /*!
    \brief Read from raw event data stream into buffer
    \param buf Byte buffer
    \param size Number of bytes to read
    \return Number of bytes effectively read, 0 for no more data
    */
    inline int readData(void* buf, int size)
    {
        // logger << "readData(" << buf << ", " << size << ')' << log_trace;
        int numBytes = 0;

        do {
            int numRead = 0;
            try {
                numRead = dataStream.receiveBytes(&static_cast<char*>(buf)[numBytes], size - numBytes);
                if (numRead == 0)
                    break;
            } catch (Poco::TimeoutException&) {
                if (global::instance->stop_collect) {
                    stopNow();
                    return 0;
                }
            }
            numBytes += numRead;
        } while (numBytes < size);

        return numBytes;
    }

    /*!
    \brief Code for raw event data reader thread
    */
    inline void readData()
    {
        set_thread_name("tpx3app:reader");

        {   // set thread affinity
            int reader_cpu = global::instance->cpu_affinity.reader_cpu;
            if (reader_cpu >= 0) {
                int rval = cpu_mask::set_affinity(cpu_mask::get_tid(), reader_cpu);
                if (rval != 0)
                    logger << "reader: set affinity - " << cpu_mask::error(rval) << log_error;
            }
        }

        logger << "reader start" << log_debug;

        do {
            if (start_reader.wait_reset())
                break;

            double spinTime = .0;
            double workTime = .0;
            u64 readBytes = 0ul;

            try {
                int bytesRead;
                Timer timer;
                iobuf::reservation_t reservation = databuf.write_reservation(iobuf::initial_reservation);

                while (reservation.end) {
                    assert(reservation.jar && reservation.jar->container.data && (reservation.start < reservation.end));
                    char* data = reservation.jar->container.data;
                    auto amount = reservation.end - reservation.start;

                    timer.set();

                    bytesRead = readData(&data[reservation.start], amount);
                    // bytesRead will be 0 here if there's no more data or stop_collect is true

                    workTime += timer.elapsed_reset();
                    readBytes += bytesRead;

                    // logger << "read " << bytesRead << " bytes of " << amount << log_debug;

                    reservation.end = reservation.start + bytesRead;
                    reservation = databuf.write_reservation(reservation);

                    spinTime += timer.elapsed();
                }

            } catch (Poco::Exception& ex) {
                stopNow();
                logger << "reader exception: " << ex.displayText() << log_critical;
                global::set_error(std::string{"reader: "} + ex.displayText());
            } catch (std::exception& ex) {
                stopNow();
                logger << "reader exception: " << ex.what() << log_critical;
                global::set_error(std::string{"reader: "} + ex.what());
            }

            // reader stopped
            {
                std::lock_guard lock{memberMutex};
                readTime += workTime;
                readSpinTime += spinTime;
                readTotalTime += (workTime + spinTime);
                byteCount += readBytes;
            }

            reader_finished.send();
            logger << "reader stopped" << log_debug;
        } while (true);

        logger << "reader shutdown" << log_debug;
    }

    /*!
    \brief Code for analyzer thread
    \param threadId Thread number, must correspond to chip number
    */
    inline void analyseData(unsigned threadId)
    {
        set_thread_name("tpx3app:analyze");

        {   // set thread affinity}
            int analysis_cpu = global::instance->cpu_affinity.get_cpu(threadId);
            if (analysis_cpu >= 0) {
                int rval = cpu_mask::set_affinity(cpu_mask::get_tid(), analysis_cpu);
                if (rval != 0)
                    logger << threadId << ": set affinity - " << cpu_mask::error(rval) << log_error;
            }
        }

        logger << threadId << ": analysis start" << log_debug;

        const unsigned chipIndex = threadId;

        // Reorder buffer
        std::vector<event_t> heap(reorderSize);

        do {
            if (start_analysis.wait_reset(threadId))
                break;

            uint64_t tdcHits = 0ul;
            uint64_t toaHits = 0ul;
            double spinTime = .0;
            double workPassOneTime = .0;
            double workPassTwoTime = .0;
            double workPassThreeTime = .0;

            try {

                // Reorder buffer
                size_t heap_sz = 0ul;

                // Last seen TDC event timestamp
                uint64_t tdc_ts = 0u;

                // Period counter
                period_type period = 0u;

                Timer timer;

                iobuf::subreservation_t subreservation{databuf, chipIndex};
                subreservation.update();

                spinTime += timer.elapsed_reset();

                while (subreservation.rest) {
                    // logger << threadId << ": subreservation p" << subreservation.pos << " r" << subreservation.rest << " c" << subreservation.consume << log_debug;
                    assert(subreservation.consume > 0);
                    event_iterator events{subreservation};
                    event_t ev;

                    // First stage: extract TDC/TOA events and fill up heap
                    // This stage is only active for the very first buffer(s)
                    while(heap_sz < reorderSize) {
                        if (! events.next(ev)) {
                            goto no_more_events;
                            workPassOneTime += timer.elapsed_reset();
                        }
                        heap[heap_sz++] = ev;
                        std::push_heap(heap.data(), heap.data()+heap_sz);
                    }

                    while (tdc_ts == 0ul) {
                        std::pop_heap(heap.data(), heap.data()+heap_sz);
                        const auto& el = heap[--heap_sz];
                        if (el.is_tdc) {
                            tdc_ts = el.ts;
                            tdcHits++;
                        } else {
                            toaHits++;
                        }
                        if (! events.next(ev)) {
                            workPassOneTime += timer.elapsed_reset();
                            goto no_more_events;
                        }
                        heap[heap_sz++] = ev;
                        std::push_heap(heap.data(), heap.data()+heap_sz);
                    }

                    workPassOneTime += timer.elapsed_reset();

                    // Third stage: handle earlier events while extracting and buffering later TDC/TOA events
                    // This stage is the work horse
                    do {
                        std::pop_heap(heap.data(), heap.data()+heap_sz);
                        const auto& el = heap[--heap_sz];
                        if (el.is_tdc) {
                            tdcHits++;
                            analysis.PurgePeriod(chipIndex, period);
                            tdc_ts = el.ts;
                            period++;
                        } else {
                            toaHits++;
                            analysis.ProcessEvent(chipIndex, period, { el.ts - tdc_ts, el.px });
                        }
                        if (! events.next(ev))
                            break;
                        heap[heap_sz++] = ev;
                        std::push_heap(heap.data(), heap.data()+heap_sz);
                    } while (true);

                    workPassTwoTime += timer.elapsed_reset();
                    // no more TOA events

                no_more_events:
                    subreservation.update();
                    spinTime += timer.elapsed_reset();
                } // while reservation is not fully processed

                // Forth stage: handle last events
                while (heap_sz > 0ul) {
                    std::pop_heap(heap.data(), heap.data()+heap_sz);
                    const auto& el = heap[--heap_sz];
                    if (el.is_tdc) {
                        tdcHits++;
                        analysis.PurgePeriod(chipIndex, period);
                        tdc_ts = el.ts;
                        period++;
                    } else {
                        toaHits++;
                        analysis.ProcessEvent(chipIndex, period, { el.ts - tdc_ts, el.px });
                    }
                }

                analysis.PurgePeriod(chipIndex, period, true);
                workPassThreeTime += timer.elapsed();

            } catch (Poco::Exception& ex) {
                stopNow();
                logger << threadId << ": analyser exception: " << ex.displayText() << log_critical;
                global::set_error(ex.displayText());
            } catch (std::exception& ex) {
                stopNow();
                logger << threadId << ": analyser exception: " << ex.what() << log_critical;
                global::set_error(ex.what());
            }

            auto workTime = workPassOneTime + workPassTwoTime + workPassThreeTime;
            {
                std::lock_guard lock{memberMutex};
                toaCount += toaHits;
                tdcCount += tdcHits;
                analyseSpinTime += spinTime;
                analysePassOneTime += workPassOneTime;
                analysePassTwoTime += workPassTwoTime;
                analysePassThreeTime += workPassThreeTime;
                analyseWorkTime += workTime;
            }

            analysis_finished.send();
            logger << threadId << ": Processed " << toaHits << " TOA, " << tdcHits << " TDC, work time " << workTime
                    << ", rate " << ((toaHits + tdcHits)/workTime) << log_info;
        } while (true);

        logger << threadId << ": analysis shutdown" << log_debug;
    }

public:
    /*!
    \brief Constructor
    \param log      Poco::Logger object for logging
    \param analysisObj Analysis object
    \param numChips Number of TPX3 chips for the detector that generated the events
    \param queueSize Size of reorder queue (heap)
    */
    inline DataHandler(Logger& log, Analysis& analysisObj, unsigned numChips, unsigned long queueSize)
        : dataStream{}, logger{log}, databuf{numChips}, reorderSize{queueSize},
          analyserThreads(numChips), analysis(analysisObj),
          analysis_finished{numChips}, start_analysis(all_shutdown, numChips)
    {
        logger << "DataHandler(" << numChips << ", " << queueSize << ')' << log_trace;
        for (unsigned i=0; i<analyserThreads.size(); i++)
            analyserThreads[i] = std::thread([this, i]{this->analyseData(i);});
        readerThread = std::thread([this]{this->readData();});
    }

    /*!
    \brief Destructor
    */
    inline ~DataHandler()
    {
        all_shutdown.send();
        if (readerThread.joinable())
            readerThread.join();
        for (auto& thread : analyserThreads) {
            if (thread.joinable())
                thread.join();
        }
    }

    /*!
    \brief Set raw data stream
    \param socket   Raw event data stream receiving end
    */
    inline void rawDataStream(StreamSocket& socket) noexcept
    {
        logger << "DataHandler::rawDataStream(" << socket.address().toString() << ')' << log_trace;
        dataStream = socket;
    }

    /*!
    \brief Request for all threads to stop processing
    */
    inline void stopNow() noexcept
    {
        databuf.stop_now();
    }

    /*!
    \brief Request shutdown for all threads
    */
    inline void shutdown() noexcept
    {
        all_shutdown.send();
        readerThread.join();
        for (auto& thread : analyserThreads)
            thread.join();
    }

    /*!
    \brief Start a raw event data analyser thread for each chip, and one raw event data reader thread
    */
    inline void run_async()
    {
        start_analysis.send();
        start_reader.send();
    }

    /*!
    \brief Wait for completion of reader and analyzer threads
    */
    inline void await()
    {
        iobuf::resetter reset(databuf);
        reader_finished.wait_reset();
        dataStream.shutdown();
        dataStream.close();
        analysis_finished.wait_reset();
    }

    /*!
    \brief Log output
    \param time Total wall time
    */
    inline void logOutput(double time) const
    {
        const auto& gvars = *global::instance;
        const uint64_t ntoa = toaCount;
        const uint64_t ntdc = tdcCount;
        const u64 readCount = (byteCount / sizeof(u64));
        const auto numChips = gvars.layout.chip.size();
        const double avgAnalysisWorkTime = analyseWorkTime / numChips;
        const double avgAnalysisTime = (analyseWorkTime + analyseSpinTime) / numChips;
        logger << "time: " << time << "s tdcs: " << ntdc << " toas: " << ntoa << " at " << (ntoa / time)
                                   << " toas/s rate: " << ((ntoa+ntdc) / time) << " events/s\n"
               << "analysis spin: " << analyseSpinTime << "s work 1:" << analysePassOneTime
                                                                      << " 2:" << analysePassTwoTime
                                                                      << " 3:" << analysePassThreeTime
                                                                      << " self: " << analyseWorkTime
                                                                      << " avg: " << avgAnalysisWorkTime
               << "\n         self rate: " << (ntoa / avgAnalysisWorkTime) << " toas/s " << ((ntoa+ntdc) / avgAnalysisWorkTime) << " events/s"
               << "\n         rate: " << (ntoa / avgAnalysisTime) << " toas/s " << ((ntoa+ntdc) / avgAnalysisTime) << " events/s"
               << "\nreading spin: " << readSpinTime << "s work: " << readTime
                                     << "s total: " << readTotalTime << "s items: " << readCount
                                     << " at " << (readCount / readTotalTime) << " items/s"
                                     << ", " << (ntoa / readTotalTime) << " toas/s"
                                     << ", " << ((ntoa+ntdc) / readTotalTime) << " events/s" << log_notice;
    }

    u64 toaCount = 0ul;                         //!< Number of TOA events encountered
    u64 tdcCount = 0ul;                         //!< Number of TDC events encountered
    u64 byteCount = 0ul;                        //!< Total bytes read
    double readSpinTime = .0;                   //!< Time used in spin loop to wait for empty IO buffers
    double readTime = .0;                       //!< Time used for reading raw event data
    double readTotalTime = .0;                  //!< Total time spent in reader thread
    double analyseSpinTime = .0;                //!< Aggregated time used in spin loop to wait for full IO buffers
    double analysePassOneTime = .0;             //!< Aggregated time used for analysing raw events, pass one
    double analysePassTwoTime = .0;             //!< Aggregated time used for analysing raw events, pass two
    double analysePassThreeTime = .0;           //!< Aggregated time used for analysing raw events, pass three
    double analyseWorkTime = .0;                //!< Effective work time (without waiting for I/O buffers)
};

#endif // DATA_HANDLER_H
