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
}

/*!
\brief Handler object for copying raw stream data to a file
*/
class CopyHandler final {

    StreamSocket dataStream;    //!< Raw event data stream receiving end
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
    inline int readBytes(void* buf, int size)
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
    inline void readData()
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

            int bytesRead;
            Timer timer;
            iobuf::reservation_t reservation = queue.write_reservation(iobuf::initial_reservation);

            do {
                assert(reservation.jar && reservation.jar->container.data && (reservation.start < reservation.end));
                char* data = reservation.jar->container.data;
                auto amount = reservation.end - reservation.start;

                timer.set();

                bytesRead = readBytes(&data[reservation.start], amount);

                readOpTime += timer.elapsed();
                readTotalBytes += bytesRead;
                
                logger << "read " << bytesRead << " bytes, " << readTotalBytes << " total" << log_trace;

                if (! readOnly) {
                    reservation.end = reservation.start + bytesRead;
                    reservation = queue.write_reservation(reservation);
                }

                readTime += timer.elapsed();
            } while (bytesRead);

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
    inline void writeData()
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

                writeOpTime += timer.elapsed();
                writeTotalBytes += amount;

                logger << "write " << amount << " bytes, " << writeTotalBytes << " total" << log_trace;

                if (! streamFile)
                    throw Poco::ReadFileException("writer error");

                reservation = queue.read_reservation(reservation);

                writeTime += timer.elapsed();
            } // end while

        } catch (Poco::Exception& ex) {
            stopNow();
            logger << "writer exception: " << ex.displayText() << log_critical;
        } catch (std::exception& ex) {
            stopNow();
            logger << "writer exception: " << ex.what() << log_critical;
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
    inline CopyHandler(const std::string& path, Logger& log)
        : queue(1), logger{log}
    {
        logger << "CopyHandler(" << path << ')' << log_trace;
        if (! (readOnly = (path == "none")))
            streamFile = std::ofstream(path);
        else
            logger << "CopyHandler running in read only mode" << log_debug;
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
    \brief Request threads to stop
    */
    inline void stopNow()
    {
        queue.stop_now();
    }

    /*!
    \brief Start worker threads for reading and writing of raw event data
    */
    inline void run_async()
    {
        readerThread = std::thread([this]{this->readData();});
        if (! readOnly)
            writerThread = std::thread([this]{this->writeData();});
    }

    /*!
    \brief Wait for completion of threads for reading and writing raw event data
    */
    inline void await()
    {
        readerThread.join();
        if (! readOnly)
            writerThread.join();
    }

    /*!
    \brief Log output
    \param time Total wall time
    */
    inline void logOutput(double time) {
        const auto items = writeTotalBytes / sizeof(u64);
        const auto ri = readTotalBytes / sizeof(u64);
        const auto wi = writeTotalBytes / sizeof(u64);
        const auto rt = readTime;
        const auto rot = readOpTime;
        const auto wt = writeTime;
        const auto wot = writeOpTime;

        logger << "total: " << items << " items in " << time << "s at " << (items / time) << " items/s\n"
               << "read: " << ri << " items in " << rt << "s at " << (ri / rt) << " items/s, op: " << rot << "s at " << (ri / rot) << " items/s\n"
               << "write: " << items << " items in " << wt << "s at " << (wi / wt) << " items/s, op: " << wot << "s at " << (wi / wot) << " items/s" << log_notice;
    }

    double readOpTime = .0;     //!< Time used for synchronous read operations
    double readTime = .0;       //!< Time used by raw event data reading thread
    double writeTime = .0;      //!< Time used by raw event data writing thread
    double writeOpTime = .0;    //!< Time used for write operation
    u64 readTotalBytes = 0ul;   //!< Total bytes read
    u64 writeTotalBytes = 0ul;  //!< Total bytes written
};

#endif // COPY_HANDLER_H
