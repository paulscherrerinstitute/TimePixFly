/*!
\file
Unit tests
*/

#include "energy_points.h"
#include "pixel_map.h"
#include <fcntl.h>
#include <cstdio>
#include <ostream>
#include <set>
#include <iostream>
#include <regex>
#include <sstream>

#if defined(__AVX2__)
    #include <immintrin.h>
    #include "avx2_decoder.h"
#endif

#include <Poco/Exception.h>

#include "global.h"
#include "io_buf.h"
#include "subreservation.h"
#include "event_type.h"

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
    \brief Output
    \param out Output stream
    \param ev Event
    \return Output stream
    */
    inline std::ostream& operator<<(std::ostream& out, const event_t& ev)
    {
        return out << (ev.is_tdc ? "tdc" : "toa") << "{.ts=" << ev.ts << ", .px=" << ev.px << '}';
    }

    /*!
    \brief Output
    \param out Output stream
    \param ep Energy point part
    \return Output stream
    */
    inline std::ostream& operator<<(std::ostream& out, const EpPart& ep)
    {
        return out << "ep{" << ep.energy_point << ", " << ep.weight << '}';
    }

    /*!
    \brief Output
    \param out Output stream
    \param md Map destination
    \return Output stream
    */
    inline std::ostream& operator<<(std::ostream& out, const MapDest& md)
    {
        return out << "md{" << md.energy_point << ", " << md.weight << '}';
    }

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
            : output(not_quiet), out(s)
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
    \brief Check last test result
    \param unit Test unit of the last test
    \return True iff test succeeded
    */
    bool last_ok(const test_unit& unit) noexcept
    {
        unsigned t = 0;
        bool state = false;
        if (! successful_tests.empty()) {
            auto& test = successful_tests.back();
            if (test.unit == &unit) {
                t = test.num;
                state = true;
            }
        }
        if (! failed_tests.empty()) {
            auto& test = failed_tests.back();
            if (test.unit == &unit) {
                if (! state)
                    return false;
                return t > test.num;
            }
        }
        return state;
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
    \brief Inequality check

    This test fails iff a==b
    The test position counter will be incremented by one.

    \param unit Test unit reference
    \param t    Test position counter reference
    \param a    First value
    \param b    Second value
    */
    template<typename T>
    void check_neq(const test_unit& unit, unsigned& t, const T& a, const T& b)
    {
        if (a == b) {
            verbose << unit.name << ' ' << t << " failed: " << a << " == " << b << '\n';
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
    [[maybe_unused]] void check_eq<double>(const test_unit& unit, unsigned& t, const double& a, const double&b)
    {
        constexpr static double threshold = 1e-6;
        if ((a <= b - threshold) || (a >= b + threshold)) {
            verbose << unit.name << ' ' << t << " failed: " << a << " != " << b << '\n';
            test_failed(unit, t);
        } else
            test_succeeded(unit, t);
    }

    /*! Pixel to energy point mapping unit tests */
    namespace pixmap {
        /*!
        \brief PixelIndexToEp tests
        \param unit Test unit
        */
        void energypoints_test(const test_unit& unit)
        {
            unsigned t = 0;
            auto& gvar = *global::instance;
            ::PixelIndexToEp pite;
            ::detector_layout layout{chip_size, chip_size, {{{0,0}}}};
            gvar.layout = layout;

            std::string ep {"0,0,0,1,.8,.2\n0,1,0,1,.2,.8\n"};
            std::istringstream iss{ep};

            PixelIndexToEp::from(pite, iss);
            check_eq(unit, t, pite.npoints, 2u);
            check_eq(unit, t, pite.chip.size(), 1ul);
            if (! last_ok(unit))
                return;
            check_eq(unit, t, pite.chip[0].flat_pixel.size(), 256ul*256ul);
            if (! last_ok(unit))
                return;
            check_eq(unit, t, pite.chip[0].flat_pixel[1].part.size(), 2ul);
            if (! last_ok(unit))
                return;
            check_eq(unit, t, (unsigned)pite.chip.size(), 1u);
            check_eq(unit, t, pite.at({0,1}).part[1], EpPart{1,.8});

            auto pm = pite.to_map();
            bool first = true;
            for (auto& p : (*pm)[{0, 0}]) {
                check_eq(unit, t, p, first ? MapDest{0, .8} : MapDest{1, .2});
                first = !first;
            }
            for (auto& p : (*pm)[{0, 1}]) {
                check_eq(unit, t, p, first ? MapDest{0, .2} : MapDest{1, .8});
                first = !first;
            }
        }
    } // namespace pixmap

    /*! CPU mask parsing and setting */
    namespace cpumask {
        /*!
        \brief cpu_mask parsing tests
        \param unit Test unit
        */
        void parse(const test_unit& unit)
        {
            unsigned t = 0;
            cpu_mask::cpu_mask_t mask;

            std::string test = "OK";
            unsigned pos = 0;
            auto fn = [&test, &pos](unsigned p, const std::string& err) {
                test = err;
                pos = p;
            };

            cpu_mask::parse(mask, "a:0-8;w:9;r:10", fn);
            check_eq(unit, t, std::string{"OK"}, test);
            check_eq(unit, t, mask.reader_cpu, 10);
            check_eq(unit, t, mask.writer_cpu, 9);
            check_eq(unit, t, mask.analysis_cpus.size(), size_t{9});
            for (int i=0; i<(int)mask.analysis_cpus.size(); i++)
                check_eq(unit, t, mask.get_cpu(i), i);
            // t=13
            mask.clear();
            cpu_mask::parse(mask, "a0-8;w:9;r:10", fn);
            check_eq(unit, t, std::string{"colon expected"}, test);
            check_eq(unit, t, pos, 2u); // t=15
            test = "OK";
            mask.clear();
            cpu_mask::parse(mask, ":0-8;w:9;r:10", fn);
            check_eq(unit, t, std::string{"one of the characters a,r,w expected"}, test);
            check_eq(unit, t, pos, 1u);
            test = "OK";
            mask.clear();
            cpu_mask::parse(mask, "a:0-8;:9;r:10", fn);
            check_eq(unit, t, std::string{"one of the characters a,r,w expected"}, test);
            check_eq(unit, t, pos, 7u);
            test = "OK";
            mask.clear();
            cpu_mask::parse(mask, "a:0-8;w:9;:10", fn);
            check_eq(unit, t, std::string{"one of the characters a,r,w expected"}, test); // t=20
            check_eq(unit, t, pos, 11u);
            test = "OK";
            mask.clear();
            cpu_mask::parse(mask, "a:0-8;w:9;r:", fn);
            check_eq(unit, t, std::string{"expected number"}, test);
            check_eq(unit, t, pos, 12u);
            test = "OK";
            mask.clear();
            cpu_mask::parse(mask, "a:0-;w:9;r:10", fn);
            check_eq(unit, t, std::string{"expected number"}, test);
            check_eq(unit, t, pos, 5u); // t=25
            test = "OK";
            mask.clear();
            cpu_mask::parse(mask, "a:0-8;w:9;w:10", fn);
            check_eq(unit, t, std::string{"affinity can only be set once"}, test);
            check_eq(unit, t, pos, 14u);
            test = "OK";
            mask.clear();
            cpu_mask::parse(mask, "a:0,1-8;a:0;w:9;w:10", fn);
            check_eq(unit, t, std::string{"analysis affinity already set"}, test);
            check_eq(unit, t, pos, 9u);
            test = "OK";
            mask.clear();
            cpu_mask::parse(mask, "a:1,1", fn);
            check_eq(unit, t, std::string{"OK"}, test);
            check_eq(unit, t, mask.reader_cpu, -1); // t=30
            check_eq(unit, t, mask.writer_cpu, -1);
            check_eq(unit, t, mask.get_cpu(0), 1);
            check_eq(unit, t, mask.get_cpu(1), 1);
            check_eq(unit, t, mask.get_cpu(2), -1);
            test = "OK";
            mask.clear();
            check_eq(unit, t, mask.get_cpu(0), -1); // t=35
        }
    } // namespace cpumask

    namespace iobuf {
        ::iobuf::jar_t jar;     //!< Data jar
        ::iobuf::reservation_t reservation = {&jar, 0, ::iobuf::container_size};    //!< Reservation

        /*!
        \brief cpu_mask parsing tests
        \param unit Test unit
        */
        void subreservation(const test_unit& unit)
        {
            using Event = AsiRawStreamDecoder::Event;
            using ::iobuf::subreservation_t;
            const auto& chunk_id = AsiRawStreamDecoder::chunk_id;
            Event* data = (Event*)jar.container.data;
            subreservation_t subreservation(1);

            unsigned t = 0;
            data[0].header = {};
            try {
                subreservation.update(reservation);
                test_failed(unit, t);
            } catch (Poco::RuntimeException& ex) {
                check_eq(unit, t, ex.message(), std::string{"expected header has no TPX3 id"});
            } catch (...) {
                test_failed(unit, t);
            }
            // t=1
            data[0].header = {chunk_id, 1, 0, 0};
            try {
                subreservation.update(reservation);
                test_failed(unit, t);
            } catch (Poco::RuntimeException& ex) {
                check_eq(unit, t, ex.message(), std::string{"encountered bogus chunk size"});
            }
            // t=2
            data[0].header = {chunk_id, 0, 0, 2};
            try {
                subreservation.update(reservation);
                test_failed(unit, t);
            } catch (Poco::RuntimeException& ex) {
                check_eq(unit, t, ex.message(), std::string{"chunk size not a multiple of the event size"});
            } catch (...) {
                test_failed(unit, t);
            }
            // t=3
            subreservation = subreservation_t{1};
            data[0].header = {chunk_id, 1, 0, 3*sizeof(u64)};
            data[1].packet_id = {3, 0, 0x50};
            try {
                subreservation.update(reservation);
                test_failed(unit, t);
            } catch (Poco::RuntimeException& ex) {
                check_eq(unit, t, ex.message(), std::string{"unable to handle reordered chunk, expected id 0, but got id 3"});
            } catch (...) {
                test_failed(unit, t);
            }
            check_eq(unit, t, subreservation.state, subreservation.CHECK_ID);
            // t=5
            data[1].packet_id = {0, 0, 0x50};   // packet id=0
            subreservation.update(reservation);
            check_eq(unit, t, subreservation.state, subreservation.DATA);
            check_eq(unit, t, subreservation.pos, 2);
            check_eq(unit, t, subreservation.consume, 2);
            // t=8
            data[4].header = {chunk_id, 0, 0, 3*sizeof(u64)};
            data[8].header = {chunk_id, 1, 0, 3*sizeof(u64)};
            data[9].packet_id = {1, 0, 0x50};   // packet id=1
            subreservation.update(reservation);
            check_eq(unit, t, subreservation.state, subreservation.DATA);
            check_eq(unit, t, subreservation.pos, 10);
            check_eq(unit, t, subreservation.consume, 2);
            // t=11 - border before header
            reservation.end = 12*sizeof(u64);
            subreservation.update(reservation);
            check_eq(unit, t, subreservation.state, subreservation.SEARCH);
            check_eq(unit, t, subreservation.pos, 0);
            check_eq(unit, t, subreservation.rest, 0);
            check_eq(unit, t, subreservation.consume, 0);
            // t=15 - border after header
            reservation.end = 1*sizeof(u64);
            subreservation.update(reservation);
            check_eq(unit, t, subreservation.state, subreservation.CHECK_ID);
            check_eq(unit, t, subreservation.pos, -2);
            check_eq(unit, t, subreservation.rest, 0);
            // t=18 - border after chunk id
            data[0].packet_id = {2, 0, 0x50};   // packet id=2
            subreservation.update(reservation);
            check_eq(unit, t, subreservation.state, subreservation.DATA);
            check_eq(unit, t, subreservation.pos, -2);
            check_eq(unit, t, subreservation.rest, 0);
            // t=21 - border within data
            subreservation.update(reservation);
            check_eq(unit, t, subreservation.state, subreservation.DATA);
            check_eq(unit, t, subreservation.pos, 0);
            check_eq(unit, t, subreservation.rest, 2);
            check_eq(unit, t, subreservation.consume, 1);
            // t=25
            subreservation.update(reservation);
            check_eq(unit, t, subreservation.state, subreservation.DATA);
            check_eq(unit, t, subreservation.pos, -1);
            check_eq(unit, t, subreservation.rest, 0);
            check_eq(unit, t, subreservation.consume, 0);
            // t=29
            subreservation.update(reservation);
            check_eq(unit, t, subreservation.state, subreservation.DATA);
            check_eq(unit, t, subreservation. pos, 0);
            check_eq(unit, t, subreservation.rest, 1);
            check_eq(unit, t, subreservation.consume, 1);
            // t=33
        }
    } // namespace iobuf

    #if defined(__AVX2__)
        namespace decode {
            /*!
            \brief AVX2 raw event decoding
            \param unit Test unit
            */
            void avx2(const test_unit& unit)
            {
                using Event = AsiRawStreamDecoder::Event;
                constexpr u64 chunk_id = AsiRawStreamDecoder::chunk_id;

                alignas(sizeof(__m256i)) Event raw_events[] = {
                    { .header = {chunk_id, 3, 0, 8}},
                    { .packet_id = {1, 0, 0x50}},
                    { .tdc = {0, 6, 100, 0, 0xf, 0x6}},
                    { .toa = {10, 10, 10, 10, 0x011f, 0xb}}
                };
                auto event_vec = _mm256_load_si256((__m256i*)raw_events);
                alignas(sizeof(__m256i)) event_t decoded_events[4];
                _mm256_store_si256((__m256i*)decoded_events, avx2::decode(event_vec));

                unsigned t=0;
                check_eq(unit, t, decoded_events[0], event_t{0,0,0});
                check_eq(unit, t, decoded_events[1], event_t{0,0,0});
                check_eq(unit, t, decoded_events[2], event_t{
                    AsiRawStreamDecoder::getTdcClock(raw_events[2].tdc),
                    1,
                    0
                });
                check_eq(unit, t, decoded_events[3], event_t{
                    AsiRawStreamDecoder::getToaClock(raw_events[3].toa),
                    0,
                    AsiRawStreamDecoder::flatPixel(raw_events[3].toa)
                });
            }
        }
    #endif

    /*!
    \brief Initialize unit tests
    */
    void init_tests()
    {
        tests.insert({
            "pixmap::energypoints",
            "PixelIndexToEp data structure tests",
            pixmap::energypoints_test
        });
        tests.insert({
            "cpu_mask::parse",
            "cpu mask argument parsing",
            cpumask::parse
        });
        tests.insert({
            "iobuf::subreservation",
            "subreservation type",
            iobuf::subreservation
        });
        #if defined(__AVX2__)
            tests.insert({
                "decode::avx2",
                "avx2 raw event decoding",
                decode::avx2
            });
        #endif
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

    return failed_tests.empty() ? 0 : 1;
}
