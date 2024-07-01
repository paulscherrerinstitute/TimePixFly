#pragma once

#ifndef LOGGING_H
#define LOGGING_H

/*!
\file
Logging code
*/

#include <sstream>
#include "Poco/Logger.h"

namespace {
    using Poco::Logger;
    using Poco::Message;
}

/*! A fatal error. The application will most likely terminate. This is the highest priority. */
[[maybe_unused]] constexpr Message::Priority log_fatal = Message::PRIO_FATAL;

/*! A critical error. The application might not be able to continue running successfully. */
[[maybe_unused]] constexpr Message::Priority log_critical = Message::PRIO_CRITICAL;

/*! An error. An operation did not complete successfully, but the application as a whole is not affected. */
[[maybe_unused]] constexpr Message::Priority log_error = Message::PRIO_ERROR;

/*! A warning. An operation completed with an unexpected result. */
[[maybe_unused]] constexpr Message::Priority log_warn = Message::PRIO_WARNING;

/*! A notice, which is an information with just a higher priority. */
[[maybe_unused]] constexpr Message::Priority log_notice = Message::PRIO_NOTICE;

/*! An informational message, usually denoting the successful completion of an operation. */
[[maybe_unused]] constexpr Message::Priority log_info = Message::PRIO_INFORMATION;

/*! A debugging message. */
[[maybe_unused]] constexpr Message::Priority log_debug = Message::PRIO_DEBUG;

/*! A tracing message. This is the lowest priority. */
[[maybe_unused]] constexpr Message::Priority log_trace = Message::PRIO_TRACE;

/*!
\brief Proxy object for a Poco Logger object
*/
struct LogProxy final : private std::ostringstream {
    using base_type = std::ostringstream;   //!< Shorthand for base type
    Logger& logger;                         //!< Poco::Logger object that is proxied

    /*!
    \brief Constructor
    \param l Poco::Logger object that is proxied
    */
    inline LogProxy(Logger& l)
        : logger(l)
    {}

    /*!
    \brief Move constructor
    \param other Will be moved into `this`
    */
    inline LogProxy(LogProxy&& other)
        : logger(other.logger)
    {}

    LogProxy(const LogProxy&) = delete;
    LogProxy& operator=(const LogProxy&) = delete;
    LogProxy& operator=(LogProxy&&) = delete;

    inline virtual ~LogProxy() {}   //!< Virtual destructor

    /*!
    \brief Output operator

    The logging output is not written to `logger`right away.
    The output is saved away in a `std::ostringstream` here.

    \param value Value to print out into the logging stream
    \return Reference to `this`
    */
    template<typename T>
    inline LogProxy& operator<< (const T& value)
    {
        static_cast<LogProxy::base_type&>(*this) << value;
        return *this;
    }

    /*!
    \brief Log saved output
    
    The saved output is written to `logger` with the given priority.

    \param priority Poco logging priority
    \return Reference to `this`
    */
    inline LogProxy& operator<< (const Message::Priority& priority)
    {
        logger.log(Message("Tpx3App", str(), priority));
        str("");
        return *this;
    }

    /*!
    \brief Get reference to base object
    \return Reference to base object
    */
    base_type& base() noexcept
    {
        return *this;
    }

    /*!
    \brief Query for debug log level or higher
    Is the log level of `logger` at least debug?
    \return True if debug priority log messages will be visible in the log stream
    */
    bool debug() const {
        return logger.debug();
    }
};

/*!
\brief Operator for initial logging operation
This will return a LogProxy object that collects output until it sees the log priority input (see LogProxy).

Example:
Poco::Logger log;
log << "my new log entry: " << "test" << log_info;
     |                       |         |
collect and return LogProxy  collect   forward collected output to log with PRIO_INFORMATION

\param logger Poco logger object reference. Output will eventually be forwarded to this logger.
\param value  Output value reference
\return LogProxy object
*/
template<typename T>
inline LogProxy operator<< (Logger& logger, const T& value)
{
    LogProxy proxy(logger);
    proxy << value;
    return proxy;
}

#endif // LOGGING_H