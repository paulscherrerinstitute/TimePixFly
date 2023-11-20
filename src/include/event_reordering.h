#ifndef EVENT_REORDERING_H
#define EVENT_REORDERING_H

/*!
Code for event reordering based on event TOA
*/

#include "reorder_queue.h"

/*!
\brief Reorder element representation for one TOA event
*/
struct reordering_element final {
    int64_t toa;    // priority
    uint64_t event; // full event

    inline reordering_element(int64_t toa_, uint64_t event_) noexcept
        : toa{toa_}, event{event_}
    {}

    inline ~reordering_element() = default;
    inline reordering_element(const reordering_element&) = default;
    inline reordering_element(reordering_element&&) = default;
    inline reordering_element& operator=(const reordering_element&) = default;
    inline reordering_element& operator=(reordering_element&&) = default;
};

/*!
\brief Reordering element priority comparator based on TOA
*/
struct toa_older_comparator final {
    inline bool operator()(const reordering_element& lhs, const reordering_element& rhs) const noexcept
    {
        return lhs.toa > rhs.toa;
    }
};

using event_reorder_queue = reorder_queue<reordering_element, toa_older_comparator>;

#endif // EVENT_REORDERING_H
