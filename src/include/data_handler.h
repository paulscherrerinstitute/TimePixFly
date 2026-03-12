#pragma once

#ifndef DATA_HANDLER_H
#define DATA_HANDLER_H  //!< Include flag

/*!
\file
Code for processing raw data stream
*/

#include <cstddef>
#include <immintrin.h>
#include <popcntintrin.h>

#include "Poco/Exception.h"
#include "Poco/Net/StreamSocket.h"

#include "subreservation.h"
#include "logging.h"
#include "global.h"
#include "io_buf.h"
#include "processing.h"
#include "thread_naming.h"
#include "timing.h"

namespace {
    using Poco::Net::StreamSocket;

    /*!
    \brief Reorder buffer entry
    */
    struct event_t final {
        u64 ts:47;      //!< Timestamp
        u64 is_tdc: 1;  //!< Flag 1=TDC, 0=TOA
        u64 px: 16;     //!< Flat pixel
    };

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

    /*!
    \brief Extract TOA position
    \param events Event vector
    \return TOA flat pixel position vector
    */
    [[gnu::const]]
    __m256i toapos(__m256i events) noexcept
    {
        const static __m256i dcol_mask = _mm256_set1_epi64x(0x0fe00ull);
        const static __m256i spix_mask = _mm256_set1_epi64x(0x001f8ull);
        const static __m256i pix1_mask = _mm256_set1_epi64x(0x7ull);
        const static __m256i pix2_mask = _mm256_set1_epi64x(0x3ull);
        auto encoded = _mm256_srli_epi64(events, 44);
        auto dcol = _mm256_and_si256(encoded, dcol_mask);
        auto spix = _mm256_and_si256(encoded, spix_mask);
        encoded = _mm256_and_si256(encoded, pix1_mask);
        spix = _mm256_add_epi64(spix, _mm256_and_si256(encoded, pix2_mask));
        dcol = _mm256_add_epi64(dcol, _mm256_srli_epi64(encoded, 2));
        return _mm256_add_epi64(spix, _mm256_slli_epi64(dcol, 8));
    }

    /*!
    \brief Extract TOA clock
    \param events Event vector
    \return TOA clock vector
    */
    [[gnu::const]]
    __m256i toaclk(__m256i events) noexcept
    {
        static const __m256i spidr_mask = _mm256_set1_epi64x(0xffffull);
        static const __m256i toa_mask = _mm256_set1_epi64x(0x3fff0ull);
        static const __m256i ftoa_mask = _mm256_set1_epi64x(0xfull);

        const auto spidr = _mm256_slli_epi64(_mm256_and_si256(events, spidr_mask), 14);
        const auto toa = _mm256_and_si256(_mm256_srli_epi64(events, 26), toa_mask);
        auto clk = _mm256_add_epi64(spidr, toa);
        const auto ftoa = _mm256_and_si256(_mm256_srli_epi64(events, 16), ftoa_mask);
        return _mm256_sub_epi64(clk, ftoa);
    }

    /*!
    \brief Extract TDC clock
    \param events Event vector
    \return TDC clock vector
    */
    [[gnu::const]]
    __m256i tdcclk(__m256i events) noexcept
    {
        static const __m256i six = _mm256_set1_epi64x(0x60ull);
        static const __m256i fine_mask = _mm256_set1_epi64x(0xf0ull);
        static const __m256i ts_mask = _mm256_set1_epi64x(0x1ffffffffull);
        const auto fine = _mm256_and_si256(events, fine_mask);
        const auto lastbit = _mm256_srli_epi64(_mm256_cmpgt_epi64(fine, six), 63);
        const auto ts = _mm256_and_si256(_mm256_srli_epi64(events, 8), ts_mask);
        return _mm256_or_si256(ts, lastbit);
    }

