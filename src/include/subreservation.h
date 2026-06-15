#pragma once

#ifndef SUBRESERVATION_H
#define SUBRESERVATION_H

/*!
\file
Provide subreservation within reservation functionality
*/
#include <cstring>
#include <deque>

#include "decoder.h"
#include "io_buf.h"

using Poco::RuntimeException;

namespace iobuf {

    /*!
    \brief Subreservation

    Provide subranges related to a specific chip within a reservation
    */
    struct subreservation_t final {
        constexpr static int event_size = sizeof(u64);  //!< Size of an event
        const u64 chip;                                 //!< Fixed chip number

        /*!
        \brief Parser state
        */
        enum state_t {
            INIT,       //!< Uninitialized subreservation
            SEARCH,     //!< Look for packet header
            CHECK_ID,   //!< Check packet id
            DATA,       //!< Return data range
            STORE,      //!< Store package data
            RESTORE     //!< Restore package data
        };

      private:
        collection_t& buffers;                          //!< I/O buffer collection
        reservation_t reservation;                      //!< Underlying reservation
        u64 pkgid;                                      //!< Next expected pkg id
        int end;                                        //!< Reservation end offset
        state_t state;                                  //!< Current parser state

        /*!
        \brief Stored data state
        */
        struct store_t final {
            jar_t* jar;                                 //!< Jar
            int pos;                                    //!< Current position in reservation
            int rest;                                   //!< 0: no more data
            int consume;                                //!< Amount to consume
        };

        std::deque<store_t> store;                      //!< Stored package
        store_t current;                                //!< Current data state for restore operation
        jar_t* restored_jar;                            //!< Currently restored jar
        u64 stored_pkgid;                               //!< Id of stored package

      public:
        const AsiRawStreamDecoder::Event* content;      //!< Event data null->initial subreservation
        int pos;                                        //!< Current position in reservation
        int rest;                                       //!< 0: no more data
        int consume;                                    //!< Amount to consume

      private:
        /*!
        \brief Update underlying reservation
        */
        inline void update_reservation()
        {
            if (__builtin_expect(store.empty(), true))
                reservation = buffers.read_reservation(reservation);
            else
                reservation = buffers.read_reservation(reservation, store.back().jar == reservation.jar);
            auto rdata = (AsiRawStreamDecoder::Event*)reservation.jar->container.data;
            if (content != rdata)
                pos -= end;
            end = reservation.end / event_size;
            content = rdata;
            assert((pos >= 0) && (end <= (iobuf::container_size / event_size)));
        }

        /*!
        \brief Return range

        Use pos to store rest accross reservation
        \return Continue reservation update loop
        */
        inline bool data() noexcept
        {
            assert(state == DATA);

            if (consume > 0) {
                pos += consume;
                rest -= consume;
                consume = 0;

                if (rest == 0) {
                    state = SEARCH;
                    return true;
                }
            }

            if (pos >= end)
                return true;
            
            consume = std::min(end - pos, rest);
            return false;
        }

        /*!
        \brief Store data for later retrieval

        \return Continue subreservation update loop
        */
        inline bool store_data()
        {
            assert(state == STORE);
            do {
                consume = std::min(end - pos, rest);
                store.push_back({reservation.jar, pos, rest, consume});
                pos += consume;
                rest -= consume;

                if (rest == 0)
                    break;

                reservation = buffers.read_reservation(reservation, true);
                auto rdata = (AsiRawStreamDecoder::Event*)reservation.jar->container.data;
                if (content != rdata)
                    pos -= end;
                end = reservation.end / event_size;
                content = rdata;
                assert((pos >= 0) && (end <= (iobuf::container_size / event_size)));
            } while (true);

            consume = 0;
            state = SEARCH;
            return true;
        }

        /*!
        \brief Restore stored data
        \return Continue subreservation update loop
        */
        inline bool restore_data()
        {
            assert(state == RESTORE);
            if (store.empty()) {
                end = reservation.end / event_size;
                content = (AsiRawStreamDecoder::Event*)current.jar->container.data;
                pos = current.pos;
                rest = current.rest;
                consume = current.consume;
                current = {};
                state = CHECK_ID;
                restored_jar = nullptr;
                stored_pkgid = 0;
                return true;
            }

            const store_t restore = store.front();
            store.pop_front();

            if (restored_jar && (restored_jar != restore.jar))
                buffers.return_jar(restored_jar);

            end = restore.pos + restore.consume; // prevent update_reservation in update()
            restored_jar = restore.jar;
            content = (AsiRawStreamDecoder::Event*)restored_jar->container.data;
            pos = restore.pos;
            rest = restore.rest;
            consume = restore.consume;

            return false;
        }

