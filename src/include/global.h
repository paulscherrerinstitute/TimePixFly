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
#include <string_view>
#include <memory>
#include <variant>

#include "Poco/JSON/Object.h"

#include "shared_types.h"
#include "pixel_index.h"
#include "energy_points.h"

/*!
\brief Global configuration and control data
*/
struct global final {
    // Constants
    static constexpr std::string_view no_error{"none"};                                 //!< json error string for no error

    // Callbacks
    using key_type = std::string;                                                       //!< key = path (for PUT and GET) or path?key (for GET with key)
    using put_callback = std::variant<                                                  //!< PUT(path) JSON -> string
        std::function<std::string(Poco::JSON::Object::Ptr)>,                            //!< (json-obj) -> string or
        std::function<std::string(std::istream&)>                                       //!< (istream) -> string
    >;
    using get_callback = std::function<std::string(const std::string&)>;                //!< GET(path?key) value -> string
    std::map<key_type, put_callback> put_callbacks;                                     //!< PUT callbacks
    std::map<key_type, get_callback> get_callbacks;                                     //!< GET callbacks
    using stop_handler = std::function<void()>;                                         //!< Stop somethinig gracefully
    std::vector<stop_handler> stop_handlers;                                            //!< Called by REST /?stop

    // Accessible by REST interface
    std::atomic_bool stop{false};                                                       //!< Stop processing
    std::atomic_bool start{false};                                                      //!< Start processing
    std::atomic<period_type> save_interval{131000};                                     //!< Histogram saving period: ~1s for TDC frequency 131kHz
    std::atomic<u64> TRoiStart{0};                                                      //!< Time ROI start (server mode)
    std::atomic<u64> TRoiStep{1};                                                       //!< Time ROI step (server mode)
    std::atomic<u64> TRoiN{5000};                                                       //!< Time ROI number of steps (server mode)
    // TODO: Protect these with a lock
    std::unique_ptr<PixelIndexToEp> pixel_map{nullptr};                                 //!< Area ROI
    std::string output_uri;                                                             //!< file:name (without period and .xes), or tcp:host:port

    // From CLI arguments
    bool server_mode{false};                                                            //!< Run program in server mode (from commandline arg)

    // From ASI server
    detector_layout layout;                                                             //!< Detector layout (retrieved from ASI server)

    // From code
    // TODO: Protect this with a lock
    std::string last_error;                                                             //!< Last known error

    // program state: init -> config -> setup -> collect (-> config..) -> shutdown
    static constexpr std::string_view init{"init"};
    static constexpr std::string_view config{"config"};
    static constexpr std::string_view setup{"setup"};
    static constexpr std::string_view collect{"collect"};
    static constexpr std::string_view shutdown{"shutdown"};
    std::string_view state{init};

    // Singleton
    static std::unique_ptr<global> instance;                                            //!< unique instance
};

#endif
