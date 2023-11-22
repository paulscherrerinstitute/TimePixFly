/*!
\file
Unit tests
*/

#include <set>
#include <vector>
#include <functional>
#include <iostream>
#include <cstring>
#include <regex>
#include "period_predictor.h"
#include "event_reordering.h"
#include "period_queues.h"

namespace {

    /*!
    \brief Unit test object
    */
    struct test_unit final {
        std::string name;                           //!< Name of test unit
        std::string desc;                           //!< Description of test unit
        std::function<void(const test_unit&)> test; //!< Test function for test unit

        /*!
        \brief Comparator for sorting unit test objects
        */
        struct less final {
            /*!
            \brief Comparison operator
            \param a    Firts test unit
            \param b    Second test unit
            \return True if first test unit name is alphabetically less than that of the second test unit
            */
            bool operator()(const test_unit& a, const test_unit& b) const
            {
                return a.name < b.name;
            }
        };
    };

    /*!
    \brief Unit test result
    */
    struct test_result final {
        const test_unit* unit;  //!< Pointer to test unit
        unsigned num;           //!< Test position number
    };

    std::set<test_unit, test_unit::less> tests; //!< Set of all tests
    std::vector<test_result> failed_tests;      //!< List of failed tests
    std::vector<test_result> successful_tests;  //!< List of successful tests

    /*!
    \brief Verbose output abstraction object
    */
    template<typename Stream>
    struct verbose_type final : public Stream {
        bool output;    //!< Generate output?
        Stream& out;    //!< Underlying output stream

        /*!
        \brief Output operator
        \param val Value to print into the stream
        \return Reference to `this`
        */
        template<typename T>
        inline verbose_type& operator<<(const T& val)
        {
            if (output)
                out << val;
            return *this;
        }

        /*!
        \brief Constructor
        \param s            Underlying stream
        \param not_quiet    True if output should not be hidden
        */
        inline explicit verbose_type(Stream& s, bool not_quiet) noexcept
            : out(s), output(not_quiet)
        {}

        inline ~verbose_type() = default;

        verbose_type(const verbose_type&) = delete;
        inline verbose_type(verbose_type&&) = default;  //!< Move constructor

        verbose_type& operator=(const verbose_type&) = delete;
        inline verbose_type& operator=(verbose_type&&) = default;   //!< Move assignment \return `this`
    };
    
    verbose_type<decltype(std::cout)> verbose{std::cout, false};    //!< verbose_type stream object

    /*!
    \brief Failed test processing

    This puts the test unit object on the list of failed tests and increments the test position counter.
    The test position counter can be used to identify which check within the test unit failed.

    \param unit Test unit object reference
    \param t    Reference to test position counter
    */
    void test_failed(const test_unit& unit, unsigned& t)
    {
        failed_tests.push_back({&unit, t});
        t++;
    }

    /*!
    \brief Successful test processing

    This puts the test unit object on the list of successful tests and increments the test position counter.
    The test position counter can be used to identify which check within the test unit succeeded.

    \param unit Test unit object reference
    \param t    Reference to test position counter
    */
    void test_succeeded(const test_unit& unit, unsigned& t)
    {
        successful_tests.push_back({&unit, t});
        t++;
    }

    /*!
    \brief Output operator for period_index
    \param out Output stream
    \param idx Abstract period index
    \return out
    */
    inline decltype(verbose)& operator<<(decltype(verbose)& out, const period_index& idx)
    {
        return out.operator<<(idx);
    }

    /*!
    \brief Unequality check for abstract period indices
    \param a First period index
    \param b Second period index
    \return true iff a is not equal to b by value
    */
    bool operator!=(const period_index& a, const period_index& b) noexcept
    {
        return a != b;
    }

    /*!
    \brief Equality check

    This test fails iff a!=b
    The test position counter will be incremented by one.

    \param unit Test unit reference
    \param t    Test position counter reference
    \param a    First value
    \param b    Second value
    */
    template<typename T>
    void check_eq(const test_unit& unit, unsigned& t, const T& a, const T& b)
    {
        if (a != b) {
            verbose << unit.name << ' ' << t << " failed: " << a << " != " << b << '\n';
            test_failed(unit, t);
        } else
            test_succeeded(unit, t);
    }

    /*!
    \brief Equality check for double

    Due to numeric issues, double values are considered equal if they are within a small distance (1e-6) of each other.

    \param unit Test unit reference
    \param t    Test position counter reference
    \param a    First value
    \param b    Second value
    */    
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

    /*! Period predictor unit tests */
    namespace period_predictor {
        /*!
        \brief Period predictor reset() unit test
        \param unit Test unit
        */
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
        
        /*!
        \brief Period predictor update() unit test
        \param unit Test unit
        */
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

    /*! Event reorder queue unit tests */
    namespace event_reorder_queue {
        /*!
        \brief Check sorting feature of an event reorder queue
        \param unit Test unit
        */
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

