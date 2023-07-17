#ifndef PERIOD_PREDICTOR_H
#define PERIOD_PREDICTOR_H

// Code for maintaining a period prediction
// Both the interval and the period number can be predicted


#include <algorithm>
#include <cmath>

class period_predictor final {
    struct data_point final { int64_t ts; double p; };
    static constexpr unsigned N = 4;
    std::array<data_point, N> past;  // time stamp, period
    int64_t start;
    double interval;
    long correction = 0;
    unsigned first = 0;

    inline double predict_interval() const noexcept
    {
        std::array<double, N-1> diff;
        for (unsigned i=0; i<N; i++) {
            const unsigned l = (first + i) % (N-1);
            const unsigned h = (l + 1) % (N-1);
            diff[i] = (past[h].ts - past[l].ts) / (past[h].p - past[l].p);
        }
        std::sort(&diff[0], &diff[N-1]);
        return diff[(N-1) / 2];
    }

  public:
    inline period_predictor(int64_t start_, int64_t period) noexcept
    {
        start = start_;
        interval = period;
        reset();
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
        double period = std::round(period_prediction(ts));
        past[first] = { ts, period };
        first = (first + 1) % N;
        interval = predict_interval();
    }

    // set new start time and recalculate correction
    inline void start_update(int64_t start_) noexcept
    {
        correction += std::lround((start_ - start) / interval);
        start = start_;
        interval = predict_interval();
    }

    inline void reset()
    {
        for (unsigned i=0; i<N; i++)
            past[i] = { std::lround(start - i * interval), -(double)i };
        correction = 0;
    }
};

#endif // PERIOD_PREDICTOR_H