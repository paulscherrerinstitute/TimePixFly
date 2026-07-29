#pragma once

#ifndef XES_DATA_WRITER_H
#define XES_DATA_WRITER_H

/*!
\file
Provide writers for XES data
*/

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
        */
        virtual void write(const Data& data) = 0;

        /*!
        \brief Start data writing
        \param time_roi Time ROI
        */
        virtual void start(const TimeRoi& time_roi);

        /*!
        \brief Stop data writing
        \param error_message Error message
        */
        virtual void stop(const std::string& error_message);

        /*!
        \brief Destination string
        \return Destination string
        */
        virtual std::string dest() const = 0;

        /*!
        \brief Create writer from uri
        \param uri Output file:name (without period and .xes), tcp://host:port, or redis://host:port/key?scan-id=xxxx
        \return FileWriter or TcpWriter
        */
        static std::unique_ptr<Writer> from_uri(const std::string& uri);

        unsigned data_counter = 0u;     //!< Data packet counter
        period_type last_period = 0u;   //!< Last seen packet period
    };


} // namespace xes

#endif // ifndef XES_DATA_WRITER_H