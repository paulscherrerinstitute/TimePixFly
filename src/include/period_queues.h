#ifndef PERIOD_QUEUES_H
#define PERIOD_QUEUES_H

/*!
\file
Code for event to period assignment logic
*/

#include <memory>
#include <map>
#include <cassert>
#include "event_reordering.h"
#include "shared_types.h"

/*!
\brief Abstract period change interval representation
*/
struct period_queue_element final {
    /*!
    \brief Event reordeing queue for this period interval change

    The queue should be empty if the start time stamp (TDC event at the beginning of the period)
    has been seen. In that case `start_seen` should be true.
    */
    std::unique_ptr<event_reorder_queue> queue;
    int64_t start = 0;                          //!< The period start time stamp in number of clock ticks
    bool start_seen = false;                    //!< Either start is valid, or the queue, but not both

    inline period_queue_element()
        : queue{new event_reorder_queue{}}
    {}

    inline ~period_queue_element() = default;
    period_queue_element(const period_queue_element&) = delete;
    inline period_queue_element(period_queue_element&&) = default;              //!< Move constructor
    period_queue_element& operator=(const period_queue_element&) = delete;
    inline period_queue_element& operator=(period_queue_element&&) = default;   //!< Move assignment \return Reference to `this`
};

/*!
\brief Abstract period index
*/
struct period_index final {
    period_type period;         //!< Period number for undisputed period, lower period number for disputed period
    period_type disputed_period;//!< Higher period number for disputed period, equal to `period` if period is not disputed
    bool disputed;              //!< Is the period disputed?
};

/*!
\brief Stream output for period_index objects
\param out Output stream reference
\param idx Abstract period index
\return out
*/
template<typename Stream>
Stream& operator<<(Stream& out, const period_index& idx)
{
    out << 'p' << idx.period << (idx.disputed ? 'd' : 'u') << idx.disputed_period;
    return out;
}

#ifdef LOGGING_H
    LogProxy& operator<<(LogProxy& proxy, const period_index& idx)
    {
        return proxy.operator<<(idx);
    }
#endif

/*!
\brief Collection of recent period change interval representations
*/
struct period_queues final {
    /*!
    \brief Shortcut for the container type of the period interval change event reorder queues

    These period interval representing queues are stored with a key corresponding to the (predicted) period number.
    */
    using queue_type = std::map<period_type, period_queue_element>;

    [[gnu::const]]
    inline period_index period_index_for(double period) const noexcept
    {
        period_type p = std::floor(period);
        double f = period - p;
        if (f > 1. - threshold)
            return { p, p+1, true };
        if (f < threshold)
            return { p, p, true };
        return { p, p, false };
    }

    inline void refined_index(period_index& to_refine, int64_t time_stamp) noexcept
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

    [[gnu::pure]]
    inline period_queue_element& operator[](const period_index& idx)
    {
        return element[idx.disputed ? idx.disputed_period : idx.period];
    }

    [[gnu::pure]]
    inline period_queue_element& operator[](const period_type& period)
    {
        return element[period];
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

    [[gnu::pure]]
    inline queue_type::iterator oldest() noexcept
    {
        return std::begin(element);
    }

    [[gnu::pure]]
    inline queue_type::iterator end() noexcept
    {
        return std::end(element);
    }

    inline void erase(queue_type::iterator pos)
    {
        element.erase(pos);
    }

    [[gnu::pure]]
    inline queue_type::size_type size() const noexcept
    {
        return element.size();
    }

    [[gnu::pure]]
    inline bool empty() const noexcept
    {
        return element.empty();
    }

    queue_type element;         // key = period number
    double threshold = 0.1;     // [threshold .. (1 - threshold)] is the undisputed period attribution interval
};

#endif // PERIOD_QUEUES_H
