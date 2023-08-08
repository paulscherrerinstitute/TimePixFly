#ifndef LOGGING_H
#define LOGGING_H

#include <sstream>
#include "Poco/Logger.h"

namespace {
    using Poco::Logger;
    using Poco::Message;
}

// A fatal error. The application will most likely terminate. This is the highest priority.
[[maybe_unused]] constexpr Message::Priority log_fatal = Message::PRIO_FATAL;

// A critical error. The application might not be able to continue running successfully.
[[maybe_unused]] constexpr Message::Priority log_critical = Message::PRIO_CRITICAL;

// An error. An operation did not complete successfully, but the application as a whole is not affected.
[[maybe_unused]] constexpr Message::Priority log_error = Message::PRIO_ERROR;

// A warning. An operation completed with an unexpected result.
[[maybe_unused]] constexpr Message::Priority log_warn = Message::PRIO_WARNING;

// A notice, which is an information with just a higher priority.
[[maybe_unused]] constexpr Message::Priority log_notice = Message::PRIO_NOTICE;

// An informational message, usually denoting the successful completion of an operation.
[[maybe_unused]] constexpr Message::Priority log_info = Message::PRIO_INFORMATION;

// A debugging message.
[[maybe_unused]] constexpr Message::Priority log_debug = Message::PRIO_DEBUG;

// A tracing message. This is the lowest priority.
[[maybe_unused]] constexpr Message::Priority log_trace = Message::PRIO_TRACE;

struct LogProxy final : public std::ostringstream {
    using base_type = std::ostringstream;
    Logger& logger;

    inline LogProxy(Logger& l)
        : logger(l)
    {}

    inline LogProxy(LogProxy&& other)
        : logger(other.logger)
    {}

    LogProxy(const LogProxy&) = delete;
    LogProxy& operator=(const LogProxy&) = delete;
    LogProxy& operator=(LogProxy&&) = delete;

    inline virtual ~LogProxy() {}

    template<typename T>
    inline LogProxy& operator<< (const T& value)
    {
        static_cast<LogProxy::base_type&>(*this) << value;
        return *this;
    }

    inline LogProxy& operator<< (const Message::Priority& priority)
    {
        logger.log(Message("Tpx3App", str(), priority));
        str("");
        return *this;
    }
};

template<typename T>
inline LogProxy operator<< (Logger& logger, const T& value)
{
    LogProxy proxy(logger);
    proxy << value;
    return proxy;
}

#endif // LOGGING_H