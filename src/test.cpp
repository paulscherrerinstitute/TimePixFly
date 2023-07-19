#include <set>
#include <vector>
#include <functional>
#include <iostream>
#include "period_predictor.h"
#include "event_reordering.h"

namespace {

    struct test_unit final {
        std::string name;
        std::string desc;
        std::function<void(const test_unit&)> test;
        struct less final {
            bool operator()(const test_unit& a, const test_unit& b) const
            {
                return a.name < b.name;
            }
        };
    };

    struct test_result final {
        const test_unit* unit;
        unsigned num;
    };

    std::set<test_unit, test_unit::less> tests;
    std::vector<test_result> failed_tests;
    std::vector<test_result> successful_tests;

    template<typename Stream>
    struct verbose_type final : public Stream {
        bool output;
        Stream& out;

        template<typename T>
        inline verbose_type& operator<<(const T& val)
        {
            if (output)
                out << val;
            return *this;
        }

        inline explicit verbose_type(Stream& s, bool not_quiet) noexcept
            : out(s), output(not_quiet)
        {}

        inline ~verbose_type() = default;

        verbose_type(const verbose_type&) = delete;
        inline verbose_type(verbose_type&&) = default;

        verbose_type& operator=(const verbose_type&) = delete;
        inline verbose_type& operator=(verbose_type&&) = default;
    };
    
    verbose_type<decltype(std::cout)> verbose{std::cout, false};

    void test_failed(const test_unit& unit, unsigned& t)
    {
        failed_tests.push_back({&unit, t});
        t++;
    }

    void test_succeeded(const test_unit& unit, unsigned& t)
    {
        successful_tests.push_back({&unit, t});
        t++;
    }

    template<typename T>
    void check_eq(const test_unit& unit, unsigned& t, const T& a, const T& b)
    {
        if (a != b) {
            verbose << unit.name << ' ' << t << " failed: " << a << " != " << b << '\n';
            test_failed(unit, t);
        } else
            test_succeeded(unit, t);
    }

    template<>
    void check_eq<double>(const test_unit& unit, unsigned& t, const double& a, const double&b)
    {
        constexpr static double threshold = 1e-6;
        if ((a <= b - threshold) || (a >= b + threshold)) {
            verbose << unit.name << ' ' << t << " failed: " << a << " != " << b << '\n';
            test_failed(unit, t);
        } else
            test_succeeded(unit, t);
    }

    namespace period_predictor {
        void predictor_reset_test(const test_unit& unit)
        {
            unsigned t = 0;
            ::period_predictor p{0, 2};
            check_eq(unit, t, p.interval_prediction(), 2.0);
            check_eq(unit, t, p.period_prediction(6), 3.0);
            check_eq(unit, t, p.minPoints(), (unsigned)3);
            p.reset(1, 2);
            check_eq(unit, t, p.interval_prediction(), 2.0);
            check_eq(unit, t, p.period_prediction(5), 2.0);
        }
        
        void predictor_update_test(const test_unit& unit)
        {
            unsigned t = 0;
            ::period_predictor p{0, 2};
            p.start_update(2);
            check_eq(unit, t, p.interval_prediction(), 2.0);
            check_eq(unit, t, p.period_prediction(6), 3.0);
            p.prediction_update(5);
            p.prediction_update(8);
            p.prediction_update(11);
            check_eq(unit, t, p.interval_prediction(), 3.0);
            check_eq(unit, t, p.period_prediction(14), 5.0);
        }
    }

    namespace event_reorder_queue {
        void sorted_test(const test_unit& unit)
        {
            unsigned t = 0;
            ::event_reorder_queue q;
            q.push({4, 4});
            q.push({1, 1});
            q.push({2, 2});
            check_eq(unit, t, q.size(), (::event_reorder_queue::size_type)3);
            check_eq(unit, t, q.top().toa, (int64_t)1);
            q.pop();
            check_eq(unit, t, q.top().toa, (int64_t)2);
            q.pop();
            check_eq(unit, t, q.top().toa, (int64_t)4);
            q.pop();
            check_eq(unit, t, q.empty(), true);
        }
    }

    void init_tests()
    {
        tests.insert({
            "period_predictor::predictor_reset",
            "contructor, reset, interval_prediction, period_prediction",
            period_predictor::predictor_reset_test
        });
        tests.insert({
            "period_predictor::predictor_update",
            "prediction_update, start_update",
            period_predictor::predictor_update_test
        });
        tests.insert({
            "event_reorder_queue::sorted",
            "iterator sequence",
            event_reorder_queue::sorted_test
        });
    }

    [[noreturn]]
    void help(const std::string& progname)
    {
        std::cout << progname << " (-h | --help)\n";
        std::cout << progname << " [(-v | --verbose)] substr*\n";
        exit(1);
    }
}

int main(int argc, char *argv[])
{
    std::vector<std::string> pattern;
    for (int i=1; i<argc; i++) {
        const std::string& arg = argv[i];
        if ((arg == "--help") || (arg == "-h"))
            help(argc ? argv[0] : "<exe>");
        else if ((arg == "--verbose") || (arg == "-v"))
            verbose.output = true;
        else
            pattern.push_back(argv[i]);
    }
    init_tests();
    for (const auto& unit : tests) {
        bool selected = false;
        if (! pattern.empty()) {
            for (const auto& pat : pattern) {
                if (unit.name.find(pat) != std::string::npos) {
                    selected = true;
                    break;
                }
            }
        } else
            selected = true;
        if (selected)
            unit.test(unit);
    }

    for (const auto& res : successful_tests)
        std::cout << "OK    : " << res.unit->name << ' ' << res.num << '\n';

    for (const auto& res : failed_tests)
        std::cout << "FAILED: " << res.unit->name << ' ' << res.num << '\n';

    return 0;
}
