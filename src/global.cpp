/*!
\file
Provide static instance for global configuration and control data
*/

#include <mutex>
#include "global.h"

namespace {
    std::mutex error_lock;  //!< protect last_error
}

std::unique_ptr<global> global::instance{new global};

void global::set_error(const std::string& error)
{
    std::lock_guard lock(error_lock);
    instance->last_error = error;
}
