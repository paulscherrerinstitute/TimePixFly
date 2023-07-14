#ifndef DATA_HANDLER_H
#define DATA_HANDLER_H

#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include "Poco/Exception.h"
#include "Poco/Net/StreamSocket.h"
#include "logging.h"
#include "io_buffers.h"

namespace {
    using Poco::Net::StreamSocket;
    using Poco::LogicException;
    using Poco::RuntimeException;
    using Poco::ReadFileException;
    using Poco::DataFormatException;
    using wall_clock = std::chrono::high_resolution_clock;
}

template<typename Decode>
class DataHandler final {
    //      tpxHeader = int.from_bytes(b'TPX3', 'little')
    // const uint32_t tpxHeader = Poco::ByteOrder::fromLittleEndian(*reinterpret_cast<const uint64_t*>(tpxBytes.data()));
    constexpr static uint64_t tpxHeader = 861425748UL; // 'TPX3' as uint64_t
    #if SERVER_VERSION >= 320
        uint64_t DATA_OFFSET = 8;   // start of event data
    #else
        uint64_t DATA_OFFSET = 0;   // start of event data
    #endif

    StreamSocket& dataStream;
    Logger& logger;
    io_buffer_pool_collection perChipBufferPool;
    const size_t bufferSize;
    std::thread readerThread;
    std::vector<std::thread> analyserThreads;
    std::mutex coutMutex;
    std::mutex memberMutex;
    std::atomic<unsigned> analyzerReady = 0;
    std::atomic<bool> stopOperation = false;

    bool stop() const
    {
        return stopOperation.load(std::memory_order_relaxed);
    }

    void stopNow()
    {
        stopOperation.store(true, std::memory_order_relaxed);
    }

    int readData(void* buf, int size)
    {
        logger << "readData(" << buf << ", " << size << ')' << log_trace;
        int numBytes = 0;

        do {
            int numRead = dataStream.receiveBytes(&static_cast<char*>(buf)[numBytes], size - numBytes);
            if (numRead == 0)
                break;
            numBytes += numRead;
        } while (numBytes < size);

        return numBytes;
    }

    int readPacketHeader(uint64_t& chipIndex, uint64_t& chunkSize, uint64_t& packetId)
    {
        logger << "readPacketHeader()" << log_trace;
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

        logger << "packed header: " << std::hex << header[0]
            #if SERVER_VERSION >= 320
                << ' ' << header[1]
            #endif
                << std::dec << log_debug;

        //         if isChip:
        //             chipIndex = get_bits(d, 39, 32)
        //             # print('ChipIndex: ', chipIndex)
        if ((header[0] & 0xffffffffUL) != tpxHeader)
            throw DataFormatException("chunk header expected");
        chipIndex = Decode::getBits(header[0], 39, 32);
        chunkSize = Decode::getBits(header[0], 63, 48);
        #if SERVER_VERSION >= 320
            if (!Decode::matchesByte(header[1], 0x50))
                throw DataFormatException("packet id expected");
            packetId = Decode::getBits(header[1], 47, 0);
            logger << "packet header: chipIndex " << chipIndex << ", chunkSize " << chunkSize << ", packetId " << packetId << log_info;
        #else
            packetId = 0;
            logger << "packet header: chipIndex " << chipIndex << ", chunkSize " << chunkSize << log_info;
        #endif

        return numRead;
    }

    void readData()
    {
        // with connection:
        //     while reading:

        //         # Read a multiple of 8 into buffer_
        //         read = 0
        //         while read == 0 or read % 8 != 0:
        //             newly_read = connection.recv_into(view[read:], bytes_to_read - read)

        //             if newly_read == 0:
        //                 reading = False
        //                 break

        //             read += newly_read
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
                    if (bytesRead == 0)
                        break;
                }

                while (totalBytes < chunkSize) {
                    auto& bufferPool = *perChipBufferPool[chipIndex];

                    const auto t1 = wall_clock::now();

                    auto eventBuffer = bufferPool.get_empty_buffer();
                    if (eventBuffer == nullptr)
                        throw LogicException("received nullptr as empty buffer");

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

                        logger << "read " << bytesRead << " bytes into buffer " << eventBuffer->id << ", " << totalBytes << " total" << log_debug;

                        eventBuffer->content_size += bytesRead;

                        if (bytesRead <= 0)
                            throw ReadFileException("no bytes received");
                        if (bytesRead == readSize)
                            break;
                        if (stop())
                            goto reader_stopped;
                    } while (true);

                    const auto t3 = wall_clock::now();

                    bufferPool.put_nonempty_buffer({ packetId, std::move(eventBuffer) });

