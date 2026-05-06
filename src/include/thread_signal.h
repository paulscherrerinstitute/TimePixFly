#pragma once

#ifndef THREAD_SIGNAL_H
#define THREAD_SIGNAL_H

/*!
\file
Provide inter thread signals
*/

#include <condition_variable>
#include <mutex>
#include <atomic>
#include <vector>
#include <climits>
#include <cassert>

namespace thread_signal {

    template<bool type> class multi;

    template <typename T>
    constexpr unsigned bit_size(const T& value) noexcept
    {
        return sizeof(value) * CHAR_BIT;
    }

    static constexpr bool with_shutdown = true;
    static constexpr bool no_shutdown = false;
    static constexpr bool send = true;
    static constexpr bool reset_with_shutdown = false;

    class base {
      protected:
        std::condition_variable cond;   //!< Signal sent condition
        std::mutex lock;                //!< Potect signal

      public:
        void notify_one()
        {
            cond.notify_one();
        }

        void notify_all()
        {
            cond.notify_all();
        }
    };

    /*!
    \brief Signal from single thread
    \tparam kind With, or no shutdown
    */
    template<bool kind=with_shutdown>
    class single final : base {
        std::vector<base*> dep;             //!< Dependent signals
        std::atomic_bool signal = false;    //!< Signal flag

    public:
        single() noexcept
        {
            dep.push_back(this);
        }

        /*!
        \brief Send signal

        Wait for threads that read the signal before reset.
        */
        inline void send() noexcept
        {
            signal = true;
            for (auto* sig : dep)
                sig->notify_all();
        }
        
        /*!
        \brief Wait for and reset the signal

        Wait for threads that read the signal before reset.
        */
        inline void wait_reset() noexcept
        {
            std::unique_lock lck{lock};
            while (!signal)
                cond.wait(lck);
            signal = false;
        }

        /*!
        \brief Wait for signal
        */
        inline void wait() noexcept
        {
            std::unique_lock lck{lock};
            while (!signal)
                cond.wait(lck);
        }

        /*!
        \brief Reset signal

        Wait for threads that read the signal before reset.
        */
        inline void reset() noexcept
        {
            signal = false;
        }

        friend single<with_shutdown>;
        friend multi<thread_signal::reset_with_shutdown>;
    };

    template<>
    class single<true> final : base {
        single<false>& shutdown;        //!< Shutdown signal
        bool signal = false;            //!< Signal flag

    public:
        /*!
        \brief Signal from single thread
        \param shutdown_signal 
        */
        inline explicit single(single<false>& shutdown_signal) noexcept
            : shutdown{shutdown_signal}
        {
            shutdown.dep.push_back(this);
        }

        /*!
        \brief Send signal

        Wait for thread that reads signal before reset.
        */
        inline void send() noexcept
        {
            std::lock_guard lck{lock};
            signal = true;
            notify_all();
        }
        
        /*!
        \brief Wait for and reset the signal
        \return True if shutdown signal was sent
        */
        inline bool wait_reset() noexcept
        {
            std::unique_lock lck{lock};
            bool ss;
            {
                while (!signal && !(ss = shutdown.signal))
                    cond.wait(lck);
            }
            signal = false;
            return ss;
        }

        /*!
        \brief Wait for signal
        \return True if shutdown signal was sent
        */
        inline bool wait() noexcept
        {
            std::unique_lock lck{lock};
            bool ss;
            while (!signal && !(ss = shutdown.signal))
                cond.wait(lck);
            return ss;
        }

        /*!
        \brief Reset signal

        Wait for thread that reads signal before reset.
        */
        inline void reset() noexcept
        {
            signal = false;
        }
    };

    template<bool kind=send>
    class multi final : base {
        const unsigned nthreads;
        std::atomic<unsigned> sendcnt = 0u;
        bool signal = false;

    public:
        inline explicit multi(unsigned num_threads) noexcept
            : nthreads{num_threads}
        {}

        inline void send() noexcept
        {
            if (++sendcnt == nthreads) {
                std::lock_guard lck{lock};
                signal = true;
                notify_all();
            }
        }

        inline void wait_reset() noexcept
        {
            std::unique_lock lck{lock};
            while (! signal)
                cond.wait(lck);
            signal = false;
            sendcnt = 0u;
        }

        inline void wait() noexcept
        {
            std::unique_lock lck{lock};
            while (! signal)
                cond.wait(lck);
        }

        inline void reset() noexcept
        {
            signal = false;
            sendcnt = 0u;
        }
    };

    template<>
    class multi<reset_with_shutdown> final : base {
        single<false>& shutdown;        //!< Shutdown signal
        const unsigned nthreads;
        unsigned signalbits = 0u;

    public:
        inline explicit multi(single<false>& shutdown_signal, unsigned num_threads) noexcept
            : shutdown{shutdown_signal}, nthreads{num_threads}
        {
            assert(num_threads <= bit_size(signalbits));
            shutdown.dep.push_back(this);
        }

        inline void send() noexcept
        {
            std::lock_guard lck{lock};
            signalbits = (1ul << nthreads) - 1ul;
            notify_all();
        }

        inline bool wait_reset(unsigned thread) noexcept
        {
            auto mask = 1u << thread;
            bool ss;
            std::unique_lock lck{lock};
            while (! (signalbits & mask) && !(ss = shutdown.signal))
                cond.wait(lck);
            signalbits ^= mask;
            return ss;
        }

        inline bool wait(unsigned thread) noexcept
        {
            auto mask = 1u << thread;
            bool ss;
            std::unique_lock lck{lock};
            while (! (signalbits & mask) && !(ss = shutdown.signal))
                cond.wait(lck);
            return ss;
        }

        inline void reset(unsigned thread) noexcept
        {
            auto mask = 1u << thread;
            signalbits &= ~mask;
        }
    };
} // namespace signal

#endif // THREAD_SGINLA_H