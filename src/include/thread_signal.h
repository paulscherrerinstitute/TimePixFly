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

namespace thread_signal {

    static constexpr bool with_shutdown = true;
    static constexpr bool no_shutdown = false;

    class base {
      protected:
        std::condition_variable cond;   //!< Signal sent condition
        std::mutex lock;                //!< Potect signal

      public:
        void notify_one()
        {
            cond.notify_one();
        }
    };

    /*!
    \brief Signal from single thread
    \tparam with_shutdown Support shutdown signal
    */
    template<bool with_shutdown=false>
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
                sig->notify_one();
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

        friend single<true>;
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
            notify_one();
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

    class multi final : base {
        unsigned sent = 0u;
        const unsigned nthreads;

    public:
        inline explicit multi(unsigned num_threads) noexcept
            : nthreads{num_threads}
        {}

        inline void send() noexcept
        {
            std::lock_guard lck{lock};
            if (++sent == nthreads)
                notify_one();
        }

        inline void wait_reset() noexcept
        {
            std::unique_lock lck{lock};
            while (sent != nthreads)
                cond.wait(lck);
            sent = 0u;
        }
    };

} // namespace signal

#endif // THREAD_SGINLA_H