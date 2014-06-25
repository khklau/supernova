#ifndef SIMULATION_GRID_GRID_DB_MMAP_CONTAINER_HXX
#define SIMULATION_GRID_GRID_DB_MMAP_CONTAINER_HXX

#include <cstring>
#include <boost/bind.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/function.hpp>
#include <boost/interprocess/creation_tags.hpp>
#include <boost/optional.hpp>
#include <boost/ref.hpp>
#include <simulation_grid/core/compiler_extensions.hpp>
#include <simulation_grid/grid_db/exception.hpp>
#include "multi_reader_ring_buffer.hpp"
#include "multi_reader_ring_buffer.hxx"
#include "mmap_container.hpp"

namespace bip = boost::interprocess;
namespace bgr = boost::gregorian;

namespace {

using namespace simulation_grid::grid_db;

typedef boost::uint8_t history_depth;
typedef boost::uint64_t mvcc_revision;
typedef mmap_queue<reader_token_id, MVCC_READER_LIMIT>::type reader_token_list;
typedef mmap_queue<writer_token_id, MVCC_WRITER_LIMIT>::type writer_token_list;

static const size_t DEFAULT_HISTORY_DEPTH = 1 <<  std::numeric_limits<history_depth>::digits;

struct mvcc_mmap_header
{
    boost::uint16_t endianess_indicator;
    char file_type_tag[48];
    version container_version;
    boost::uint16_t header_size;
    mvcc_mmap_header();
};

struct mvcc_mmap_reader_token
{
    boost::optional<mvcc_revision> last_read_revision;
    boost::optional<bgr::date> last_read_timestamp;
};

struct mvcc_mmap_writer_token
{
    boost::optional<mvcc_revision> last_write_revision;
    boost::optional<bgr::date> last_write_timestamp;
    boost::optional<mvcc_revision> last_flushed_revision;
    boost::optional<bgr::date> last_flush_timestamp;
};

struct mvcc_mmap_resource_pool
{
    mvcc_mmap_reader_token reader_token_pool[MVCC_READER_LIMIT];
    mvcc_mmap_writer_token writer_token_pool[MVCC_WRITER_LIMIT];
    bip::allocator<reader_token_id, bip::managed_mapped_file::segment_manager> reader_allocator;
    bip::allocator<writer_token_id, bip::managed_mapped_file::segment_manager> writer_allocator;
    bip::offset_ptr<reader_token_list> reader_free_list;
    bip::offset_ptr<writer_token_list> writer_free_list;
    mvcc_mmap_resource_pool(bip::managed_mapped_file* file);
};

template <class value_t>
struct mvcc_record
{
    value_t value;
    mvcc_revision revision;
    bgr::date timestamp;
};

template <class content_t>
struct mmap_ring_buffer
{
    typedef typename mmap_allocator<content_t>::type allocator_t;
    typedef multi_reader_ring_buffer<content_t, allocator_t> type;
};

template <class value_t>
const value_t* find_const(const mvcc_mmap_container& container, const bip::managed_mapped_file::char_type* key)
{
    // Unfortunately boost::interprocess::managed_mapped_file::find is not a const function
    // due to use of internal locks which were not declared as mutable, so this function
    // has been provided to fake constness
    return const_cast<mvcc_mmap_container&>(container).file.find<value_t>(key).first;
}

template <class value_t>
value_t* find_mut(mvcc_mmap_container& container, const bip::managed_mapped_file::char_type* key)
{
    return container.file.find<value_t>(key).first;
}

const mvcc_mmap_header& const_header(const mvcc_mmap_container& container);
mvcc_mmap_header& mut_header(mvcc_mmap_container& container);
const mvcc_mmap_resource_pool& const_resource_pool(const mvcc_mmap_container& container);
mvcc_mmap_resource_pool& mut_resource_pool(const mvcc_mmap_container& container);

template <class element_t>
bool exists(const mvcc_mmap_reader_handle& handle, const char* key)
{
    typedef typename mmap_ring_buffer< mvcc_record<element_t> >::type value_t;
    const value_t* ringbuf = find_const<value_t>(handle.get_container(), key);
    return ringbuf && !ringbuf->empty();
}

template <class element_t>
const mvcc_record<element_t>& read_newest(const mvcc_mmap_reader_handle& handle, const char* key)
{
    typedef typename mmap_ring_buffer< mvcc_record<element_t> >::type value_t;
    const value_t* ringbuf = find_const<value_t>(handle.get_container(), key);
    if (UNLIKELY_EXT(!ringbuf || ringbuf->empty()))
    {
	throw malformed_db_error("Could not find data")
		<< info_db_identity(handle.get_container().path.string())
		<< info_component_identity("mvcc_mmap_reader")
		<< info_data_identity(key);
    }
    const mvcc_record<element_t>& record = ringbuf->front();
    // the mvcc_mmap_reader_token will ensure the returned reference remains valid
    mut_resource_pool(handle.get_container()).reader_token_pool[handle.get_token_id()].
	    last_read_timestamp = record.timestamp;
    mut_resource_pool(handle.get_container()).reader_token_pool[handle.get_token_id()].
	    last_read_revision = record.revision;
    return record;
}

template <class element_t>
const mvcc_record<element_t>& read_oldest(const mvcc_mmap_reader_handle& handle, const char* key)
{
    typedef typename mmap_ring_buffer< mvcc_record<element_t> >::type value_t;
    const value_t* ringbuf = find_const<value_t>(handle.get_container(), key);
    if (UNLIKELY_EXT(!ringbuf || ringbuf->empty()))
    {
	throw malformed_db_error("Could not find data")
		<< info_db_identity(handle.get_container().path.string())
		<< info_component_identity("mvcc_mmap_reader")
		<< info_data_identity(key);
    }
    return ringbuf->front();
}

} // anonymous namespace

namespace simulation_grid {
namespace grid_db {

template <class element_t>
bool mvcc_mmap_reader::exists(const char* key) const
{
    return exists<element_t>(reader_handle_, key);
}

template <class element_t>
const element_t& mvcc_mmap_reader::read(const char* key) const
{
    return read_newest<element_t>(reader_handle_, key).value;
}

template <class element_t>
bool mvcc_mmap_owner::exists(const char* key) const
{
    return exists<element_t>(reader_handle_, key);
}

template <class element_t>
const element_t& mvcc_mmap_owner::read(const char* key) const
{
    return read_newest<element_t>(reader_handle_, key).value;
}

} // namespace grid_db
} // namespace simulation_grid

#endif
