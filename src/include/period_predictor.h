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
    static constexpr double extrapolation_threshold = 100.;
    static constexpr int N = 4;
    std::array<int64_t, N> past;    // time stamp
    int64_t start;
    double interval;
    long correction;
    unsigned first = 0;

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

    inline period_predictor(int64_t start_, int64_t period) noexcept
    {
        reset(start_, period);
    }

    ~period_predictor() = default;

    inline double interval_prediction() const noexcept
    {
        return interval;
    }

    inline double period_prediction(int64_t ts) const noexcept
    {
        return (ts - start) / interval + correction;
    }

    inline void prediction_update(int64_t ts) noexcept
    {
        past[first] = ts;
        first = (first + 1) % N;
        interval = predict_interval();
    }

    // set new start time and recalculate correction
    inline void start_update(int64_t start_) noexcept
    {
        correction += std::lround((start_ - start) / interval);
        start = start_;
    }

    inline void reset(int64_t start_, int64_t period) noexcept
    {
        start = start_;
        interval = period;
        for (int i=0; i<N; i++)
            past[N-i-1] = start - i * interval;
        correction = 0;
    }

    inline unsigned minPoints() const noexcept
    {
        return (N + 2) / 2;
    }

    bool ok(int64_t ts) const noexcept
    {
        return ((ts - start) / interval) < extrapolation_threshold;
    }

    template<typename Stream>
    inline void print_to(Stream& out) const
    {
        out << "ts: ";
        for (const auto& dp : past)
            out << dp << ' ';
        out << 's' << start << " i" << interval << " c" << correction << " f" << first;
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