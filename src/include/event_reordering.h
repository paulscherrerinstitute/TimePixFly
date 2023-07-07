#ifdef EVENT_REORDERING_H
#define EVENT_REORDERING_H

// Provide event reordering based on event TOA

#include "reorder_queue.h"

// What is reordered
struct reordering_element final {
    uint64_t toa;   // priority
    uint64_t event; // full event

    [[gnu::const]]
    inline reordering_element(uint64_t toa_, uint64_t event_) noexcept
        : toa{toa_}, event{event_}
    {}
};

// TOA priority comparator
struct toa_older_comparator final {
    inline bool operator()(const reordering_element& lhs, const reordering_element& rhs) noexcept
    {
        return lhs.toa > rhs.toa;
    }
};

using event_reorder_queue = reorder_queue<reordering_element, toa_older_comparator>;

#endif // EVENT_REORDERING_H
