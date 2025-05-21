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
#include "pixel_index.h"
#include "energy_points.h"

/*!
\brief Global configuration and control data
*/
struct global final {
    using key_type = std::string;                                                       //!< key = path (for PUT) or path/key (for GET)
    using put_callback = std::function<std::string(Poco::JSON::Object::Ptr)>;           //!< PUT(path) JSON -> string
    using get_callback = std::function<std::string(const std::string&)>;                //!< GET(path+key) value -> string
    std::map<key_type, put_callback> put_callbacks;                                     //!< PUT callbacks
    std::map<key_type, get_callback> get_callbacks;                                     //!< GET callbacks

    std::atomic_bool stop{false};                                                       //!< Stop processing
    std::atomic_bool start{false};                                                      //!< Start processing
    std::atomic<period_type> save_interval{131000};                                     //!< Histogram saving period: ~1s for TDC frequency 131kHz
    std::atomic<u64> TRoiStart{0};                                                      //!< Time ROI start (server mode)
    std::atomic<u64> TRoiStep{1};                                                       //!< Time ROI step (server mode)
    std::atomic<u64> TRoiN{5000};                                                       //!< Time ROI number of steps (server mode)
    std::atomic<PixelIndexToEp*> pixel_map{nullptr};                                    //!< Area ROI

    bool server_mode{false};                                                            //!< Run program in server mode (from commandline arg)
    detector_layout layout;                                                             //!< Detector layout (retrieved from ASI server)

    static std::unique_ptr<global> instance;                                            //!< unique instance
};

#endif
