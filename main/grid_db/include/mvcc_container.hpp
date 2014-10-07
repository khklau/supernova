#ifndef SIMULATION_GRID_GRID_DB_MVCC_CONTAINER_HPP
#define SIMULATION_GRID_GRID_DB_MVCC_CONTAINER_HPP

#include <string>
#include <limits>
#include <boost/cstdint.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/segment_manager.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/lockfree/policies.hpp>
#include <boost/lockfree/queue.hpp>
#include <boost/noncopyable.hpp>
#include <boost/optional.hpp>
#include <simulation_grid/grid_db/about.hpp>
#include "role.hpp"

namespace simulation_grid {
namespace grid_db {

typedef boost::uint16_t reader_token_id;
typedef boost::uint16_t writer_token_id;

static const size_t MVCC_READER_LIMIT = (1 << std::numeric_limits<reader_token_id>::digits) - 4; // due to boost::lockfree limit
static const size_t MVCC_WRITER_LIMIT = 1;
static const size_t MVCC_MAX_KEY_LENGTH = 31;

class mvcc_container
{
public:
    static const version MIN_SUPPORTED_VERSION;
    static const version MAX_SUPPORTED_VERSION;
    mvcc_container(const owner_t, const boost::filesystem::path& path, size_t size);
    mvcc_container(const reader_t, const boost::filesystem::path& path);
    template <class element_t> const element_t* find_const(const char* key) const;
    template <class element_t> element_t* find_mut(const char* key);
    std::size_t available_space() const;
    const boost::filesystem::path& get_path() const { return path_; }
    boost::interprocess::managed_mapped_file& get_file() { return file_; }
private:
    bool exists_;
    const boost::filesystem::path path_;
    boost::interprocess::managed_mapped_file file_;
};

struct mvcc_reader_handle : private boost::noncopyable
{
    mvcc_reader_handle(mvcc_container& container);
    ~mvcc_reader_handle();
    mvcc_container& container;
    const reader_token_id token_id;
};

struct mvcc_writer_handle : private boost::noncopyable
{
    mvcc_writer_handle(mvcc_container& container);
    ~mvcc_writer_handle();
    mvcc_container& container;
    const writer_token_id token_id;
};

class mvcc_reader : private boost::noncopyable
{
public:
    mvcc_reader(const boost::filesystem::path& path);
    ~mvcc_reader();
    template <class element_t> bool exists(const char* key) const;
    template <class element_t> const boost::optional<const element_t&> read(const char* key) const;
#ifdef SIMGRID_GRIDDB_MVCCCONTAINER_DEBUG
    reader_token_id get_reader_token_id() const;
    boost::uint64_t get_last_read_revision() const;
    template <class element_t> boost::uint64_t get_oldest_revision(const char* key) const;
    template <class element_t> boost::uint64_t get_newest_revision(const char* key) const;
#endif
private:
    mvcc_container container_;
    mvcc_reader_handle reader_handle_;
};

#ifdef SIMGRID_GRIDDB_MVCCCONTAINER_DEBUG
#include <vector>
#endif

class mvcc_owner : private boost::noncopyable
{
public:
    mvcc_owner(const boost::filesystem::path& path, std::size_t size);
    ~mvcc_owner();
    template <class element_t> bool exists(const char* key) const;
    template <class element_t> const boost::optional<const element_t&> read(const char* key) const;
    template <class element_t> void write(const char* key, const element_t& value);
    template <class element_t> void remove(const char* key);
    void process_read_metadata(reader_token_id from = 0, reader_token_id to = MVCC_READER_LIMIT);
    void process_write_metadata(std::size_t max_attempts = 0);
    std::string collect_garbage(std::size_t max_attempts = 0);
    std::string collect_garbage(const std::string& from, std::size_t max_attempts = 0);
    void flush();
#ifdef SIMGRID_GRIDDB_MVCCCONTAINER_DEBUG
    reader_token_id get_reader_token_id() const;
    boost::uint64_t get_last_read_revision() const;
    template <class element_t> boost::uint64_t get_oldest_revision(const char* key) const;
    template <class element_t> boost::uint64_t get_newest_revision(const char* key) const;
    boost::uint64_t get_global_oldest_revision_read() const;
    std::vector<std::string> get_registered_keys() const;
    template <class element_t> std::size_t get_history_depth(const char* key) const;
#endif
private:
    void flush_impl();
    mvcc_container container_;
    mvcc_writer_handle writer_handle_;
    mvcc_reader_handle reader_handle_;
    boost::interprocess::file_lock file_lock_;
};

} // namespace grid_db
} // namespace simulation_grid

#endif
