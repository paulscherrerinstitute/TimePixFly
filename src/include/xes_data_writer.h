#pragma once

#ifndef XES_DATA_WRITER_H
#define XES_DATA_WRITER_H

/*!
\file
Provide writers for XES data
*/

#include "shared_types.h"
#include "xes_data.h"

namespace xes {

    /*!
    \brief Common type for all writers
    */
    struct Writer {
        virtual ~Writer() = 0;  //!< Destructor

        /*!
        \brief Write data for period
        \param data XES Data
        \param period Which period
        */
        virtual void write(const Data& data, period_type period) = 0;

        /*!
        \brief Start data writing
        \param detector Detector
        */
        virtual void start(const Detector& detector);

        /*!
        \brief Stop data writing
        \param error_message Error message
        */
        virtual void stop(const std::string& error_message);

        /*!
        \brief Destination string
        \return Destination string
        */
        virtual std::string dest() = 0;

        /*!
        \brief Create writer from uri
        \param uri Output file:name (without period and .xes), or tcp:host:port
        \return FileWriter or TcpWriter
        */
        static std::unique_ptr<Writer> from_uri(const std::string& uri);
    };


} // namespace xes

#endif // ifndef XES_DATA_WRITER_H