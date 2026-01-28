#pragma once

#ifndef SHARED_TYPES_H
#define SHARED_TYPES_H

/*!
\file
Provide types shared accross the source files
*/

#include <cstdint>

using u8 = uint8_t;             //!< Unsigned 8 bit integer
using u16 = uint16_t;           //!< Unsigned 16 bit integer
using u64 = uint64_t;           //!< Unsigned 64 bit integer

using period_type = int64_t;    //!< Type for representing a period

/*!
\brief TOA event type
*/
struct alignas(u64) toa_event {
    u64 ts: 48;     //!< Time stamp
    u64 px: 16;     //!< Flat pixel within chip
};
static_assert(sizeof(toa_event) == sizeof(u64));

#endif
