#ifndef COPY_HANDLER_H
#define COPY_HANDLER_H

/*!
\file
Provide raw stream to file copying code
*/

#include <array>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include "Poco/Exception.h"
#include "Poco/Net/StreamSocket.h"
#include "logging.h"

namespace {
    using Poco::Net::StreamSocket;
    using Poco::LogicException;
    using Poco::RuntimeException;
    using Poco::ReadFileException;
    using Poco::DataFormatException;
    using wall_clock = std::chrono::high_resolution_clock;  //!< Clock object
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
    std::atomic<bool> stopOperation = false; //!< Stop requested flag

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

        try {
            uint64_t totalBytes = 0;

            do {
                const auto t1 = wall_clock::now();

                int bytesRead = readBytes(header.data(), header.size());

                if (bytesRead == 0)
                    break;
                if (bytesRead < (int)header.size())
                    throw ReadFileException("read incomplete header");

                uint64_t value = *(uint64_t*)header.data();
                if ((value & 0xffffffffUL) != 861425748UL)
                    throw DataFormatException("unknown header");

                uint64_t chunk_size = value >> 48;
                logger << "chunk " << chunk_size << " bytes\n";

                std::unique_ptr<std::vector<char>> data(new std::vector<char>(chunk_size + 8));
                *(uint64_t*)data->data() = value;

                bytesRead = readBytes(&data->data()[8], chunk_size);

                const auto t2 = wall_clock::now();
                time += std::chrono::duration<double>(t2 - t1).count();

                totalBytes += bytesRead;
                logger << "read " << bytesRead << " bytes, " << totalBytes << " total" << log_debug;

                if (stop())
                    goto reader_stopped;
                if (bytesRead < (int)chunk_size)
                    throw DataFormatException("incomplete chunk");

                {
                    std::lock_guard lock{memberMutex};
                    buffers.push_back(std::move(data));
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
        readTime += time;
        logger << "reader stopped" << log_debug;
    }

    /*!
    \brief Code for raw event data writer thread
    */
    void writeData()
    {
        double time = .0;

        try {
            uint64_t totalBytes = 0;

            do {
                std::unique_ptr<std::vector<char>> data;

                do {
                    std::this_thread::yield();
                    std::lock_guard lock{memberMutex};
                    if (! buffers.empty()) {
                        data = std::move(buffers.front());
                        buffers.pop_front();
                    }
                } while (!stop() && (data.get() == nullptr));

                if (stop())
                    goto writer_stopped;

                const auto t1 = wall_clock::now();
                streamFile.write(data->data(), data->size());
                const auto t2 = wall_clock::now();
                time += std::chrono::duration<double>(t2 - t1).count();
                totalBytes += data->size();
                logger << "write " << data->size() << " bytes, " << totalBytes << " total" << log_debug;

                if (! streamFile)
                    throw ReadFileException("writer error");
            } while (true);
        } catch (Poco::Exception& ex) {
            stopNow();
            logger << "reader exception: " << ex.displayText() << log_critical;
        } catch (std::exception& ex) {
            stopNow();
            logger << "reader exception: " << ex.what() << log_critical;
        }

    writer_stopped:
        writeTime += time;
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
        writerThread = std::thread([this]{this->writeData();});
    }

    /*!
    \brief Wait for completion of threads for reading and writing raw event data
    */
    void await()
    {
        readerThread.join();
        writerThread.join();
    }

    double readTime = .0;   //!< Time used by raw event data reading thread
    double writeTime = .0;  //!< Time used by raw event data writing thread
};

#endif // COPY_HANDLER_H