        /*!
        \brief Check packet id

        Use pos to store rest accross reservation
        \return Continue subreservation update loop
        */
        inline bool check_id()
        {
            assert(state == CHECK_ID);
            if (pos >= end)
                return true;    // continue with next reservation

            if (__builtin_expect(content[pos].packet_id.count != pkgid, false)) {
                if (store.empty()) {
                    // no stored packages
                    stored_pkgid = content[pos].packet_id.count;
                    pos += 1;
                    state = STORE;
                    return store_data();
                } else if (stored_pkgid == pkgid) {
                    pkgid += 1;
                    stored_pkgid = 0;
                    current = {reservation.jar, pos, rest, consume};
                    state = RESTORE;
                    return restore_data();
                }
                throw RuntimeException{std::string{"unable to handle reordered chunk, expected id "} + std::to_string(pkgid) + ", but got id " + std::to_string(content[pos].packet_id.count)};
            }

            pkgid += 1;
            pos += 1;
            state = DATA;
            return data();
        }

        /*!
        \brief Look for packet header
        \return Continue subreservation update loop
        */
        inline bool search_pkg()
        {
            // pos points to package header
            assert((state == SEARCH) && (pos < end));

            do {
                if (__builtin_expect(content[pos].header.id != AsiRawStreamDecoder::chunk_id, false))
                    throw RuntimeException{"expected header has no TPX3 id"};
                if (__builtin_expect(content[pos].header.size % event_size != 0, false))
                    throw RuntimeException{"chunk size not a multiple of the event size"};

                const int size = content[pos].header.size / event_size;

                #if SERVER_VERSION >= 320
                    if (__builtin_expect(size < 2, false))
                #else
                    if (__builtin_expect(size < 1, false))
                #endif
                        throw RuntimeException{"encountered bogus chunk size"};

                if (content[pos].header.chip == chip) {
                    pos += 1;
                    #if SERVER_VERSION >= 320
                        rest = size - 1;
                        state = CHECK_ID;
                        return check_id();
                    #else
                        rest = size;
                        state = DATA;
                        return data();
                    #endif            
                }
            
                pos += size + 1;
            } while (pos < end);

            return true;
        }

      public:
        /*!
        \brief Initializer
        \param bufs I/O buffer collection
        \param chip_no Provide subranges for this chip
        */
        inline subreservation_t(collection_t& bufs, u64 chip_no)
            : chip{chip_no},
              buffers(bufs), reservation{initial_reservation},
              pkgid{}, end{}, state{INIT}, current{}, restored_jar{}, stored_pkgid{},
              content{nullptr}, pos{}, rest{}, consume{}
        {
            update_reservation();
        }

        /*!
        \brief Move constructor
        */
        inline subreservation_t(subreservation_t&&) noexcept = default;

        subreservation_t& operator=(subreservation_t&& other) noexcept = delete;

        /*!
        \brief Update subreservation
        
        Set rest to 0 if all data in the reservation has been consumed
        */
        inline void update()
        {
            bool more = false;

            do {
                while (pos >= end) {
                    update_reservation();   // modifies pos and end
                    if (! end) {
                        if (rest)
                            throw RuntimeException{"premature end of data package"};
                        return;
                    }
                }

                switch (state) {
                case INIT:
                    assert((pos == 0) && (rest == 0) && (consume == 0) && (end > 0));
                    state = SEARCH;
                    [[fallthrough]];
                case SEARCH:
                    assert(content && (pos >= 0) && (rest == 0));
                    more = search_pkg();
                    break;
                case CHECK_ID:
                    assert(content && (pos >= 0) && (rest > 1));
                    more = check_id();
                    break;
                case DATA:
                    assert(content && (pos >= 0) && (rest > 0));
                    more = data();
                    break;
                case STORE:
                    more = store_data();
                    break;
                case RESTORE:
                    more = restore_data();
                    break;
                default:
                    assert(false);
                }
            } while (more);
        }

        /*!
        \brief Reset subreservation

        For debugging
        \param state_ New state
        \param pos_ New pos
        \param rest_ New rest
        \param consume_ New consume
        */
        void reset(state_t state_, int pos_, int rest_, int consume_) noexcept
        {
            state = state_;
            store.clear();
            current = {};
            restored_jar = {};
            stored_pkgid = {};
            pos = pos_;
            rest = rest_;
            consume = consume_;
        }

        /*!
        \brief Get state

        For debugging
        \return State
        */
        state_t get_state() const noexcept
        {
            return state;
        }
    }; // struct subreservation
} // namespace iobuf

#endif // ifndef SUBRESERVATION_H