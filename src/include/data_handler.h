#pragma once

#ifndef DATA_HANDLER_H
#define DATA_HANDLER_H  //!< Include flag

/*!
\file
Code for processing raw data stream
*/

#include <cstddef>

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

            do {
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
            } while (bytesRead);

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
                char* data = reservation.jar->container.data;
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

                    const AsiRawStreamDecoder::Event* content = (const AsiRawStreamDecoder::Event*)data;
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
            }

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
