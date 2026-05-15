/*!
\file
Provide static instance for global configuration and control data
*/

#include <mutex>
#include "global.h"

namespace {
    std::mutex error_lock;  //!< protect last_error
    std::mutex config_lock; //!< protect configuration
}

std::unique_ptr<global> global::instance{new global};

void global::set_error(const std::string& error)
{
    std::lock_guard lock(error_lock);
    instance->last_error = error;
}

std::lock_guard<std::mutex> global::configLock()
{
    return std::lock_guard{config_lock};
}
