#pragma once

#include <mutex>
#ifndef DATA_HANDLER_H
#define DATA_HANDLER_H  //!< Include flag

/*!
\file
Code for processing raw data stream
*/

#ifndef SERVER_VERSION
    #define SERVER_VERSION 320  //!< Default ASI server version
#endif

#include <cstddef>
#include <algorithm>

#include "Poco/Exception.h"
#include "Poco/Net/StreamSocket.h"

#include "cpu_mask.h"
#include "decoder.h"
#include "logging.h"
#include "global.h"
#include "io_buffers.h"
#include "processing.h"
#include "thread_naming.h"

namespace {
    using Poco::Net::StreamSocket;
    using Poco::LogicException;
    using Poco::RuntimeException;
    using Poco::ReadFileException;
    using Poco::DataFormatException;
    using wall_clock = std::chrono::high_resolution_clock;  //!< Clock object

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
}

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
    io_buffer_pool_collection perChipBufferPool;//!< Per chip IO buffer pool
    const size_t bufferSize;                    //!< IO buffer size in bytes
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
    \return Number of bytes effectively read
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
                ;
            }
            numBytes += numRead;
        } while ((numBytes < size) && !global::instance->stop_collect);

        return numBytes;
    }

    /*!
    \brief Read packet header from raw event data stream
    \param chipIndex    Chip number reference
    \param chunkSize    Raw event data packet chunk size reference
    \param packetId     Raw event data packet number reference
    \return Number of bytes effectively read
    */
    int readPacketHeader(uint64_t& chipIndex, uint64_t& chunkSize, uint64_t& packetId)
    {
        // logger << "readPacketHeader()" << log_trace;
        #if SERVER_VERSION >= 320
            AsiRawStreamDecoder::Event header[2];
        #else
            AsiRawStreamDecoder::Event header[1];
        #endif

        int numRead = readData(header, sizeof(header));
        if (numRead == 0)
            return 0;
        if (numRead != sizeof(header))
            throw ReadFileException(std::string("unable to read packet header") + std::to_string(numRead));

        // logger << "packed header: " << std::hex << header[0]
        //     #if SERVER_VERSION >= 320
        //         << ' ' << header[1]
        //     #endif
        //         << std::dec << log_debug;

        if (header[0].header.id != AsiRawStreamDecoder::chunk_id)
            throw DataFormatException("chunk header expected");
        chipIndex = header[0].header.chip;
        chunkSize = header[0].header.size;
        #if SERVER_VERSION >= 320
            if (header[1].packet_id.type != 0x50)
                throw DataFormatException("packet id expected");
            packetId = header[1].packet_id.count;
            // logger << "packet header: chipIndex " << chipIndex << ", chunkSize " << chunkSize << ", packetId " << packetId << log_debug;
        #else
            packetId = 0;
            // logger << "packet header: chipIndex " << chipIndex << ", chunkSize " << chunkSize << log_info;
        #endif

        return numRead;
    }

    /*!
    \brief Code for raw event data reader thread
    */
    void readData()
    {
        set_thread_name("tpx3app:reader");

        double spinTime = .0;
        double workTime = .0;

        try {
            {   // set thread affinity
                int reader_cpu = global::instance->cpu_affinity.reader_cpu;
                if (reader_cpu >= 0) {
                    int rval = cpu_mask::set_affinity(cpu_mask::get_tid(), reader_cpu);
                    if (rval != 0)
                        logger << "reader: set affinity - " << cpu_mask::error(rval) << log_error;
                }
            }

            do {
                uint64_t chipIndex = 0;
                uint64_t chunkSize = 0;
                uint64_t packetId = 0;
                uint64_t totalBytes = DATA_OFFSET;
                int bytesRead;

                {
                    const auto t1 = wall_clock::now();
                    bytesRead = readPacketHeader(chipIndex, chunkSize, packetId);
                    const auto t2 = wall_clock::now();
                    workTime += std::chrono::duration<double>(t2 - t1).count();
                    if (bytesRead == 0) {
                        logger << "reader: graceful connection shutdown detected" << log_debug;
                        break;
                    }
                }

                while (totalBytes < chunkSize) {
                    auto& bufferPool = *perChipBufferPool[chipIndex];

                    const auto t1 = wall_clock::now();

                    auto eventBuffer = bufferPool.get_empty_buffer();
                    if (eventBuffer == nullptr)
                        throw LogicException("received nullptr as empty buffer");
                    if (eventBuffer->content_size != 0)
                        throw LogicException("empty buffer has content");

                    const auto t2 = wall_clock::now();

                    char* data = eventBuffer->content.data();
                    eventBuffer->content_offset = totalBytes;
                    eventBuffer->chunk_size = chunkSize;

                    do {
                        const int bytesBuffered = eventBuffer->content_size;
                        const int restCapacity = bufferSize - bytesBuffered;
                        const int restData = chunkSize - totalBytes;
                        const int readSize = std::min(restCapacity, restData);
                        bytesRead = dataStream.receiveBytes(&data[bytesBuffered], readSize);
                        totalBytes += bytesRead;

                        // logger << "read " << bytesRead << " bytes into buffer " << eventBuffer->id << ", " << totalBytes
                        //        << " total" << log_debug;

                        eventBuffer->content_size += bytesRead;

                        if (bytesRead <= 0)
                            throw ReadFileException("no bytes received");
                        if (bytesRead == readSize)
                            break;
                        if (stop())
                            goto reader_stopped;
                    } while (true);

                    const auto t3 = wall_clock::now();

                    // {
                    //     auto logproxy = logger << "  data[0..32] = ";
                    //     logproxy << std::hex;
                    //     for (int i=0; i<4; i++)
                    //         logproxy << *reinterpret_cast<uint64_t*>(&data[i*8]) << "  ";
                    //     logproxy << std::dec << log_debug;
                    // }
                    bufferPool.put_nonempty_buffer({ packetId, std::move(eventBuffer) });

                    spinTime += std::chrono::duration<double>{t2 - t1}.count();
                    workTime += std::chrono::duration<double>{t3 - t2}.count();
                }
            } while (true);
        } catch (Poco::Exception& ex) {
            stopNow();
            logger << "reader exception: " << ex.displayText() << log_critical;
            global::set_error(std::string{"reader: "} + ex.displayText());
        } catch (std::exception& ex) {
            stopNow();
            logger << "reader exception: " << ex.what() << log_critical;
            global::set_error(std::string{"reader: "} + ex.what());
        }

    reader_stopped:
        for (auto& pool : perChipBufferPool)
            pool->finish_writing();

        {
            std::lock_guard lock{memberMutex};
            readTime += workTime;
            readSpinTime += spinTime;
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

        perChipBufferPool[chipIndex].reset(new io_buffer_pool{});
        analyzerReady.fetch_add(1, std::memory_order_release);

        uint64_t tdcHits = 0ul;
        uint64_t toaHits = 0ul;
        double spinTime = .0;
        double workPassOneTime = .0;
        double workPassTwoTime = .0;
        double workPassThreeTime = .0;
        double workTime = .0;

        try {

            {   // set thread affinity}
                int analysis_cpu = global::instance->cpu_affinity.get_cpu(threadId);
                if (analysis_cpu >= 0) {
                    int rval = cpu_mask::set_affinity(cpu_mask::get_tid(), analysis_cpu);
                    if (rval != 0)
                        logger << threadId << ": set affinity - " << cpu_mask::error(rval) << log_error;
                }
            }

            auto& bufferPool = *perChipBufferPool[chipIndex];

            // Reorder buffer
            std::vector<event_t> heap(reorderSize);
            size_t heap_sz = 0ul;

            // Buffer for storing TDC event timestamps, and index of free TDC timestamp buffer entry
            std::vector<uint64_t, aligned_allocator<uint64_t>> tdc_buf(io_buffer_pool::buffer_size / sizeof(uint64_t));
            uint64_t tdc_ts = 0u;

            // Period counter
            period_type period = 0u;

            do {

                const auto t1 = wall_clock::now();

                auto [packetNumber, eventBuffer] = bufferPool.get_nonempty_buffer();

                const auto t2 = wall_clock::now();
                spinTime += std::chrono::duration<double>(t2 - t1).count();

                if (eventBuffer == nullptr) // no more data
                    goto analyser_stopped;

                size_t content_sz = eventBuffer->content_size / sizeof(u64);
//                    logger << threadId << ": full buffer " << eventBuffer->id
//                                        << " chunk " << eventBuffer->chunk_size
//                                        << " offset " << eventBuffer->content_offset
//                                        << " size " << eventBuffer->content_size
//                                        << " packet " << packetNumber << log_debug;

                const AsiRawStreamDecoder::Event* content = (const AsiRawStreamDecoder::Event*)eventBuffer->content.data();
                size_t i=0;

                // First stage: extract TDC/TOA events and fill up heap
                // This stage is only active for the very first buffer(s)
                for (; (heap_sz < reorderSize) && (i < content_sz); i++) {
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

                if ((heap_sz >= reorderSize) && (i < content_sz)) {
                    if (tdc_ts == 0ul)
                        std::make_heap(&heap[0], &heap[heap_sz]);

                    // Second stage: assert the presence of the last TDC time
                    for (; (tdc_ts == 0ul) && (i < content_sz); i++) {
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

                    const auto t3 = wall_clock::now();
                    workPassOneTime += std::chrono::duration<double>(t3 - t2).count();

                    // Third stage: handle earlier events while extracting and buffering later TDC/TOA events
                    // This stage is the work horse
                    for (; i < content_sz; i++) {
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

                    const auto t4 = wall_clock::now();
                    workPassOneTime += std::chrono::duration<double>(t4 - t3).count();
                }
                // no more TOA events

                bufferPool.put_empty_buffer(std::move(eventBuffer));

                const auto t5 = wall_clock::now();
                workTime += std::chrono::duration<double>(t5 - t2).count();

            } while(! stop());

        analyser_stopped:
            const auto t1 = wall_clock::now();

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

            const auto t2 = wall_clock::now();
            const auto t = std::chrono::duration<double>(t2 - t1).count();
            workPassThreeTime += t;
            workTime += t;
            
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
    DataHandler(StreamSocket& socket, Logger& log, unsigned long bufSize, unsigned long numChips, unsigned long queueSize)
        : dataStream{socket}, logger{log}, perChipBufferPool{numChips}, bufferSize{bufSize}, reorderSize{queueSize},
          analyserThreads(numChips)
    {
        io_buffer_pool::buffer_size = bufSize;
        logger << "DataHandler(" << socket.address().toString() << ", " << bufSize << ", " << numChips << ')' << log_trace;
    }

    /*!
    \brief Request for all threads to stop
    */
    void stopNow()
    {
        stopOperation.store(true, std::memory_order_release);
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

    uint64_t toaCount = 0u;                     //!< Number of TOA events encountered
    uint64_t tdcCount = 0u;                     //!< Number of TDC events encountered
    double readSpinTime = .0;                   //!< Time used in spin loop to wait for empty IO buffers
    double readTime = .0;                       //!< Time used for reading raw event data
    double analyseSpinTime = .0;                //!< Aggregated time used in spin loop to wait for full IO buffers
    double analysePassOneTime = .0;             //!< Aggregated time used for analysing raw events, pass one
    double analysePassTwoTime = .0;             //!< Aggregated time used for analysing raw events, pass two
    double analysePassThreeTime = .0;           //!< Aggregated time used for analysing raw events, pass three
    double analyseWorkTime = .0;                //!< Effective work time (without waiting for I/O buffers)
};

#endif // DATA_HANDLER_H
