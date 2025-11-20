#pragma once

#ifndef ALIGNED_ALLOCATOR_H
#define ALIGNED_ALLOCATOR_H

#include <cstdlib>


inline static constexpr size_t memory_alignment = 256u/8u;  //!< Memory alignment for AVX2

/*!
\brief Custom allocator
Allocates memory aligned (to memory_alignment)
\tparam T Type of allocated elements
*/
template<typename T>
struct aligned_allocator {
    typedef T value_type;                   //!< Element type of memory

    inline aligned_allocator() = default;   //!< Default constructor

    /*!
    \brief Copy constructor
    \tparam U Element type of other constructor
    */
    template<typename U>
    inline constexpr aligned_allocator(const aligned_allocator<U>&) noexcept {}

    /*!
    \brief Allocation function
    Allocates aligned memory (to memory_alignment)
    \param n Memory size in elements
    \return Pointer to first element
    */
    inline T* allocate(std::size_t n) const {
        return static_cast<T*>(std::aligned_alloc(memory_alignment, n * sizeof(T)));
    }

    /*!
    \brief Deallocate memory
    \param p Pointer to first element
    */
    inline void deallocate(T* p, std::size_t) const {
        std::free(p);
    }
};

#endif // ALIGNED_ALLOCATOR_H