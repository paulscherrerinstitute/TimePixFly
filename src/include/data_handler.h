#pragma once

#ifndef DATA_HANDLER_H
#define DATA_HANDLER_H  //!< Include flag

/*!
\file
Code for processing raw data stream
*/

#if defined(__AVX2__) && !defined(AVX_DECODE)
    #define USE_AVX2_DECODE
#endif

#if defined(USE_AVX2_DECODE)
    #include "avx2_decoder.h"
#endif

#include "Poco/Net/StreamSocket.h"

#include "subreservation.h"
#include "logging.h"
#include "global.h"
#include "processing.h"
#include "thread_naming.h"
#include "timing.h"
#include "event_type.h"

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
    // std::mutex coutMutex;                    //!< Output mutex for debugging
    std::mutex memberMutex;                     //!< Protection for member variables
    std::atomic<unsigned> analyzerReady = 0;    //!< Counter for ready event analyzer threads
    std::atomic<bool> stopOperation = false;    //!< Stop requested flag

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

        double spinTime = .0;
        double workTime = .0;
        u64 readBytes = 0ul;

        try {
            {   // set thread affinity
                int reader_cpu = global::instance->cpu_affinity.reader_cpu;
                if (reader_cpu >= 0) {
                    int rval = cpu_mask::set_affinity(cpu_mask::get_tid(), reader_cpu);
                    if (rval != 0)
                        logger << "reader: set affinity - " << cpu_mask::error(rval) << log_error;
                }
            }

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

        logger << "reader stopped" << log_debug;

    }

    /*!
    \brief Code for analyzer thread
    \param threadId Thread number, must correspond to chip number
    */
    inline void analyseData(unsigned threadId)
    {
        set_thread_name("tpx3app:analyze");

        const unsigned chipIndex = threadId;

        uint64_t tdcHits = 0ul;
        uint64_t toaHits = 0ul;
        double spinTime = .0;
        double workPassOneTime = .0;
        double workPassTwoTime = .0;
        double workPassThreeTime = .0;

        try {

            {   // set thread affinity}
                int analysis_cpu = global::instance->cpu_affinity.get_cpu(threadId);
                if (analysis_cpu >= 0) {
                    int rval = cpu_mask::set_affinity(cpu_mask::get_tid(), analysis_cpu);
                    if (rval != 0)
                        logger << threadId << ": set affinity - " << cpu_mask::error(rval) << log_error;
                }
            }

            // Reorder buffer
            std::vector<event_t> heap(reorderSize);
            size_t heap_sz = 0ul;

            // Last seen TDC event timestamp
            uint64_t tdc_ts = 0u;

            // Period counter
            period_type period = 0u;

            // We are ready
            analyzerReady++;

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
                        processing::purgePeriod(chipIndex, period);
                        tdc_ts = el.ts;
                        period++;
                    } else {
                        toaHits++;
                        processing::processEvent(chipIndex, period, { el.ts - tdc_ts, el.px });
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
                    processing::purgePeriod(chipIndex, period);
                    tdc_ts = el.ts;
                    period++;
                } else {
                    toaHits++;
                    processing::processEvent(chipIndex, period, { el.ts - tdc_ts, el.px });
                }
            }

            processing::purgePeriod(chipIndex, period, true);

            workPassThreeTime += timer.elapsed();
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

            logger << threadId << ": Processed " << toaHits << " TOA, " << tdcHits << " TDC, work time " << workTime
                   << ", rate " << ((toaHits + tdcHits)/workTime) << log_info;
        } catch (Poco::Exception& ex) {
            stopNow();
            logger << threadId << ": analyser exception: " << ex.displayText() << log_critical;
            global::set_error(ex.displayText());
        } catch (std::exception& ex) {
            stopNow();
            logger << threadId << ": analyser exception: " << ex.what() << log_critical;
            global::set_error(ex.what());
        }
    }

public:
    /*!
    \brief Constructor
    \param log      Poco::Logger object for logging
    \param numChips Number of TPX3 chips for the detector that generated the events
    \param queueSize Size of reorder queue (heap)
    */
    inline DataHandler(Logger& log, unsigned numChips, unsigned long queueSize)
        : dataStream{}, logger{log}, databuf{numChips}, reorderSize{queueSize},
          analyserThreads(numChips)
    {
        logger << "DataHandler(" << numChips << ", " << queueSize << ')' << log_trace;
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
    \brief Request for all threads to stop
    */
    inline void stopNow() noexcept
    {
        databuf.stop_now();
    }

    /*!
    \brief Start a raw event data analyser thread for each chip, and one raw event data reader thread
    */
    inline void run_async()
    {
        analyzerReady.store(0);
        for (unsigned i=0; i<analyserThreads.size(); i++)
            analyserThreads[i] = std::thread([this, i]{this->analyseData(i);});
        while (analyzerReady.load(std::memory_order_consume) != analyserThreads.size())
            std::this_thread::yield();
        readerThread = std::thread([this]{this->readData();});
    }

    /*!
    \brief Wait for completion of reader and analyzer threads
    */
    inline void await()
    {
        readerThread.join();
        dataStream.shutdown();
        dataStream.close();
        for (auto& thread : analyserThreads)
            thread.join();
        processing::stop();
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
