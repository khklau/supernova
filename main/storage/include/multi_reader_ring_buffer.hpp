#ifndef SUPERNOVA_STORAGE_MULTI_READER_RING_BUFFER_HPP
#define SUPERNOVA_STORAGE_MULTI_READER_RING_BUFFER_HPP

#include <memory>
#include <boost/noncopyable.hpp>
#include <boost/interprocess/interprocess_fwd.hpp>
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/sync/interprocess_sharable_mutex.hpp>
#include <boost/circular_buffer.hpp>

namespace supernova {
namespace storage {

template <class element_t, class allocator_t>
class multi_reader_ring_buffer : private boost::noncopyable
{
public:
    typedef typename allocator_t::reference element_ref_t;
    typedef typename allocator_t::const_reference const_element_ref_t;
    typedef typename allocator_t::size_type size_type;
    typedef boost::interprocess::sharable_lock<boost::interprocess::interprocess_sharable_mutex> read_lock;
    typedef boost::interprocess::scoped_lock<boost::interprocess::interprocess_sharable_mutex> write_lock;
    multi_reader_ring_buffer(size_type capacity, const allocator_t& allocator);
    const_element_ref_t front() const;
    void push_front(const_element_ref_t element);
    const_element_ref_t back() const;
    void pop_back(const_element_ref_t back_element);
    void grow(size_type new_capacity);
    size_type capacity() const;
    size_type element_count() const;
    bool empty() const;
    bool full() const;
private:
    boost::interprocess::interprocess_sharable_mutex mutex_;
    boost::circular_buffer<element_t, allocator_t> ringbuf_;
};

// TODO : replace the following with type aliases after moving to a C++11 compiler

template <class element_t>
struct mmap_multi_reader_ring_buffer
{
    typedef multi_reader_ring_buffer<element_t, boost::interprocess::managed_mapped_file> type;
};

template <class element_t>
struct shmem_multi_reader_ring_buffer
{
    typedef multi_reader_ring_buffer<element_t, boost::interprocess::managed_shared_memory> type;
};

} // namespace storage
} // namespace supernova

#endif
