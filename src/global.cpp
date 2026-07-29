/*!
\file
Provide static instance for global configuration and control data
*/

#include <mutex>
#include "global.h"

namespace {
    std::mutex error_lock;  //!< protect last_error
    std::string last_error; //!< Last known error

    std::mutex config_lock; //!< protect configuration
}

std::unique_ptr<global> global::instance = std::make_unique<global>();

void global::set_error(const std::string& error) noexcept
{
    std::lock_guard lock(error_lock);
    last_error = error;
}

std::string global::get_error(bool reset) noexcept
{
    std::string error;
    {
        std::lock_guard lock(error_lock);
        if (reset)
            std::swap(last_error, error);
        else
            error = last_error;
    }
    return error;
}

bool global::error_empty() noexcept
{
    return last_error.empty();
}

std::lock_guard<std::mutex> global::configLock()
{
    return std::lock_guard{config_lock};
}
