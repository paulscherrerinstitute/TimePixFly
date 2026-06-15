
#pragma once

#ifndef TIMING_H
#define TIMING_H

/*!
\file
Provide means to measure elapsed time
*/

#include <mutex>

/*!
\brief Timer clock
*/
class Timer final {
    public:
    using clock = std::chrono::high_resolution_clock;   //!< Clock type
    using time_point = clock::time_point;               //!< Time point type

    private:
    time_point start;   //!< Start time

    public:
    /*!
    \brief Constructor
    \param start_ Start time
    */
    inline explicit Timer(const time_point& start_=clock::now()) noexcept
        : start{start_}
    {}

    inline Timer(const Timer&) = default;              //!< Copy constructor
    inline Timer(Timer&&) = default;                   //!< Move constructor

    /*!
    \brief Assignment
    \return this
    */
    inline Timer& operator=(const Timer&) noexcept = default;

    /*!
    \brief Move assignment
    \return this
    */
    inline Timer& operator=(Timer&&) noexcept = default;

    inline ~Timer() = default;                         //!< Destructor

    /*!
    \brief Elapsed time since timer start
    \return Elapsed time
    */
    inline double elapsed() const noexcept
    {
        return std::chrono::duration<double>{clock::now() - start}.count();
    }

    /*!
    \brief Set start time
    \param start_ New start time
    */
    inline void set(const time_point& start_=clock::now()) noexcept
    {
        start = start_;
    }

    /*!
    \brief Elapsed time since start and reset start to now
    \return Elapsed time
    */
    inline double elapsed_reset() noexcept
    {
        auto now = clock::now();
        double duration = std::chrono::duration<double>{now - start}.count();
        start = now;
        return duration;
    }
};

// /*!
// \brief Per thread time aggregator agent
// */
// template<class Agg>
// class TimeAggregatorAgent final {
//     Agg* aggregator = nullptr;      //!< Aggregator
//     Timer local_timer;              //!< Local timer
//     double locally_aggregated=0.0;  //!< Locally aggregated value

//     public:
//     /*!
//     \brief Constructor
//     \param aggregator_ Create agent for this aggregator
//     */
//     inline explicit TimeAggregatorAgent(Agg& aggregator_) noexcept
//         : aggregator(&aggregator_)
//     {}

//     TimeAggregatorAgent(const TimeAggregatorAgent&) = delete;

//     /*!
//     \brief Move constructor
//     \param other Value to be moved into this
//     */
//     inline TimeAggregatorAgent(TimeAggregatorAgent&& other) noexcept
//     {
//         std::swap(*this, other);
//     }

//     TimeAggregatorAgent& operator=(const TimeAggregatorAgent&) = delete;

//     /*!
//     \brief Move assignment
//     \param other Value to be moved into this
//     \return this
//     */
//     inline TimeAggregatorAgent& operator=(TimeAggregatorAgent&& other) noexcept
//     {
//         aggregator = nullptr;
//         std::swap(*this, other);
//         return *this;
//     }

//     /*!
//     \brief Destructor
//     Pass locally aggregated value to aggregator.
//     */
//     inline ~TimeAggregatorAgent() noexcept
//     {
//         if (aggregator)
//             aggregator->add(locally_aggregated);
//     }

//     /*!
//     \brief Set timer start
//     */
//     inline void set() noexcept
//     {
//         local_timer.set();
//     }

//     /*!
//     \brief Aggregate elapsed time
//     */
//     inline void add() noexcept
//     {
//         locally_aggregated += local_timer.elapsed();
//     }

//     /*!
//     \brief Reset locally aggregated value
//     */
//     inline void reset() noexcept
//     {
//         locally_aggregated = 0.0;
//     }
// };

// /*!
// \brief Aggregator for elapsed time
// */
// class TimeAggregator final {
//     std::mutex add_lock;    //!< Protect aggregated value
//     double aggregated=0.0;  //!< Aggregated value

//     public:
//     inline TimeAggregator() noexcept = default;
//     TimeAggregator(const TimeAggregator&) = delete;
//     TimeAggregator(TimeAggregator&&) = delete;
//     TimeAggregator& operator=(const TimeAggregator&) = delete;
//     TimeAggregator& operator=(TimeAggregator&&) = delete;
//     inline ~TimeAggregator() = default;

//     /*!
//     \brief Aggregate value
//     \param value Value to be aggregated
//     */
//     inline void add(double value)
//     {
//         std::lock_guard lock{add_lock};
//         aggregated += value;
//     }

//     /*!
//     \brief Reset aggregated value
//     */
//     inline void reset()
//     {
//         aggregated = 0.0;
//     }

//     /*!
//     \brief Get per thread agent for this aggregator
//     \return Agent for this aggregator
//     */
//     inline TimeAggregatorAgent<TimeAggregator> agent() noexcept
//     {
//         return TimeAggregatorAgent{*this};
//     }
// };

#endif
