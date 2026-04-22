#pragma once

#ifndef SUBRESERVATION_H
#define SUBRESERVATION_H

/*!
\file
Provide subreservation within reservation functionality
*/
#include <cstring>

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
            DATA        //!< Return data range
        };

      private:
        collection_t& buffers;                          //!< I/O buffer collection
        reservation_t reservation;                      //!< Underlying reservation
        u64 pkgid;                                      //!< Next expected pkg id
        int end;                                       //!< Reservation end offset
        state_t state;                                  //!< Current parser state

      public:
        const AsiRawStreamDecoder::Event* content;      //!< Event data null->initial subreservation
        int pos;                                        //!< Current position in reservation
        int rest;                                       //!< 0: no more data
        int consume;                                    //!< Amount to consume

      private:
        void update_reservation()
        {
            reservation = buffers.read_reservation(reservation);
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
        \param from From this position within the reservation
        \param to Up to this position within the reservation
        \return Continue within same reservation
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
        \brief Check packet id

        Use pos to store rest accross reservation
        \param from From this position within the reservation
        \param to Up to this position within the reservation
        \return Continue within same reservation
        */
        inline bool check_id()
        {
            assert(state == CHECK_ID);
            if (pos >= end)
                return true;    // continue with next reservation

            if (content[pos].packet_id.count != pkgid)
                throw RuntimeException{std::string{"unable to handle reordered chunk, expected id "} + std::to_string(pkgid) + ", but got id " + std::to_string(content[pos].packet_id.count)};

            pkgid += 1;
            pos += 1;
            state = DATA;
            return data();
        }

        /*!
        \brief Look for packet header
        \param from From this position within the reservation
        \param to Up to this position within the reservation
        \return Continue within same reservation
        */
        inline bool search_pkg()
        {
            // pos points to package header
            assert((state == SEARCH) && (pos < end));

            do {
                if (content[pos].header.id != AsiRawStreamDecoder::chunk_id)
                    throw RuntimeException{"expected header has no TPX3 id"};
                if (content[pos].header.size % event_size != 0)
                    throw RuntimeException{"chunk size not a multiple of the event size"};

                const int size = content[pos].header.size / event_size;

                #if SERVER_VERSION >= 320
                    if (size < 2)
                #else
                    if (size < 1)
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
              pkgid{0ul}, end{0}, state{INIT},
              content{nullptr}, pos{0}, rest{0}, consume{0}
        {
            update_reservation();
        }

        /*!
        \brief Move constructor
        */
        inline subreservation_t(subreservation_t&&) noexcept = default;

        /*!
        \brief Move assignment
        \param other Temporary object
        \return Subreservation
        */
        subreservation_t& operator=(subreservation_t&& other) noexcept
        {
            std::memcpy((void*)this, &other, sizeof(*this));
            return *this;
        }

        /*!
        \brief Update subreservation
        
        Set rest to 0 if all data in the reservat in reservationion has been consumed
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
                default:
                    assert(false);
                }
            } while (more);
        }

        /*!
        \brief Reset subreservation

        For debugging, state is set to INIT
        \param pos_ New pos
        \param rest_ New rest
        \param consume_ New consume
        */
        void reset(int pos_, int rest_, int consume_) noexcept
        {
            pos = pos_;
            rest = rest_;
            consume = consume_;
            state = INIT;
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