                    spinTime += std::chrono::duration<double>{t2 - t1}.count();
                    workTime += std::chrono::duration<double>{t3 - t2}.count();
                }
            } while (true);
        } catch (Poco::Exception& ex) {
            stopNow();
            logger << "reader exception: " << ex.displayText() << log_critical;
        } catch (std::exception& ex) {
            stopNow();
            logger << "reader exception: " << ex.what() << log_critical;
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

    void analyseData(unsigned threadId)
    {
        // def process_raw_data(buffer_, N):                

        //     chipIndex = 0
        //     count = 0

        const unsigned chipIndex = threadId;

        // init buffer pool / io_buffer_pool::buffer_size set elsewhere
        perChipBufferPool[chipIndex].reset(new io_buffer_pool{});
        analyzerReady.fetch_add(1, std::memory_order_release);

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
                    logger << threadId << ": full buffer " << eventBuffer->id
                                        << " chunk " << chunkSize
                                        << " offset " << eventBuffer->content_offset
                                        << " size " << eventBuffer->content_size
                                        << " packet " << packetNumber << log_debug;

                    size_t processingByte = 0;
                    const char* content = eventBuffer->content.data();

                    //     for i in range(N):
                    while (processingByte < dataSize) {
                        //         d = int(buffer_[i])
                        uint64_t d = *reinterpret_cast<const uint64_t*>(&content[processingByte]);

                        //         isHit = matches_nibble(d, 0xb)
                        //         isTDC = matches_nibble(d, 0x6)
                        //         isChip = (d & ((1 << 32) - 1) == tpxHeader)
                        // bool isHit = matchesNibble(d, 0xb);
                        // bool isTdc = matchesNibble(d, 0x6);
                        // bool isChip = (d & 0xffffffffUL) == tpxHeader;

                        if ((d & 0xffffffffUL) == tpxHeader) {
                            throw RuntimeException(std::string("encountered chunk header within chunk at offset ") + std::to_string(processingByte));
                        } else if (Decode::matchesByte(d, 0x50)) {
                            throw RuntimeException(std::string("encountered packet ID within chunk at offset ") + std::to_string(processingByte));
                        } else if (Decode::matchesNibble(d, 0xb)) {
                            //         if isHit:
                            //             # Note: calculating TOF is harder, since the order of the pixel data
                            //             # is not guaranteed. Therefore in worst case all pixel data must be sorted.
                            //             toa = clock_to_float(get_TOA_clock(d))
                            //             tot = clock_to_float(get_TOT_clock(d), clock=40e6)
                            //             x, y = calculate_XY(d)
                            //             count += 1
                            //             print('Hit: ', chipIndex, x, y, tot, toa)
                            const int64_t toaclk = Decode::getToaClock(d);
                            const uint64_t totclk = Decode::getTotClock(d);
                            float toa = Decode::clockToFloat(toaclk);
                            float tot = Decode::clockToFloat(totclk, 40e6);
                            std::pair<uint64_t, uint64_t> xy = Decode::calculateXY(d);
                            hits++;
                            logger << threadId << ": Hit: " << chipIndex << ' ' << xy.first << ' ' << xy.second << ' ' << tot << ' ' << toa << "  (" << totclk << ' ' << toaclk << ' ' << std::hex << d << std::dec << ')' << log_info;
                        } else if (Decode::matchesNibble(d, 0x6)) {
                            //         elif isTDC:
                            //             tdc = clock_to_float(get_TDC_clock(d))
                            //             # print('TDC: ', tdc)
                            float tdc = Decode::clockToFloat(Decode::getTdcClock(d));
                            logger << threadId << ": TDC: " << tdc << log_info;
                        } else {
                            logger << threadId << ": unknown " << std::hex << d << std::dec << log_info;
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

        } catch (Poco::Exception& ex) {
            stopNow();
            logger << threadId << ": analyser exception: " << ex.displayText() << log_critical;
        } catch (std::exception& ex) {
            stopNow();
            logger << threadId << ": analyser exception: " << ex.what() << log_critical;
        }

    analyser_stopped:
        {
            std::lock_guard lock{memberMutex};
            hitCount += hits;
            analyseTime += workTime;
            analyseSpinTime += spinTime;
        }

        //         print('Processed %i hits' % processed)
        logger << threadId << ": Processed " << hits << " hits" << log_info;
    }

public:
    DataHandler(StreamSocket& socket, Logger& log, unsigned long bufSize, unsigned long numChips)
        : dataStream{socket}, logger{log}, perChipBufferPool{numChips}, bufferSize{bufSize}
    {
        io_buffer_pool::buffer_size = bufSize;
        logger << "DataHandler(" << socket.address().toString() << ", " << bufSize << ", " << numChips << ')' << log_trace;
        analyserThreads.resize(numChips);
    }

    void run_async()
    {
        for (unsigned i=0; i<analyserThreads.size(); i++)
            analyserThreads[i] = std::thread([this, i]{this->analyseData(i);});
        while (analyzerReady.load(std::memory_order_consume) != analyserThreads.size())
            std::this_thread::yield();
        readerThread = std::thread([this]{this->readData();});
    }

    void await()
    {
        readerThread.join();
        for (auto& thread : analyserThreads)
            thread.join();
    }

    uint64_t hitCount = 0;
    double readSpinTime = .0;
    double readTime = .0;
    double analyseSpinTime = .0;
    double analyseTime = .0;
};

#endif // DATA_HANDLER_H