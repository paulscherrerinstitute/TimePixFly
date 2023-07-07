#ifndef PERIOD_PREDICTOR_H
#define PERIOD_PREDICTOR_H

#include <algorithm>

class period_predictor final {
    static constexpr unsigned N = 4;
    std::array<struct{ int64_t ts, double p }, N> past;  // time stamp, period
    double current;
    unsigned first = 0;

    int64_t predict_period() const noexcept
    {
        std::array<double, N-1> interval;
        for (unsigned i=0; i<N; i++) {
            const unsigned l = (first + i) % (N-1);
            const unsigned h = (l + 1) % (N-1);
            interval[i] = (past[h].ts - past[l].ts) / (past[h].p - past[l].p);
        }
        std::sort(&interval[0], &interval[N-1]);
        return interval[(N-1) / 2];
    }

  public:
    period_predictor(int64_t start, int64_t period) noexcept
    {
        for (unsigned i=0; i<N; i++)
            time_stamp[i] = { start + i * period, i };
        current = predict_period();
    }

    double interval_prediction() const noexcept
    {
        return current;
    }

    double period_prediction(int64_t start, int64_t ts) const noexcept
    {
        return double(ts - start) / double(current);
    }

    void prediction_update(int64_t ts, double period) noexcept
    {
        time_stamp[first] = { ts, period };
        first = (first + 1) % N;
        current = predict_period();
    }
};

#endif // PERIOD_PREDICTOR_H