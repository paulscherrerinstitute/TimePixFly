#ifndef PERIOD_QUEUES_H
#define PERIOD_QUEUES_H

#include <memory>
#include <map>
#include "event_reordering.h"

using period_type = int64_t;

struct period_queue_element final {
    std::unique_ptr<event_reorder_queue> queue;
    int64_t start = 0;
    bool start_seen = false;    // either start is valid, or the queue, but not both

    inline period_queue_element()
        : queue{new event_reorder_queue{}}
    {}

    inline ~period_queue_element() = default;
    period_queue_element(const period_queue_element&) = delete;
    inline period_queue_element(period_queue_element&&) = default;
    period_queue_element& operator=(const period_queue_element&) = delete;
    inline period_queue_element& operator=(period_queue_element&&) = default;
};

struct period_index final {
    period_type period;
    period_type disputed_period;
    bool disputed;
};

struct period_queues final {
    using queue_type = std::map<period_type, period_queue_element>;

    [[gnu::const]]
    inline period_index period_index_for(double period)
    {
        double f = period - std::floor(period);
        period_type p = std::lround(period);
        if (f > 1. - threshold)
            return { p, p+1, true };
        if (f < threshold)
            return { p, p, true };
        return { p, p, false };
    }

    inline void refined_index(period_index& to_refine, int64_t time_stamp)
    {
        if (! to_refine.disputed)
            return;

        const auto pqe_ptr = element.find(to_refine.disputed_period);
        if (pqe_ptr == std::end(element))
            return;

        const auto& pqe = pqe_ptr->second;
        if (! pqe.start_seen)
            return;
        
        to_refine.disputed = false;

        if (to_refine.period == to_refine.disputed_period) {    // disputed at start
            if (pqe.start > time_stamp)
                to_refine.period -= 1;
        } else {                                                // disputed at end
            if (pqe.start <= time_stamp)
                to_refine.period += 1;
        }

        return;
    }

    inline period_queue_element& operator[](const period_index& idx)
    {
        return element[idx.disputed ? idx.disputed_period : idx.period];
    }

    inline event_reorder_queue& registerStart(const period_index& idx, int64_t start)
    {
        assert(idx.disputed);
        auto& pqe = (*this)[idx];
        assert(! pqe.start_seen);
        pqe.start = start;
        pqe.start_seen = true;
        return *pqe.queue;
    }

    inline queue_type::iterator oldest()
    {
        return std::begin(element);
    }

    inline void erase(queue_type::iterator pos)
    {
        element.erase(pos);
    }

    queue_type element;   // key = period number
    double threshold = 0.1;                         // (1 - threshold) is the undisputed period attribution interval
};

#endif // PERIOD_QUEUES_H
