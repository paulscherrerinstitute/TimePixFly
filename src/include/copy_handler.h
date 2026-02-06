#pragma once

#ifndef COPY_HANDLER_H
#define COPY_HANDLER_H

/*!
\file
Provide raw stream to file copying code
*/

#include <fstream>

#include "Poco/Net/StreamSocket.h"

#include "global.h"
#include "logging.h"
#include "thread_naming.h"
#include "io_buf.h"
#include "timing.h"

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
    iobuf::collection_t queue;  //!< I/O buffer collection
    Logger& logger;             //!< Poco::Logger object for logging

    std::thread readerThread;   //!< Raw event data reader thread
    std::thread writerThread;   //!< Raw event data writer thread

    bool readOnly;              //!< Don't write, just read

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

            Timer timer;
            iobuf::reservation_t reservation = queue.write_reservation(iobuf::initial_reservation, iobuf::container_size);

            do {
                assert(reservation.jar && reservation.jar->container.data && (reservation.start < reservation.end));
                char* data = reservation.jar->container.data;
                auto amount = reservation.end - reservation.start;

                timer.set();

                int bytesRead = readBytes(&data[reservation.start], amount);

                readOpTime += timer.elapsed_reset();
                readTotalBytes += bytesRead;
                
                if (! readOnly)
                    reservation = queue.write_reservation(reservation, bytesRead);

                readTime += timer.elapsed();
            } while (reservation.end);

        } catch (Poco::Exception& ex) {
            stopNow();
            logger << "reader exception: " << ex.displayText() << log_critical;
        } catch (std::exception& ex) {
            stopNow();
            logger << "reader exception: " << ex.what() << log_critical;
        }

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

            Timer timer;

            iobuf::reservation_t reservation = queue.read_reservation(iobuf::initial_reservation);

            writeTime += timer.elapsed();

            while (reservation.end) {
                assert(reservation.jar && reservation.jar->container.data && (reservation.start < reservation.end));
                char* data = reservation.jar->container.data;
                auto amount = reservation.end - reservation.start;

                timer.set();

                streamFile.write(data, amount);

                writeOpTime += timer.elapsed_reset();
                writeTotalBytes += amount;

                logger << "write " << amount << " bytes, " << writeTotalBytes << " total" << log_debug;

                if (! streamFile)
                    throw ReadFileException("writer error");

                reservation = queue.read_reservation(reservation);

                writeTime += timer.elapsed();
            } // end while

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
        : dataStream{socket}, streamFile(path), queue(1), logger{log}
    {
        readOnly = (path == "none");
        logger << "CopyHandler(" << socket.address().toString() << ", " << path << ')' << log_trace;
    }

    /*!
    \brief Request threads to stop
    */
    void stopNow()
    {
        queue.stop_now();
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
