#pragma once

#include <cstdint>
#include <string>
#ifndef DATA_HANDLER_H
#define DATA_HANDLER_H  //!< Include flag

/*!
\file
Code for processing raw data stream
*/

#ifndef SERVER_VERSION
    #define SERVER_VERSION 320  //!< Default ASI server version
#endif

#include "Poco/Exception.h"
#include "Poco/Net/StreamSocket.h"

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
    std::thread readerThread;                   //!< Raw event data stream reader thread
    std::vector<std::thread> analyserThreads;   //!< Per chip event analyzer threads
    // std::mutex coutMutex;                    //!< Output mutex for debugging
    spin_lock::type memberMutex{spin_lock::init}; //!< Protection for member variables
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
            spin_lock lock{memberMutex};
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

        uint64_t tdcHits = 0u;
        uint64_t toaHits = 0u;
        double spinTime = .0;
        double workPassOneTime = .0;
        double workPassTwoTime = .0;
        double workPassThreeTime = .0;
        double workTime = .0;

        try {

            auto& bufferPool = *perChipBufferPool[chipIndex];

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

                size_t dataSize = eventBuffer->content_size;
//                    logger << threadId << ": full buffer " << eventBuffer->id
//                                        << " chunk " << eventBuffer->chunk_size
//                                        << " offset " << eventBuffer->content_offset
//                                        << " size " << eventBuffer->content_size
//                                        << " packet " << packetNumber << log_debug;

                char* content = eventBuffer->content.data();
                unsigned toa_hits = 0u; // hits in this buffer
                unsigned tdc_hits = 0u; // hits in this buffer

                // ---------------------------------
                // First pass: extract TDCs and convert TOAs
                for (unsigned i=0; i<dataSize; i+=unsigned{sizeof(uint64_t)}) {
                    auto& d = *reinterpret_cast<AsiRawStreamDecoder::Event*>(&content[i]);
                    if (__builtin_expect(d.type.id == 0xb, 1)) { // TOA
                        *reinterpret_cast<toa_event*>(&d) = { (uint64_t)Decode::getToaClock(d.toa), Decode::flatPixel(d.toa) };
                        toa_hits++;
                        continue;
                    } else if (d.type.id == 0x6) {
                        tdc_buf[tdc_hits++] = Decode::getTdcClock(d.tdc);
                    } else if (__builtin_expect(d.header.id == AsiRawStreamDecoder::chunk_id, 0)) {
                        throw RuntimeException(std::string("encountered chunk header within chunk at offset ") + std::to_string(i));
                    } else if (__builtin_expect(d.packet_id.type == 0x50, 0)) {
                        throw RuntimeException(std::string("encountered packet ID within chunk at offset ") + std::to_string(i));
                    }
                    *reinterpret_cast<uint64_t*>(&d) = -1ul; // Max out everything except TOA
                };

                const auto t3 = wall_clock::now();
                workPassOneTime += std::chrono::duration<double>(t3 - t2).count();

                if (__builtin_expect(tdc_hits == 0u, 0)) {
                    // Handle buffer without TDCs
                    if (tdc_ts == 0u)
                        throw RuntimeException("encountered initial buffer without TDCs");
                    if (toa_hits == 0u)
                        throw RuntimeException("encountered event buffer without events");

                    // Handle TOAs for last seen TDC
                    toa_event* toa_buf = reinterpret_cast<toa_event*>(content);
                    unsigned toa_next = 0u;
                    unsigned next = 0u;
                    do {
                        while (*reinterpret_cast<uint64_t*>(&toa_buf[next]) == -1ul)
                            next++;

                        const toa_event& toa = toa_buf[next++];
                        uint64_t toa_ts = toa.ts;
                        if (toa_ts >= tdc_ts)
                            processing::processEvent(chipIndex, period, { toa_ts - tdc_ts, toa.px });
                    } while (++toa_next != toa_hits);

                    const auto t5 = wall_clock::now();
                    workPassThreeTime += std::chrono::duration<double>(t5 - t3).count();

                    goto no_toa_events;
                }

                if (__builtin_expect(tdc_ts >= tdc_buf[0], 0))
                    throw RuntimeException("encountered event buffer that is out of sequence");

                if (__builtin_expect(toa_hits != 0u, 1)) {
                    // ---------------------------------
                    // Second pass: sort TOAs and TDCs according to time
                    // Maxed out data is pushed to the end
                    std::sort(&tdc_buf[0], &tdc_buf[tdc_hits]);

                    toa_event* toa_buf = reinterpret_cast<toa_event*>(content);
                    {
                        unsigned toa_end = dataSize / sizeof(toa_event);
                        std::sort(&toa_buf[0], &toa_buf[toa_end], [](const auto& a, const auto& b) -> bool {
                            return a.ts < b.ts;
                        });
                        assert((toa_hits >= toa_end) || ((u64&)toa_buf[toa_hits] == -1ul));
                        assert((toa_hits > 0) && ((u64&)toa_buf[toa_hits - 1ul] != -1ul));
                    }

                    const auto t4 = wall_clock::now();
                    workPassTwoTime += std::chrono::duration<double>(t4 - t3).count();

                    // ---------------------------------
                    // Third pass: handle TOAs
                    unsigned tdc_next = 0u;
                    if (__builtin_expect(tdc_ts == 0u, 0)) { // set it to first tdc
                        tdc_ts = tdc_buf[0];
                        tdc_next = 1u;
                    }

                    // Drop TOAs with timestamp less than current TDC timestamp
                    unsigned toa_next = 0u;
                    while (toa_buf[toa_next].ts < tdc_ts) {
                        toa_next++;
                        if (toa_next == toa_hits)
                            goto no_toa_events;
                    }

                    // Handle TOAs up to last TDC
                    while (tdc_next != tdc_hits) {
                        const toa_event& toa = toa_buf[toa_next];
                        uint64_t toa_ts = toa.ts;
                        assert(toa_ts >= tdc_ts);

                        if (toa_ts >= tdc_buf[tdc_next]) {
                            processing::purgePeriod(chipIndex, period + tdc_next);
                            tdc_ts = tdc_buf[tdc_next++];
                            continue;
                        }

                        processing::processEvent(chipIndex, period + tdc_next, { toa_ts - tdc_ts, toa.px });

                        toa_next++;
                        if (toa_next == toa_hits)
                            goto no_toa_events;
                    }

                    // Handle TOAs for last TDC
                    while (toa_next != toa_hits) {
                        const toa_event& toa = toa_buf[toa_next];
                        uint64_t toa_ts = toa.ts;
                        assert(toa_ts >= tdc_ts);

                        processing::processEvent(chipIndex, period + tdc_next, { toa_ts - tdc_ts, toa.px });

                        toa_next++;
                    }

                    const auto t5 = wall_clock::now();
                    workPassThreeTime += std::chrono::duration<double>(t5 - t4).count();
                }

            no_toa_events:
                bufferPool.put_empty_buffer(std::move(eventBuffer));

                if (tdc_hits > 0) {
                    period += tdc_hits;                 // set period counter for next buffer
                    tdc_ts = tdc_buf[tdc_hits - 1u];    // set current tdc timestamp to last TDC
                }

                toaHits += toa_hits;
                tdcHits += tdc_hits;

                const auto t6 = wall_clock::now();
                workTime += std::chrono::duration<double>(t6 - t2).count();

            } while(! stop());

        analyser_stopped:
            processing::purgePeriod(chipIndex, period, true);
            
            {
                spin_lock lock{memberMutex};
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
    */
    DataHandler(StreamSocket& socket, Logger& log, unsigned long bufSize, unsigned long numChips)
        : dataStream{socket}, logger{log}, perChipBufferPool{numChips}, bufferSize{bufSize},
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
    double analyseWorkTime = .0;
};

#endif // DATA_HANDLER_H
