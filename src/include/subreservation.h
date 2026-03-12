#pragma once

#ifndef SUBRESERVATION_H
#define SUBRESERVATION_H

#include <cstring>

#include <Poco/Exception.h>

#include "decoder.h"
#include "io_buf.h"

using Poco::RuntimeException;

namespace iobuf {
    struct subreservation_t final {
        constexpr static u64 event_size = sizeof(u64);
        const u64 chip;     // fixed chip number
        u64 pkgid;          // next expected pkg id
        const AsiRawStreamDecoder::Event* content; // null->initial subreservation
        int pos;            // current relative (to start) position if > 0, or for CHEHECK_ID/DATA: -rest if < 0
        int rest;           // 0->no more data in reservation
        int consume;        // amount to consume
        enum { INIT, SEARCH, CHECK_ID, DATA } state;

        inline subreservation_t(u64 chip_no)
            : chip{chip_no}, pkgid{0ul}, content{nullptr}, pos{0}, rest{0}, consume{0}, state{INIT}
        {}

        inline subreservation_t(subreservation_t&&) noexcept = default;

        subreservation_t& operator=(subreservation_t&& other) noexcept
        {
            std::memcpy((void*)this, &other, sizeof(*this));
            return *this;
        }

        // return continue immediately flag
        inline bool data(int from, int to)
        {
            assert((state == DATA) && (rest > 0));

            if (consume > 0) {
                pos += consume;
                rest -= consume;
                consume = 0;

                if (rest == 0)
                    state = SEARCH;

                if (from + pos == to) {
                    pos = -rest;
                    rest = 0;
                    return false;
                }

                assert(state == SEARCH);
                return true;
            }

            int idx = from + pos;
            assert(idx <= to);

            if (idx == to) {    // continue with next reservation
                pos = -rest;
                rest = 0;
                return false;
            }

            consume = std::min(to - idx, rest);
            return false;
        }

        inline bool check_id(int from, int to)
        {
            assert(state == CHECK_ID);
            int idx = from + pos;
            assert(idx <= to);

            if (idx == to) {    // continue with next reservation
                pos = -rest;
                rest = 0;
                return false;
            }

            if (content[idx].packet_id.count != pkgid)
                throw RuntimeException{std::string{"unable to handle reordered chunk, expected id "} + std::to_string(pkgid) + ", but got id " + std::to_string(content[pos].packet_id.count)};

            pkgid += 1;
            pos += 1;
            state = DATA;
            return data(from, to);
        }

        inline bool search_pkg(int from, int to)
        {
            // pos points to package header
            assert((state == SEARCH) && (rest == 0));
            int idx = from + pos;
            assert(idx < to);

            do {
                if (content[idx].header.id != AsiRawStreamDecoder::chunk_id)
                    throw RuntimeException{"expected header has no TPX3 id"};
                if (content[idx].header.size % event_size != 0)
                    throw RuntimeException{"chunk size not a multiple of the event size"};

                const int size = content[idx].header.size / event_size;

                #if SERVER_VERSION >= 320
                    if (size < 2)
                #else
                    if (size < 1)
                #endif
                        throw RuntimeException{"encountered bogus chunk size"};

                if (content[idx].header.chip == chip) {
                    pos += 1;
                    #if SERVER_VERSION >= 320
                        rest = size - 1;
                        state = CHECK_ID;
                        return check_id(from, to);
                    #else
                        state = DATA;
                        return data(from, to);
                    #endif            
                }
            
                pos += size + 1;
                idx += size + 1;
            } while (idx < to);

            pos = idx - to;
            return false;
        }

        inline void update(const iobuf::reservation_t& res)
        {
            assert(res.jar);
            const int rstart = res.start / event_size;
            const int rend = res.end / event_size;
            const int amount = rend - rstart;
            assert((rstart >= 0) && (amount > 0) && (rend <= (int)(iobuf::container_size / event_size)));
            content = (AsiRawStreamDecoder::Event*)res.jar->container.data;
            bool more = false;

            do {
                switch (state) {
                case INIT:
                    pos = rstart;
                    rest = 0;
                    consume = 0;
                    state = SEARCH;
                    [[fallthrough]];
                case SEARCH:
                    assert(content && (pos >= 0));
                    if (pos >= amount) {
                        pos -= amount;
                        return;
                    }
                    more = search_pkg(rstart, rend);
                    break;
                case CHECK_ID:
                    if (pos < 0) {  // continue from last reservation
                        rest = -pos;
                        pos = 0;
                    }
                    more = check_id(rstart, rend);
                    break;
                case DATA:
                    if (pos < 0) {  // continue from last reservation
                        rest = -pos;
                        pos = 0;
                    }
                    more = data(rstart, rend);
                    break;
                default:
                    assert(false);
                }
            } while (more);
        }
    }; // struct subreservation
} // namespace iobuf

#endif // ifndef SUBRESERVATION_H