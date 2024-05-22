
#ifndef TIMING_H
#define TIMING_H

/*!
\file
Provide means to measure elapsed time
*/

#include <chrono>
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
    explicit Timer(const time_point& start_=clock::now()) noexcept
        : start{start_}
    {}

    Timer(const Timer&) = default;              //!< Copy constructor
    Timer(Timer&&) = default;                   //!< Move constructor

    /*!
    \brief Assignment
    \return this
    */
    Timer& operator=(const Timer&) = default;
    
    /*!
    \brief Move assignment
    \return this
    */
    Timer& operator=(Timer&&) = default;
    
    ~Timer() = default;                         //!< Destructor

    /*!
    \brief Elapsed time since timer start
    \return Elapsed time
    */
    double elapsed() const noexcept
    {
        return std::chrono::duration<double>{clock::now() - start}.count();
    }

    /*!
    \brief Set start time
    \param start_ New start time
    */
    void set(const time_point& start_=clock::now()) noexcept
    {
        start = start_;
    }
};

/*!
\brief Per thread time aggregator agent
*/
template<class Agg>
class TimeAggregatorAgent final {
    Agg* aggregator = nullptr;      //!< Aggregator
    Timer local_timer;              //!< Local timer
    double locally_aggregated=0.0;  //!< Locally aggregated value

    public:
    /*!
    \brief Constructor
    \param aggregator_ Create agent for this aggregator
    */
    TimeAggregatorAgent(Agg& aggregator_) noexcept
        : aggregator(&aggregator_)
    {}

    TimeAggregatorAgent(const TimeAggregatorAgent&) = delete;

    /*!
    \brief Move constructor
    \param other Value to be moved into this
    */
    TimeAggregatorAgent(TimeAggregatorAgent&& other)
    {
        std::swap(*this, other);
    }

    TimeAggregatorAgent& operator=(const TimeAggregatorAgent&) = delete;

    /*!
    \brief Move assignment
    \param other Value to be moved into this
    \return this
    */
    TimeAggregatorAgent& operator=(TimeAggregatorAgent&& other)
    {
        aggregator = nullptr;
        std::swap(*this, other);
    }

    /*!
    \brief Destructor
    Pass locally aggregated value to aggregator.
    */
    ~TimeAggregatorAgent()
    {
        if (aggregator)
            aggregator->add(locally_aggregated);
    }

    /*!
    \brief Set timer start
    */
    void set() noexcept
    {
        local_timer.set();
    }

    /*!
    \brief Aggregate elapsed time
    */
    void add() noexcept
    {
        locally_aggregated += local_timer.elapsed();
    }

    /*!
    \brief Reset locally aggregated value
    */
    void reset() noexcept
    {
        locally_aggregated = 0.0;
    }
};

/*!
\brief Aggregator for elapsed time
*/
class TimeAggregator final {
    std::mutex add_lock;    //!< Protect aggregated value
    double aggregated=0.0;  //!< Aggregated value

    public:
    TimeAggregator() = default;
    TimeAggregator(const TimeAggregator&) = delete;
    TimeAggregator(TimeAggregator&&) = delete;
    TimeAggregator& operator=(const TimeAggregator&) = delete;
    TimeAggregator& operator=(TimeAggregator&&) = delete;
    ~TimeAggregator() = default;

    /*!
    \brief Aggregate value
    \param value Value to be aggregated
    */
    void add(double value)
    {
        std::unique_lock lock(add_lock);
        aggregated += value;
    }

    /*!
    \brief Reset aggregated value
    */
    void reset()
    {
        aggregated = 0.0;
    }

    /*!
    \brief Get per thread agent for this aggregator
    \return Agent for this aggregator
    */
    TimeAggregatorAgent<TimeAggregator> agent()
    {
        return TimeAggregatorAgent{*this};
    }
};

#endif
