#pragma once

#ifndef COPY_HANDLER_H
#define COPY_HANDLER_H

/*!
\file
Provide raw stream to file copying code
*/

#include <array>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <fstream>

#include "Poco/Exception.h"
#include "Poco/Net/StreamSocket.h"

#include "global.h"
#include "logging.h"
#include "thread_naming.h"

namespace {
    using Poco::Net::StreamSocket;
    using Poco::LogicException;
    using Poco::RuntimeException;
    using Poco::ReadFileException;
    using Poco::DataFormatException;
    using wall_clock = std::chrono::high_resolution_clock;  //!< Clock object
    using namespace std::chrono_literals;
}

/*!
\brief Handler object for copying raw stream data to a file
*/
class CopyHandler final {

    StreamSocket& dataStream;   //!< Raw event data stream receiving end
    std::ofstream streamFile;   //!< Write raw event data into this file
    Logger& logger;             //!< Poco::Logger object for logging

    /*!
    \brief List of IO buffers

    New buffers will be created and filled with raw eevent data on the fly,
    and added to the list at the tail.

    Buffers will be removed and transferred to file from the head.
    */
    std::deque<std::unique_ptr<std::vector<char>>> buffers;

    std::thread readerThread;   //!< Raw event data reader thread
    std::thread writerThread;   //!< Raw event data writer thread
    std::mutex memberMutex;     //!< Protect member variables here
    std::condition_variable data_ready;         //!< Data is ready condition
    std::atomic<bool> stopOperation = false;    //!< Stop requested flag
    bool readOnly;              //!< Don't write, just read

    /*!
    \brief Check stop flag
    \return True for stopping requested
    */
    bool stop() const
    {
        return stopOperation.load(std::memory_order_consume);
    }

    /*!
    \brief Read data into byte buffer
    \param buf  Byte buffer
    \param size Number of bytes to read
    \return Number of bytes effectively read
    */
    int readBytes(void* buf, int size)
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

    /*!
    \brief Code for raw event data reader thread
    */
    void readData()
    {
        double time = .0;
        std::array<char, 8> header;

        set_thread_name("tpx3app:cp-reader");

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
                const auto t1 = wall_clock::now();

                int bytesRead = readBytes(header.data(), header.size());

                const auto t3 = wall_clock::now();
                readOpTime += std::chrono::duration<double>(t3 - t1).count();
                readTotalBytes += bytesRead;

                if (bytesRead == 0)
                    break;
                if (bytesRead < (int)header.size())
                    throw ReadFileException("read incomplete header");

                const u64 value = *(uint64_t*)header.data();
                if ((value & 0xffffffffUL) != 861425748UL)
                    throw DataFormatException("unknown header");

                const u64 chunk_size = value >> 48;
                logger << "chunk " << chunk_size << " bytes\n";

                std::unique_ptr<std::vector<char>> data(new std::vector<char>(chunk_size + 8));
                *(uint64_t*)data->data() = value;

                const auto t4 = wall_clock::now();
                readAllocTime += std::chrono::duration<double>(t4 - t3).count();

                bytesRead = readBytes(&data->data()[8], chunk_size);

                const auto t5 = wall_clock::now();
                readOpTime += std::chrono::duration<double>(t5 - t4).count();
                readTotalBytes += bytesRead;

                std::copy(&header[0], &header[8], &data->data()[0]);

                logger << "read " << bytesRead << " bytes, " << readTotalBytes << " total" << log_debug;

                if (stop())
                    break;

                if (bytesRead < (int)chunk_size)
                    throw DataFormatException("incomplete chunk");

                if (! readOnly) {
                    std::lock_guard lock{memberMutex};
                    buffers.push_back(std::move(data));
                    data_ready.notify_one();
                }

                const auto t2 = wall_clock::now();
                time += std::chrono::duration<double>(t2 - t1).count();
            } while (true);

            buffers.emplace_back(nullptr);
        } catch (Poco::Exception& ex) {
            stopNow();
            logger << "reader exception: " << ex.displayText() << log_critical;
        } catch (std::exception& ex) {
            stopNow();
            logger << "reader exception: " << ex.what() << log_critical;
        }

        readTime += time;
        logger << "reader stopped" << log_debug;
    }

    /*!
    \brief Code for raw event data writer thread
    */
    void writeData()
    {
        set_thread_name("tpx3app:cp-writer");

        try {
            {   // set thread affinity
                int writer_cpu = global::instance->cpu_affinity.writer_cpu;
                if (writer_cpu >= 0) {
                    int rval = cpu_mask::set_affinity(cpu_mask::get_tid(), writer_cpu);
                    if (rval != 0)
                        logger << "writer: set affinity - " << cpu_mask::error(rval) << log_error;
                }
            }

            const auto t3 = wall_clock::now();

            do {
                std::unique_ptr<std::vector<char>> data;

                do {
                    std::unique_lock lock{memberMutex};
                    if (! buffers.empty()) {
                        data = std::move(buffers.front());
                        buffers.pop_front();
                        break;
                    }
                    data_ready.wait_for(lock, 1s);
                } while (!stop());

                if (stop())
                    break;

                if (data == nullptr)
                    break;

                const auto t1 = wall_clock::now();
                streamFile.write(data->data(), data->size());
                const auto t2 = wall_clock::now();
                writeOpTime += std::chrono::duration<double>(t2 - t1).count();
                writeTotalBytes += data->size();
                logger << "write " << data->size() << " bytes, " << writeTotalBytes << " total" << log_debug;

                if (! streamFile)
                    throw ReadFileException("writer error");
            } while (true);

            const auto t4 = wall_clock::now();
            writeTime += std::chrono::duration<double>(t4 - t3).count();

        } catch (Poco::Exception& ex) {
            stopNow();
            logger << "reader exception: " << ex.displayText() << log_critical;
        } catch (std::exception& ex) {
            stopNow();
            logger << "reader exception: " << ex.what() << log_critical;
        }

        logger << "writer stopped" << log_debug;
    }

public:
    /*!
    \brief Constructor
    \param socket   Raw event data receiving end
    \param path     File path for writing the received raw event data
    \param log      Logging object
    */
    CopyHandler(StreamSocket& socket, const std::string& path, Logger& log)
        : dataStream{socket}, streamFile(path), logger{log}
    {
        readOnly = (path == "none");
        logger << "CopyHandler(" << socket.address().toString() << ", " << path << ')' << log_trace;
    }

    /*!
    \brief Request threads to stop
    */
    void stopNow()
    {
        stopOperation.store(true, std::memory_order_release);
    }

    /*!
    \brief Start worker threads for reading and writing of raw event data
    */
    void run_async()
    {
        readerThread = std::thread([this]{this->readData();});
        if (! readOnly)
            writerThread = std::thread([this]{this->writeData();});
    }

    /*!
    \brief Wait for completion of threads for reading and writing raw event data
    */
    void await()
    {
        readerThread.join();
        if (! readOnly)
            writerThread.join();
    }

    double readOpTime = .0;     //!< Time used for synchronous read operations
    double readAllocTime = .0;  //!< Time for allocating buffers
    double readTime = .0;       //!< Time used by raw event data reading thread
    double writeTime = .0;      //!< Time used by raw event data writing thread
    double writeOpTime = .0;    //!< Time used for write operation
    u64 readTotalBytes = 0ul;   //!< Total bytes read
    u64 writeTotalBytes = 0ul;  //!< Total bytes written
};

#endif // COPY_HANDLER_H