    /*! Period queues unit tests */
    namespace period_queues {
        /*!
        \brief Period queues period_index_for() unit test
        \param unit Test unit
        */
        void period_index_for_test(const test_unit& unit)
        {
            unsigned t = 0;
            ::period_queues pq;
            double d = pq.threshold / 2.0;
            check_eq(unit, t, pq.period_index_for(1.0), period_index{1, 1, true});
            check_eq(unit, t, pq.period_index_for(1.0 + d), period_index{1, 1, true});
            check_eq(unit, t, pq.period_index_for(1.5), period_index{1, 1, false});
            check_eq(unit, t, pq.period_index_for(2.0 - d), period_index{1, 2, true});
        }

        /*!
        \brief Period queues refined_index() unit test
        \param unit Test unit
        */
        void refined_index_test(const test_unit& unit)
        {
            unsigned t = 0;
            ::period_queues pq;
            double d = pq.threshold / 2.0;
            period_index idx;
            idx = pq.period_index_for(0.5); // undisputed index
            check_eq(unit, t, idx, period_index{0, 0, false});
            pq.refined_index(idx, 0);
            check_eq(unit, t, idx, period_index{0, 0, false});
            idx = pq.period_index_for(d);   // disputed index
            check_eq(unit, t, idx, period_index{0, 0, true});
            pq.refined_index(idx, 0);
            check_eq(unit, t, idx, period_index{0, 0, true});
            pq[idx] = period_queue_element{};
            check_eq(unit, t, pq[idx].start_seen, false);
            pq.refined_index(idx, 0);
            check_eq(unit, t, idx, period_index{0, 0, true});
            pq.registerStart(idx, 1);
            check_eq(unit, t, pq[idx].start, (int64_t)1);
            check_eq(unit, t, pq[idx].start_seen, true);
            pq.refined_index(idx, 2);
            check_eq(unit, t, idx, period_index{0, 0, false});
            idx.disputed = true;
            pq.refined_index(idx, 0);
            check_eq(unit, t, idx, period_index{-1, 0, false});
        }

        /*!
        \brief Period queues oldest() unit test
        \param unit Test unit
        */
        void purge_test(const test_unit& unit)
        {
            unsigned t = 0;
            ::period_queues pq;
            double d = pq.threshold / 2.0;
            period_index idx = pq.period_index_for(d);
            pq[idx] = period_queue_element{};
            auto rq = pq.registerStart(idx, 1);
            auto oldest = pq.oldest();
            check_eq(unit, t, oldest->first, (period_type)0);
            check_eq(unit, t, oldest->second.start, (int64_t)1);
            check_eq(unit, t, rq.empty(), true);
            check_eq(unit, t, pq.element.size(), (::period_queues::queue_type::size_type)1);
            pq.erase(oldest);
            check_eq(unit, t, pq.element.size(), (::period_queues::queue_type::size_type)0);
        }
    }

    /*!
    \brief Initialize unit tests
    */
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
        tests.insert({
            "period_queues::period_index_for",
            "period_index_for",
            period_queues::period_index_for_test
        });
        tests.insert({
            "period_queues::refined_index",
            "refined_index",
            period_queues::refined_index_test
        });
        tests.insert({
            "period_queues::purge",
            "registerStart, oldest, erase",
            period_queues::purge_test
        });
    }

    /*!
    \brief Print help text
    \param progname The name of the executable
    */
    [[noreturn]]
    void help(const std::string& progname)
    {
        std::cout << progname << " (-h | --help)\n";
        std::cout << progname << " [(-v | --verbose)] [(-l | --list)] pattern*\n";
        exit(1);
    }
}

/*!
\brief Main function

Parse commandline parameters and either
- print help text
- list available unit tests
- execute unit tests

\param argc Number of comandline parameters
\param argv Commandline parameters
\return 0 on success, not 0 otherwise
*/
int main(int argc, char *argv[])
{
    std::vector<std::regex> pattern;
    bool list_tests = false;

    for (int i=1; i<argc; i++) {
        const std::string& arg = argv[i];
        if ((arg == "--help") || (arg == "-h"))
            help(argc ? argv[0] : "<exe>");
        else if ((arg == "--verbose") || (arg == "-v"))
            verbose.output = true;
        else if ((arg == "--list") || (arg == "-l"))
            list_tests = true;
        else try {
            pattern.emplace_back(argv[i]);
        } catch (std::exception& ex) {
            std::cerr << "Pattern error: " << ex.what() << '\n';
            return 1;
        }
    }

    init_tests();

    std::vector<const test_unit*> selected_tests;
    if (! pattern.empty()) {
        for (const auto& unit : tests) {
            for (const auto& pat : pattern) {
                if (regex_search(std::begin(unit.name), std::end(unit.name), pat)) {
                    selected_tests.push_back(&unit);
                    break;
                }
            }
        }
    } else {
        for (const auto& unit : tests)
            selected_tests.emplace_back(&unit);
    }

    if (list_tests) {
        for (const auto* unit : selected_tests){
            std::cout << unit->name; verbose << " : " << unit->desc; std::cout << '\n';
        }
        return 0;
    }

    for (const auto& unit : selected_tests) {
        unit->test(*unit);
    }

    for (const auto& res : successful_tests)
        std::cout << "OK    : " << res.unit->name << ' ' << res.num << '\n';

    for (const auto& res : failed_tests)
        std::cout << "FAILED: " << res.unit->name << ' ' << res.num << '\n';

    return 0;
}