    /*!
    \brief Decode raw event vector
    \param events Event vector to be decoded
    \param toa_or_tdc One bit per vector element used as a flag
    \return Decoded event_t event vector
    */
    [[gnu::const]]
    inline __m256i decode(__m256i events, int& toa_or_tdc) noexcept
    {
        static const __m256i toa_type = _mm256_set1_epi64x(0xbull);
        static const __m256i tdc_type = _mm256_set1_epi64x(0x6ull);
        const auto type = _mm256_srli_epi64(events, 60);
        const auto toa_mask = _mm256_cmpeq_epi64(type, toa_type);
        const auto tdc_mask = _mm256_cmpeq_epi64(type, tdc_type);
        const auto mask = _mm256_or_si256(toa_mask, tdc_mask);
        toa_or_tdc = _mm256_movemask_pd(_mm256_castsi256_pd(mask));
        int num_toa = _mm_popcnt_u64(_mm256_movemask_pd(_mm256_castsi256_pd(toa_mask)));
        int num_tdc = _mm_popcnt_u64(_mm256_movemask_pd(_mm256_castsi256_pd(tdc_mask)));
        __m256i res = _mm256_setzero_si256();
        __m256i toa_ev = res;
        if (num_toa) {
            toa_ev = toaclk(events);
            auto toa_pos = toapos(events);
            toa_ev = _mm256_or_si256(toa_ev, _mm256_slli_epi64(toa_pos, 48));
        }
        __m256i tdc_ev = res;
        if (num_tdc) {
            tdc_ev = _mm256_set1_epi64x(1ull << 47);    // TDC flag
            tdc_ev = _mm256_or_si256(tdc_ev, tdcclk(events));
        }
        res = _mm256_castpd_si256(_mm256_blendv_pd(
            _mm256_castsi256_pd(res),
            _mm256_castsi256_pd(toa_ev),
            _mm256_castsi256_pd(toa_mask)
        ));
        return _mm256_castpd_si256(_mm256_blendv_pd(
            _mm256_castsi256_pd(res),
            _mm256_castsi256_pd(tdc_ev),
            _mm256_castsi256_pd(tdc_mask)
        ));
    }

    /*!
    \brief Iterator over events
    */
    class event_iterator final {
        static constexpr unsigned mask = ~0x3u;         //!< For alignment to 4
        const __m256i* data;                            //!< Raw event vector data
        alignas(sizeof(__m256i)) event_t current[4];    //!< Current event_t vector
        int toa_or_tdc;                                 //!< "is_event" flags for current vector elements
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
        event_iterator(const iobuf::reservation_t& reservation, const iobuf::subreservation_t& subreservation) noexcept
        {
            assert(subreservation.content);
            data = (__m256i*)subreservation.content;
            pos = reservation.start / sizeof(u64) + subreservation.pos;
            end = pos + subreservation.consume;
            unsigned start = pos & mask;
            cur = pos - start;
            pos = start;
            assert(pos + cur <= end);

            if (cur != 0)
                _mm256_store_epi64(current, decode(_mm256_load_si256(&data[pos >> 2]), toa_or_tdc));

            toa_or_tdc >>= cur;
        }

        /*!
        \brief Get next event, if possible
        \param event Set this to the next event, if possible
        \return It was possible
        */
        bool next(event_t& event) noexcept
        {
            // Load if cur == 0
            assert(cur < 4);

            while (pos + cur < end) {

                if (cur == 0)
                    _mm256_store_epi64(current, decode(_mm256_load_si256(&data[pos >> 2]), toa_or_tdc));

                bool is_event = (toa_or_tdc & 1);
                toa_or_tdc >>= 1;

                if (is_event)
                    event = current[cur];

                if (++cur == 4) {
                    pos += 4;
                    cur = 0;
                }

                if (is_event)
                    return true;
            }

            return false;
        }
    }; // event_iterator

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

    StreamSocket& dataStream;                   //!< Raw event data stream receiving end
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
    bool stop() const
    {
        return stopOperation.load(std::memory_order_consume);
    }

    /*!
    \brief Read from raw event data stream into buffer
    \param buf Byte buffer
    \param size Number of bytes to read
    \return Number of bytes effectively read, 0 for no more data
    */
    int readData(void* buf, int size)
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
    void readData()
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
                
