#ifndef PERIOD_PREDICTOR_H
#define PERIOD_PREDICTOR_H

/*!
\file
Code for maintaining a period prediction

Both the period interval and number can be predicted
*/

#include <algorithm>
#include <cmath>

/*!
\brief Period predictor object
*/
class period_predictor final {
    static constexpr double extrapolation_threshold = 100.; //!< Don't extrapolate past theis threshold
    static constexpr int N = 4;     //!< Number of past TDC time points stored
    std::array<int64_t, N> past;    //!< Storage for past TDC time time stamps. `first`points to the most recent time stamp.
    int64_t start;                  //!< Base reference time point in clock ticks
    double interval;                //!< Period interval prediction in clock ticks
    long correction;                //!< Correction factor: distance in number of periods between 0 and `start`
    unsigned first = 0;             //!< Index of first time stamp in `past`

    /*!
    \brief Calculate period interval prediction

    The prediction is the median of the intervals between the TDC time points.

    \return Interval prediction in number of clock ticks
    */
    inline double predict_interval() const noexcept
    {
        std::array<double, N-1> diff;
        for (unsigned i=0; i<N-1; i++) {
            const unsigned l = (first + i) % N;
            const unsigned h = (l + 1) % N;
            diff[i] = past[h] - past[l];
        }
        std::sort(std::begin(diff), std::end(diff));
        return diff[(N-1) / 2];
    }

  public:
    inline period_predictor() noexcept
    {
        reset(0, 1);
    }

    /*!
    \brief Constructor
    \param start_   Starting clock tick
    \param period   Period in number of clock ticks
    */
    inline period_predictor(int64_t start_, int64_t period) noexcept
    {
        reset(start_, period);
    }

    ~period_predictor() = default;

    /*!
    \brief Get predicted interval
    \return Period in number of clock ticks
    */
    inline double interval_prediction() const noexcept
    {
        return interval;
    }

    /*!
    \brief Get predicted period
    \param ts Time in clock ticks
    \return Predicted period number
    */
    inline double period_prediction(int64_t ts) const noexcept
    {
        return (ts - start) / interval + correction;
    }

    /*!
    \brief Update period prediction with new data
    \param ts Time of TDC event in clock ticks
    */
    inline void prediction_update(int64_t ts) noexcept
    {
        past[first] = ts;
        first = (first + 1) % N;
        interval = predict_interval();
    }

    /*!
    \brief Set new start time and recalculate correction

    To minimize period number prediction errors, the reference time
    for prediction calculations has to be moved forward in time regularly.

    \param start_ New time base reference in clock ticks
    */
    inline void start_update(int64_t start_) noexcept
    {
        correction += std::lround((start_ - start) / interval);
        start = start_;
    }

    /*!
    \brief Reset the period predictor
    \param start_ New base reference time in clock ticks
    \param period Initial period interval in clock ticks
    */
    inline void reset(int64_t start_, int64_t period) noexcept
    {
        start = start_;
        interval = period;
        for (int i=0; i<N; i++)
            past[N-i-1] = start - i * interval;
        correction = 0;
        first = 0;
    }

    /*!
    \brief Number of TDC time points remembered by the period predictor
    \return Number of TDC time points remembered by the period predictor
    */
    static inline unsigned numPoints() noexcept
    {
        return N;
    }

    /*!
    \brief Minimum number of TDC time points required for reliable period prediction
    \return Minimum number of times `prediction_update()` should be called before the predictor is reliable
    */
    static inline unsigned minPoints() noexcept
    {
        return (N + 2) / 2;
    }

    /*!
    \brief Query if `start_update()` should be called

    If the base reference time is too old, the predictor gets unreliable.

    \param ts Time stamp in clock ticks for which to check prediction reliability
    \return False if `start_update()` should be called as soon as possible
    */
    bool ok(int64_t ts) const noexcept
    {
        return ((ts - start) / interval) < extrapolation_threshold;
    }

    /*!
    \brief Period predictor printing
    \param out Output stream for printing a human readable period predictor representation
    */
    template<typename Stream>
    inline void print_to(Stream& out) const
    {
        out << "ts: ";
        for (const auto& dp : past)
            out << dp << ' ';
        out << 's' << start << " i" << interval << " c" << correction << " f" << first;
    }

    /*!
    \brief Period predictor string representation
    \return String in the form ts: <time stamps ..> s<start> i<interval> c<correction> f<first>
    */
    std::string to_string() const
    {
        std::string rval("ts: ");
        for (const auto& dp : past)
            rval += std::to_string(dp) + ' ';
        rval += 's'; rval += std::to_string(start);
        rval += " i"; rval += std::to_string(interval);
        rval += " c"; rval += std::to_string(correction);
        rval += " f"; rval += std::to_string(first);
        return rval;
    }
};

/*!
\brief Stream output for period predictor
\param out Output stream reference
\param p   Period predictor
\return out
*/
template<typename Stream>
Stream& operator<<(Stream& out, const period_predictor& p)
{
    p.print_to(out);
    return out;
}

#ifdef LOGGING_H
    LogProxy& operator<<(LogProxy& proxy, const period_predictor& p)
    {
        return proxy.operator<<(p);
    }
#endif

#endif // PERIOD_PREDICTOR_H