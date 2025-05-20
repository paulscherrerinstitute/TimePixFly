#pragma once

#ifndef GLOBAL_H
#define GLOBAL_H

/*!
\file
Global configuration and control data
*/

#include <utility>
#include <functional>
#include <string>
#include <memory>

#include "Poco/JSON/Object.h"

#include "shared_types.h"

// Global configuration and control data
struct global final {
    using key_type = std::string;                                                       //!< key = path (for PUT) or path/key (for GET)
    using put_callback = std::function<std::string(Poco::JSON::Object::Ptr)>;           //!< PUT(path) JSON -> string
    using get_callback = std::function<std::string(const std::string&)>;                //!< GET(path+key) value -> string
    std::map<key_type, put_callback> put_callbacks;                                     //!< PUT callbacks
    std::map<key_type, get_callback> get_callbacks;                                     //!< GET callbacks

    std::atomic_bool stop{false};                                                       //!< stop processing
    period_type save_interval{131000};                                                  //!< Histogram saving period: ~1s for TDC frequency 131kHz

    static std::unique_ptr<global> instance;                                            //!< unique instance
};

#endif
