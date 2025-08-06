#ifndef DATA_HANDLER_H
#define DATA_HANDLER_H

/*!
\file
Code for processing raw data stream
*/

#ifndef SERVER_VERSION
    #define SERVER_VERSION 320  //!< Default ASI server version
#endif

#include "Poco/Exception.h"
#include "Poco/Net/StreamSocket.h"
#include "logging.h"
#include "global.h"
#include "io_buffers.h"
#include "period_predictor.h"
#include "period_queues.h"
#include "processing.h"

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
    constexpr static uint64_t tpxHeader = 861425748UL; //!< 'TPX3' as uint64_t
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

    int64_t initialPeriod;                      //!< Initial TDC period interval in clock ticks
    std::vector<period_predictor> predictor;    //!< Per chip period predictors
    std::vector<period_queues> queues;          //!< Per chip period interval change event reorder queues
    unsigned maxPeriodQueues = 2;               //!< Default value for number of memorized period change intervals

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
            uint64_t header[2];
        #else
            uint64_t header[1];
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

        if ((header[0] & 0xffffffffUL) != tpxHeader)
            throw DataFormatException("chunk header expected");
        chipIndex = Decode::getBits(header[0], 39, 32);
        chunkSize = Decode::getBits(header[0], 63, 48);
        #if SERVER_VERSION >= 320
            if (!Decode::matchesByte(header[1], 0x50))
                throw DataFormatException("packet id expected");
            packetId = Decode::getBits(header[1], 47, 0);
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

    // ----------------------- BINNING AND PURGING LOGIC ------------------------
    /*!
    \brief Purge period change interval from memory
    \param chipIndex    Chip number
    \param period       Period number
    */
    inline void purgePeriod(unsigned chipIndex, period_type period)
    {
        processing::purgePeriod(chipIndex, period);
    }

    /*!
    \brief Process TOA event
    \param chipIndex    Chip number
    \param period       Period number
    \param toaclk       TOA event clock ticks counter
    \param event        Raw TOA event
    */
    inline void processEvent(unsigned chipIndex, period_type period, int64_t toaclk, uint64_t event)
    {
        auto start = queues[chipIndex][period].start;
        // processing::processEvent(chipIndex, period, toaclk, toaclk - start, event);
        processing::processEvent(chipIndex, period, toaclk - start, event);
    }
    // --------------------------------------------------------------------------

    /*!
    \brief Purge period interval changes from memory

    Will purge older period interval change reorder queues so that only
    `toSize` queues are remaining in memory.

    \param chipIndex    Chip number
    \param toSize       Number of period interval changes that should still be remembered
    */
    inline void purgeQueues(unsigned chipIndex, unsigned toSize=0)
    {
        // logger << "purgeQueues(" << chipIndex << ", " << toSize << ')' << log_trace;
        while (queues[chipIndex].size() > toSize) {
            auto pp = queues[chipIndex].oldest();
            purgePeriod(chipIndex, pp->first);
            // logger << chipIndex << ": remove queue for period " << pp->first << log_debug;
            queues[chipIndex].erase(pp);
        }
    }

    /*!
    \brief Process TDC event
    \param chipIndex    Chip number
    \param index        Abstract period index
    \param tdcclk       TDC clock
    */
    inline void processTdc(unsigned chipIndex, period_index& index, int64_t tdcclk) //, uint64_t event)
    {
//        logger << "processTdc(" << chipIndex << ", " << index << ", " << tdcclk << ", " << std::hex << event << std::dec << ')' << log_trace;
        // const float tdc = Decode::clockToFloat(tdcclk);
//        logger << chipIndex << ": TDC: " << tdc << log_info;
        auto& rq = queues[chipIndex].registerStart(index, tdcclk);
        for (; !rq.empty(); rq.pop()) {
            auto& el = rq.top();
            processEvent(chipIndex, (tdcclk <= el.toa ? index.disputed_period : index.period), el.toa, el.event);
        }
        // remove old period data
        purgeQueues(chipIndex, maxPeriodQueues);
    }

    /*!
    \brief Remember TOA event that falls into a disputed period change interval
    \param chipIndex    Chip number
    \param index        Abstract period index
    \param toaclk       TOA clock ticks counter
    \param event        Raw event
    */
    inline void enqueueEvent(unsigned chipIndex, period_index index, int64_t toaclk, uint64_t event)
    {
        // logger << "enqueueEvent(" << chipIndex << ", " << index.period << ", " << toaclk << ", " << std::hex << event << std::dec << ')' << log_trace;
        // logger << chipIndex << ": enqueue: " << index.period << ' ' << toaclk
        //        << " (" << std::hex << event << std::dec << ')' << log_debug;
        queues[chipIndex][index].queue->push({toaclk, event});
    }

    /*!
    \brief Code for analyzer thread
    \param threadId Thread number, must correspond to chip number
    */
    void analyseData(unsigned threadId)
    {
        const unsigned chipIndex = threadId;

        perChipBufferPool[chipIndex].reset(new io_buffer_pool{});
        analyzerReady.fetch_add(1, std::memory_order_release);

        uint64_t tdcHits = 0;
        double spinTime = .0;
        double workTime = .0;
        uint64_t hits = 0;

        try {

            auto& bufferPool = *perChipBufferPool[chipIndex];

            do {
                uint64_t chunkSize = 0;
                uint64_t totalBytes = DATA_OFFSET;

                do {

                    const auto t1 = wall_clock::now();

                    auto [packetNumber, eventBuffer] = bufferPool.get_nonempty_buffer();

                    const auto t2 = wall_clock::now();

                    if (eventBuffer == nullptr) // no more data
                        goto analyser_stopped;

                    size_t dataSize = eventBuffer->content_size;
                    chunkSize = eventBuffer->chunk_size;
//                    logger << threadId << ": full buffer " << eventBuffer->id
//                                        << " chunk " << chunkSize
//                                        << " offset " << eventBuffer->content_offset
//                                        << " size " << eventBuffer->content_size
//                                        << " packet " << packetNumber << log_debug;

                    size_t processingByte = 0;
                    const char* content = eventBuffer->content.data();
                    bool predictorReady = (tdcHits >= 3);

                    while (processingByte < dataSize) {
                        uint64_t d = *reinterpret_cast<const uint64_t*>(&content[processingByte]);
                        if (__builtin_expect((d & 0xffffffffUL) == tpxHeader, 0)) {
                            throw RuntimeException(std::string("encountered chunk header within chunk at offset ") + std::to_string(processingByte));
                        } else if (__builtin_expect(Decode::matchesNibble(d, 0xb), 1)) {
                            if (__builtin_expect(predictorReady, 1)) {
                                const int64_t toaclk = Decode::getToaClock(d);
                                const double period = predictor[chipIndex].period_prediction(toaclk);
                                auto index = queues[chipIndex].period_index_for(period);
  //                              logger << threadId << ": toaclk=" << toaclk << ", period=" << period << ", index=" << index << ", predictor=" << predictor[chipIndex] << log_debug;
                                queues[chipIndex].refined_index(index, toaclk);
                                hits++;
                                if (! index.disputed)
                                    processEvent(chipIndex, index.period, toaclk, d);
                                else
                                    enqueueEvent(chipIndex, index, toaclk, d);
                            } else {
                                // logger << threadId << ": skip event " << std::hex << d << std::dec << log_info;
                            }
                        } else if (__builtin_expect(Decode::matchesNibble(d, 0x6), 0)) {
                            const uint64_t tdcclk = Decode::getTdcClock(d);
    //                        logger << threadId << ": tdc " << tdcclk  << " (" << std::hex << d << std::dec << ')' << log_debug;
                            if (__builtin_expect(tdcHits == 0, 0)) {
                                predictor[chipIndex].reset(tdcclk, initialPeriod);
    //                            logger << threadId << ": predictor start, tdc " << tdcclk << " predictor " << predictor[chipIndex] << log_info;
                            } else {
                                predictor[chipIndex].prediction_update(tdcclk);
                                if (tdcHits == 2) {
                                    predictorReady = true;
    //                                logger << threadId << ": predictor ready, tdc " << tdcclk << " predictor " << predictor[chipIndex] << log_info;
                                }
                            }
                            tdcHits++;
                            if (__builtin_expect(predictorReady, 1)) {
                                const double period = predictor[chipIndex].period_prediction(tdcclk);
                                auto index = queues[chipIndex].period_index_for(period);
                                if (! __builtin_expect(index.disputed, 1)) {
//                                    logger << threadId << ": tdc=" << tdcclk << ", period=" << period << ", index=" << index << ", predictor=" << predictor[chipIndex] << log_fatal;
                                    throw RuntimeException(std::string("encountered undisputed period for tdc - tdc ") + std::to_string(tdcclk) + ", predictor " + predictor[chipIndex].to_string());
                                }
                                if (! predictor[chipIndex].ok(tdcclk)) {
                                    predictor[chipIndex].start_update(tdcclk);
//                                    logger << threadId << ": predictor recalibrate " << predictor[chipIndex] << log_info;
                                }
                                processTdc(chipIndex, index, tdcclk); //, d);
                            }
                        } else {
                            // if (__builtin_expect(Decode::matchesByte(d, 0x71), 0)) { // end readout
                            //     stopNow();
                            // }
                            if (__builtin_expect(Decode::matchesByte(d, 0x50), 0)) {
                                throw RuntimeException(std::string("encountered packet ID within chunk at offset ") + std::to_string(processingByte));
                            }
                            // logger << threadId << ": unknown " << std::hex << d << std::dec << log_info;
                        }

                        processingByte += sizeof(uint64_t);
                    }

                    bufferPool.put_empty_buffer(std::move(eventBuffer));

                    if (processingByte != dataSize)
                        throw LogicException("processingByte != dataSize");

                    totalBytes += dataSize;

                    const auto t3 = wall_clock::now();

                    spinTime += std::chrono::duration<double>(t2 - t1).count();
                    workTime += std::chrono::duration<double>(t3 - t2).count();

                } while (totalBytes < chunkSize);

            } while(! stop());

        analyser_stopped:

            // purge remaining queues
            purgeQueues(chipIndex);
            purgePeriod(chipIndex, std::numeric_limits<period_type>::max());

            {
                spin_lock lock{memberMutex};
                hitCount += hits;
                analyseTime += workTime;
                analyseSpinTime += spinTime;
            }

            logger << threadId << ": Processed " << hits << " events, " << tdcHits << " TDCs" << log_info;
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
    \param period   Initial TDC period
    \param undisputedThreshold Ratio r of disputed period interval, [r..1-r] will be undisputed. Must be less than 0.5
    \param maxQueues Number of recent period interval changes to remember
    */
    DataHandler(StreamSocket& socket, Logger& log, unsigned long bufSize, unsigned long numChips, int64_t period, double undisputedThreshold, unsigned maxQueues)
        : dataStream{socket}, logger{log}, perChipBufferPool{numChips}, bufferSize{bufSize},
          analyserThreads(numChips), initialPeriod(period), predictor(numChips), queues(numChips),
          maxPeriodQueues(maxQueues)
    {
        io_buffer_pool::buffer_size = bufSize;
        logger << "DataHandler(" << socket.address().toString() << ", " << bufSize << ", " << numChips << ", " << period << ", " << undisputedThreshold << ')' << log_trace;
        for (auto& q : queues)
            q.threshold = undisputedThreshold;
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

    uint64_t hitCount = 0;      //!< Number of TOA events encountered
    double readSpinTime = .0;   //!< Time used in spin loop to wait for empty IO buffers
    double readTime = .0;       //!< Time used for reading raw event data
    double analyseSpinTime = .0;//!< Aggregated time used in spin loop to wait for full IO buffers
    double analyseTime = .0;    //!< Aggregated time used for analysing raw events
};

#endif // DATA_HANDLER_H