                // logger << "read " << bytesRead << " bytes, " << readBytes << " total" << log_debug;

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
    void analyseData(unsigned threadId)
    {
        set_thread_name("tpx3app:analyze");

        const unsigned chipIndex = threadId;

        uint64_t tdcHits = 0ul;
        uint64_t toaHits = 0ul;
        double spinTime = .0;
        double workPassOneTime = .0;
        double workPassTwoTime = .0;
        double workPassThreeTime = .0;

        analyzerReady++;

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

            Timer timer;

            iobuf::reservation_t reservation = databuf.read_reservation(iobuf::initial_reservation);
            iobuf::subreservation_t subreservation{chipIndex};

            spinTime += timer.elapsed_reset();

            while (reservation.end) {
                assert(reservation.jar && reservation.jar->container.data && (reservation.start < reservation.end));
                subreservation.update(reservation);

                while (subreservation.rest) {
                    assert(subreservation.consume > 0);
                    const auto data_start = reservation.start / sizeof(u64) + subreservation.pos;
                    const auto data_end = data_start + subreservation.consume;

    //                    logger << threadId << ": full buffer " << eventBuffer->id
    //                                        << " chunk " << eventBuffer->chunk_size
    //                                        << " offset " << eventBuffer->content_offset
    //                                        << " size " << eventBuffer->content_size
    //                                        << " packet " << packetNumber << log_debug;

                    const AsiRawStreamDecoder::Event* content = subreservation.content;
                    auto i = data_start;

                    // search for packet header with correct chip

                    // First stage: extract TDC/TOA events and fill up heap
                    // This stage is only active for the very first buffer(s)
                    for (; (heap_sz < reorderSize) && (i < data_end); i++) {
                        const auto& ev = content[i];
                        const auto type = ev.type.id;
                        if (type == 0xb) { // TOA
                            heap[heap_sz++] = { Decode::getToaClock(ev.toa), 0ul, Decode::flatPixel(ev.toa) };
                            toaHits++;
                        } else if (type == 0x6) { // TDC
                            heap[heap_sz++] = { Decode::getTdcClock(ev.tdc), 1ul, 0ul };
                            tdcHits++;
                        }
                    }

                    if ((heap_sz >= reorderSize) && (i < data_end)) {
                        if (tdc_ts == 0ul)
                            std::make_heap(&heap[0], &heap[heap_sz]);

                        // Second stage: assert the presence of the last TDC time
                        for (; (tdc_ts == 0ul) && (i < data_end); i++) {
                            std::pop_heap(&heap[0], &heap[heap_sz]);
                            const auto& el = heap[--heap_sz];
                            if (el.is_tdc) {
                                tdc_ts = el.ts;
                            }
                            const auto& ev = content[i];
                            const auto type = ev.type.id;
                            if (type == 0xb) { // TOA
                                heap[heap_sz++] = { Decode::getToaClock(ev.toa), 0ul, Decode::flatPixel(ev.toa) };
                                toaHits++;
                            } else if (type == 0x6) { // TDC
                                heap[heap_sz++] = { Decode::getTdcClock(ev.tdc), 1ul, 0ul };
                                tdcHits++;
                            } else {
                                continue;
                            }
                            std::push_heap(&heap[0], &heap[heap_sz]);
                        }

                        workPassOneTime += timer.elapsed_reset();

                        // Third stage: handle earlier events while extracting and buffering later TDC/TOA events
                        // This stage is the work horse
                        for (; i < data_end; i++) {
                            std::pop_heap(&heap[0], &heap[heap_sz]);
                            const auto& el = heap[--heap_sz];
                            if (el.is_tdc) {
                                processing::purgePeriod(chipIndex, period);
                                tdc_ts = el.ts;
                                period++;
                            } else {
                                processing::processEvent(chipIndex, period, { el.ts - tdc_ts, el.px });
                            }
                            const auto& ev = content[i];
                            const auto type = ev.type.id;
                            if (type == 0xb) { // TOA
                                heap[heap_sz++] = { Decode::getToaClock(ev.toa), 0ul, Decode::flatPixel(ev.toa) };
                                toaHits++;
                            } else if (type == 0x6) { // TDC
                                heap[heap_sz++] = { Decode::getTdcClock(ev.tdc), 1ul, 0ul };
                                tdcHits++;
                            } else {
                                continue;
                            }
                            std::push_heap(&heap[0], &heap[heap_sz]);
                        }

                        workPassTwoTime += timer.elapsed_reset();
                    }
                    // no more TOA events

                    subreservation.update(reservation);
                } // while reservation is not fully processed

                reservation = databuf.read_reservation(reservation);

                spinTime += timer.elapsed_reset();
            } // process next reservation

            // Forth stage: handle last events
            while (heap_sz > 0ul) {
                std::pop_heap(&heap[0], &heap[heap_sz]);
                const auto& el = heap[--heap_sz];
                if (el.is_tdc) {
                    processing::purgePeriod(chipIndex, period);
                    tdc_ts = el.ts;
                    period++;
                } else {
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
    \param socket   Raw event data stream receiving end
    \param log      Poco::Logger object for logging
    \param bufSize  IO buffer size
    \param numChips Number of TPX3 chips for the detector that generated the events
    \param queueSize Size of reorder queue (heap)
    */
    DataHandler(StreamSocket& socket, Logger& log, unsigned numChips, unsigned long queueSize)
        : dataStream{socket}, logger{log}, databuf{numChips}, reorderSize{queueSize},
          analyserThreads(numChips)
    {
        logger << "DataHandler(" << socket.address().toString() << ", " << numChips << ", " << queueSize << ')' << log_trace;
    }

    /*!
    \brief Request for all threads to stop
    */
    void stopNow()
    {
        databuf.stop_now();
    }

    /*!
    \brief Start a raw event data analyser thread for each chip, and one raw event data reader thread
    */
    void run_async()
    {
        for (unsigned i=0; i<analyserThreads.size(); i++)
            analyserThreads[i] = std::thread([this, i]{this->analyseData(i);});
        while (analyzerReady.load(std::memory_order_consume) != analyserThreads.size())
            std::this_thread::yield();
        readerThread = std::thread([this]{this->readData();});
    }

    /*!
    \brief Wait for completion of reader and analyzer threads
    */
    void await()
    {
        readerThread.join();
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
