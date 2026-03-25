#pragma once

#ifndef EVENT_TYPE_H
#define EVENT_TYPE_H

/*!
\file
Provide event type for analysis code
*/

#include "shared_types.h"

/*!
\brief Event type
*/
struct alignas(u64) event_t final {
    u64 ts:47;      //!< Timestamp
    u64 is_tdc: 1;  //!< Flag 1=TDC, 0=TOA
    u64 px: 16;     //!< Flat pixel

    /*!
    \brief Check for validity
    Invalid events have all zero bits
    \param event Event
    \return true iff valid
    */
    [[gnu::const]]
    inline static bool valid(const event_t& event) noexcept
    {
        return *(u64*)&event != u64{0};
    }
};

static_assert(sizeof(event_t) == sizeof(u64));

/*!
\brief Heap ordering
Make smaller the higher priority
\param a First operand
\param b Second operand
\return True if and only if a > b
*/
[[gnu::const]]
inline bool operator<(const event_t& a, const event_t& b) noexcept
{
    return a.ts > b.ts;
}

/*!
\brief Inequality
\param a First operand
\param b Second operand
\return True if and only if a == b
*/
[[gnu::const]]
inline bool operator!=(const event_t&a, const event_t& b) noexcept
{
    return *(u64*)&a != *(u64*)&b;
}

#endif
