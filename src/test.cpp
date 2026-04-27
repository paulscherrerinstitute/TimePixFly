/*!
\file
Unit tests
*/

#include "energy_points.h"
#include "pixel_map.h"
#include "shared_types.h"
#include <fcntl.h>
#include <cstdio>
#include <ostream>
#include <set>
#include <iostream>
#include <regex>
#include <sstream>

#if defined(__AVX2__) && defined(AVX_DECODE)
    #define USE_AVX
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
    \brief Section descriptor
    */
    struct section final {
        std::string name;   //!< Section name
        unsigned start;     //!< Section start position
    };

    section no_section{"", 0};  //!< No section specifier

    /*!
    \brief Unit test result
    */
    struct test_result final {
        const test_unit* unit;  //!< Pointer to test unit
        section s;              //!< Section description
        unsigned num;           //!< Test position number
    };

    /*!
    \brief Print out a test result

    \param out Output stream
    \param res Test result
    \tparam stream Output stream type
    \return Stream object reference
    */
    template<typename stream>
    stream& operator<<(stream& out, const test_result& res)
    {
        out << res.unit->name << ' ' << res.num;
        if (!res.s.name.empty())
            out << " (" << res.s.name << '@' << res.s.start << ')';
        return out;
    }

    std::set<test_unit, test_unit::less> tests; //!< Set of all tests
    std::vector<test_result> failed_tests;      //!< List of failed tests
    std::vector<test_result> successful_tests;  //!< List of successful tests

    #if defined(USE_AVX)
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
    #endif

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
    [[maybe_unused]] void test_failed(const test_unit& unit, unsigned& t)
    {
        failed_tests.push_back({&unit, no_section, t});
        t++;
    }

    /*!
    \brief Failed test processing

    This puts the test unit object on the list of failed tests and increments the test position counter.
    The test section and position counter can be used to identify which check within the test unit failed.

    \param unit Test unit object reference
    \param s    Reference to test section
    \param t    Reference to test position counter
    */
    void test_failed(const test_unit& unit, const section &s, unsigned& t)
    {
        failed_tests.push_back({&unit, s, t});
        t++;
    }

    /*!
    \brief Successful test processing

    This puts the test unit object on the list of successful tests and increments the test position counter.
    The test position counter can be used to identify which check within the test unit succeeded.

    \param unit Test unit object reference
    \param t    Reference to test position counter
    */
    [[maybe_unused]] void test_succeeded(const test_unit& unit, unsigned& t)
    {
        successful_tests.push_back({&unit, no_section, t});
        t++;
    }

    /*!
    \brief Successful test processing

    This puts the test unit object on the list of successful tests and increments the test position counter.
    The test section and position counter can be used to identify which check within the test unit succeeded.

    \param unit Test unit object reference
    \param s    Test section
    \param t    Reference to test position counter
    */
    void test_succeeded(const test_unit& unit, const section& s, unsigned& t)
    {
        successful_tests.push_back({&unit, s, t});
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
    \param s    Test section
    */
    template<typename T>
    void check_eq(const test_unit& unit, unsigned& t, const T& a, const T& b, const section& s = no_section)
    {
        if (a != b) {
            verbose << unit.name << ' ' << t << " failed: " << a << " != " << b << '\n';
            test_failed(unit, s, t);
        } else
            test_succeeded(unit, s, t);
    }

    /*!
    \brief Inequality check

    This test fails iff a==b
    The test position counter will be incremented by one.

    \param unit Test unit reference
    \param t    Test position counter reference
    \param a    First value
    \param b    Second value
    \param s    Test section
    */
    template<typename T>
    void check_neq(const test_unit& unit, unsigned& t, const T& a, const T& b, const section& s = no_section)
    {
        if (a == b) {
            verbose << unit.name << ' ' << t << " failed: " << a << " == " << b << '\n';
            test_failed(unit, s, t);
        } else
            test_succeeded(unit, s, t);
    }

    /*!
    \brief Equality check for double

    Due to numeric issues, double values are considered equal if they are within a small distance (1e-6) of each other.

    \param unit Test unit reference
    \param t    Test position counter reference
    \param a    First value
    \param b    Second value
    \param s    Test section
    */
    template<>
    [[maybe_unused]] void check_eq<double>(const test_unit& unit, unsigned& t, const double& a, const double&b, const section& s)
    {
        constexpr static double threshold = 1e-6;
        if ((a <= b - threshold) || (a >= b + threshold)) {
            verbose << unit.name << ' ' << t << " failed: " << a << " != " << b << '\n';
            test_failed(unit, s, t);
        } else
            test_succeeded(unit, s, t);
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

            section s = {"loop1", t};
            auto pm = pite.to_map();
            bool first = true;
            for (auto& p : (*pm)[{0, 0}]) {
                check_eq(unit, t, p, first ? MapDest{0, .8} : MapDest{1, .2}, s);
                first = !first;
            }
            s = {"loop2", t};
            for (auto& p : (*pm)[{0, 1}]) {
                check_eq(unit, t, p, first ? MapDest{0, .2} : MapDest{1, .8}, s);
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
        /*!
        \brief cpu_mask parsing tests
        \param unit Test unit
        */
        void subreservation(const test_unit& unit)
        {
            using Event = AsiRawStreamDecoder::Event;
            using ::iobuf::reservation_t;
            using ::iobuf::subreservation_t;
            const auto& chunk_id = AsiRawStreamDecoder::chunk_id;
            const int jar_sz = ::iobuf::container_size / sizeof(u64);

            ::iobuf::collection_t buf{2, false};
            auto wres = buf.write_reservation(::iobuf::initial_reservation);
            assert(wres.jar && (wres.start == 0) && (wres.end == ::iobuf::container_size));
            Event* data = (Event*)wres.jar->container.data;
            wres.end = 12*sizeof(u64);
            wres = buf.write_reservation(wres);
            assert(wres.jar && (wres.start == 12*sizeof(u64)) && (wres.end == ::iobuf::container_size));
            subreservation_t subreservation(buf, 1);

            unsigned t = 0;
            section s = {"initialization", t};
            check_eq(unit, t, subreservation.chip, 1ul, s);
            check_eq(unit, t, subreservation.pos, 0, s);
            check_eq(unit, t, subreservation.rest, 0, s);
            check_eq(unit, t, subreservation.consume, 0, s);
            check_eq(unit, t, subreservation.content, (const Event*)data, s);

            // t=5
            s = {"exceptions", t};
            data[0].header = {};
            try {
                subreservation.update();
                test_failed(unit, s, t);
            } catch (Poco::RuntimeException& ex) {
                check_eq(unit, t, ex.message(), std::string{"expected header has no TPX3 id"}, s);
            } catch (...) {
                test_failed(unit, s, t);
            }
            // t=6
            data[0].header = {chunk_id, 1, 0, 0};
            try {
                subreservation.update();
                test_failed(unit, s, t);
            } catch (Poco::RuntimeException& ex) {
                check_eq(unit, t, ex.message(), std::string{"encountered bogus chunk size"}, s);
            }
            // t=7
            data[0].header = {chunk_id, 0, 0, 2};
            try {
                subreservation.update();
                test_failed(unit, s, t);
            } catch (Poco::RuntimeException& ex) {
                check_eq(unit, t, ex.message(), std::string{"chunk size not a multiple of the event size"}, s);
            } catch (...) {
                test_failed(unit, s, t);
            }
            // t=8
            data[0].header = {chunk_id, 1, 0, 3*sizeof(u64)};
            data[1].packet_id = {1, 0, 0x50};
            data[4].header = {chunk_id, 1, 0, 3*sizeof(u64)};
            data[5].packet_id = {3, 0, 0x50};
            try {
                subreservation.update();
                test_failed(unit, s, t);
            } catch (Poco::RuntimeException& ex) {
                check_eq(unit, t, ex.message(), std::string{"unable to handle reordered chunk, expected id 0, but got id 3"}, s);
            } catch (...) {
                test_failed(unit, s, t);
            }
            check_eq(unit, t, subreservation.get_state(), subreservation.CHECK_ID, s);
            subreservation.reset(subreservation.CHECK_ID, 1, 2, 0);
            // t=10
            s = {"data", t};
            data[1].packet_id = {0, 0, 0x50};   // packet id=0
            subreservation.update();
            check_eq(unit, t, subreservation.get_state(), subreservation.DATA, s);
            check_eq(unit, t, subreservation.pos, 2, s);
            check_eq(unit, t, subreservation.consume, 2, s);
            // t=13
            data[4].header = {chunk_id, 0, 0, 3*sizeof(u64)};
            data[8].header = {chunk_id, 1, 0, 3*sizeof(u64)};
            data[9].packet_id = {1, 0, 0x50};   // packet id=1
            subreservation.update();
            check_eq(unit, t, subreservation.get_state(), subreservation.DATA, s);
            check_eq(unit, t, subreservation.pos, 10, s);
            check_eq(unit, t, subreservation.consume, 2, s);
            // t=16 - border within data
            s = {"internal-border", t};
            data[12].header = {chunk_id, 1, 0, 3*sizeof(u64)};
            data[13].packet_id = {2, 0, 0x50};  // packet id=2
            wres.end = 15*sizeof(u64);
            wres = buf.write_reservation(wres);
            assert(wres.jar && (wres.start == 15*sizeof(u64)) && (wres.end == ::iobuf::container_size));
            subreservation.update();
            check_eq(unit, t, subreservation.get_state(), subreservation.DATA, s);
            check_eq(unit, t, subreservation.pos, 14, s);
            check_eq(unit, t, subreservation.rest, 2, s);
            check_eq(unit, t, subreservation.consume, 1, s);
            // t=12 - rest
            wres.end = 16*sizeof(u64);
            wres = buf.write_reservation(wres);
            assert(wres.jar && (wres.start == 16*sizeof(u64)) && (wres.end == ::iobuf::container_size));
            data[16].header = {chunk_id, 1, 0, ::iobuf::container_size - 17*sizeof(u64)};
            data[17].packet_id = {3, 0, 0x50};  // packet id=3
            wres = buf.write_reservation(wres);
            assert(wres.jar && (wres.start == 0) && (wres.end == ::iobuf::container_size));
            subreservation.update();
            check_eq(unit, t, subreservation.get_state(), subreservation.DATA, s);
            check_eq(unit, t, subreservation.pos, 15, s);
            check_eq(unit, t, subreservation.rest, 1, s);
            check_eq(unit, t, subreservation.consume, 1, s);
            // t=24 - cross jar before header
            s = {"header-border", t};
            subreservation.update();
            check_eq(unit, t, subreservation.get_state(), subreservation.DATA, s);
            check_eq(unit, t, subreservation.pos, 18, s);
            check_eq(unit, t, subreservation.rest, jar_sz - 18, s);
            check_eq(unit, t, subreservation.consume, jar_sz - 18, s);
            // t=28 - cross jar before id
            s = {"id-border", t};
            data = (Event*)wres.jar->container.data;
            data[0].header = {chunk_id, 1, 0, ::iobuf::container_size - 2*sizeof(u64)};
            data[1].packet_id = {4, 0, 0x50};  // packet id=4
            data[jar_sz-1].header = {chunk_id, 1, 0, ::iobuf::container_size - 2*sizeof(u64)};
            wres = buf.write_reservation(wres);
            assert(wres.jar && (wres.start == 0) && (wres.end == ::iobuf::container_size));
            subreservation.update();
            check_eq(unit, t, subreservation.get_state(), subreservation.DATA, s);
            check_eq(unit, t, subreservation.pos, 2, s);
            check_eq(unit, t, subreservation.rest, jar_sz - 3, s);
            check_eq(unit, t, subreservation.consume, jar_sz - 3, s);
            // t=32 - cross jar before data
            s = {"data-border", t};
            data = (Event*)wres.jar->container.data;
            data[0].packet_id = {5, 0, 0x50};  // packet id=5
            data[jar_sz-2].header = {chunk_id, 1, 0, 3*sizeof(u64)};
            data[jar_sz-1].packet_id = {6, 0, 0x50};  // packet id=6
            wres = buf.write_reservation(wres);
            assert(wres.jar && (wres.start == 0) && (wres.end == ::iobuf::container_size));
            subreservation.update();
            check_eq(unit, t, subreservation.get_state(), subreservation.DATA, s);
            check_eq(unit, t, subreservation.pos, 1, s);
            check_eq(unit, t, subreservation.rest, jar_sz - 3, s);
            check_eq(unit, t, subreservation.consume, jar_sz - 3, s);
            // t=36 - end
            s = {"data-end", t};
            wres.end = 2*sizeof(u64);
            wres = buf.write_reservation(wres);
            assert(wres.jar && (wres.start == 2*sizeof(u64)) && (wres.end == ::iobuf::container_size));
            subreservation.update();
            check_eq(unit, t, subreservation.get_state(), subreservation.DATA, s);
            check_eq(unit, t, subreservation.pos, 0, s);
            check_eq(unit, t, subreservation.rest, 2, s);
            check_eq(unit, t, subreservation.consume, 2, s);
            // t=40
            s = {"store", t};
            data = (Event*)wres.jar->container.data;
            data[2].header = {chunk_id, 1, 0, 3*sizeof(u64)};
            data[3].packet_id = {8, 0, 0x50};  // packet id=8
            data[6].header = {chunk_id, 1, 0, 3*sizeof(u64)};
            data[7].packet_id = {7, 0, 0x50};  // packet id=7
            data[10].header = {chunk_id, 1, 0, 3*sizeof(u64)};
            data[11].packet_id = {9, 0, 0x50};  // packet id=9;
            wres.end = 14*sizeof(u64);
            wres = buf.write_reservation(wres);
            subreservation.update();
            check_eq(unit, t, subreservation.get_state(), subreservation.DATA, s);
            check_eq(unit, t, subreservation.pos, 8, s);
            check_eq(unit, t, subreservation.rest, 2, s);
            check_eq(unit, t, subreservation.consume, 2, s);
            subreservation.update();
            check_eq(unit, t, subreservation.get_state(), subreservation.RESTORE, s);
            check_eq(unit, t, subreservation.pos, 4, s);
            check_eq(unit, t, subreservation.rest, 2, s);
            check_eq(unit, t, subreservation.consume, 2, s);
            subreservation.update();
            check_eq(unit, t, subreservation.get_state(), subreservation.DATA, s);
            check_eq(unit, t, subreservation.pos, 12, s);
            check_eq(unit, t, subreservation.rest, 2, s);
            check_eq(unit, t, subreservation.consume, 2, s);
            // t=52
            s = {"end", t};
            wres.end = 14*sizeof(u64);
            wres = buf.write_reservation(wres);
            assert(!wres.end);
            subreservation.update();
            check_eq(unit, t, subreservation.rest, 0, s);
            // t=53
        }
    } // namespace iobuf

    #if defined(USE_AVX)
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
        #if defined(USE_AVX)
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
        std::cout << "OK    : " << res << '\n';

    for (const auto& res : failed_tests)
        std::cout << "FAILED: " << res << '\n';

    return failed_tests.empty() ? 0 : 1;
}
