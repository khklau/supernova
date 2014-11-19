#ifndef SIMULATION_GRID_GRID_DB_LOG_MMAP_HXX
#define SIMULATION_GRID_GRID_DB_LOG_MMAP_HXX

#include "log_mmap.hpp"
#include "log_memory.hxx"
#include <boost/filesystem/operations.hpp>
#include <simulation_grid/grid_db/exception.hpp>

namespace bfs = boost::filesystem;
namespace bip = boost::interprocess;

namespace simulation_grid {
namespace grid_db {

template <class entry_t>
log_mmap_reader<entry_t>::log_mmap_reader(const bfs::path& path)
try :
    file_(path.string().c_str(), bip::read_only),
    region_(file_, bip::read_only, 0U, bfs::file_size(path)),
    reader_handle_(region_)
{
}
catch (grid_db_condition& cond)
{
    cond << info_db_identity(path.string());
    throw cond;
}
catch (grid_db_error& err)
{
    err << info_db_identity(path.string());
    throw err;
}

template <class entry_t>
log_mmap_reader<entry_t>::~log_mmap_reader()
{ }

template <class entry_t>
boost::optional<const entry_t&> log_mmap_reader<entry_t>::read(const log_index& index) const
{
    return reader_handle_.read(index);
}

template <class entry_t>
boost::optional<log_index> log_mmap_reader<entry_t>::get_front_index() const
{
    return reader_handle_.get_front_index();
}

template <class entry_t>
boost::optional<log_index> log_mmap_reader<entry_t>::get_back_index() const
{
    return reader_handle_.get_back_index();
}

template <class entry_t>
log_index log_mmap_reader<entry_t>::get_max_index() const
{
    return reader_handle_.get_max_index();
}

const bfs::path& init_file(const bfs::path& path, std::size_t size);

template <class entry_t>
log_mmap_owner<entry_t>::log_mmap_owner(const bfs::path& path, std::size_t size)
try :
    exists_(bfs::exists(path)),
    flock_(init_file(path, size).string().c_str()),
    slock_(flock_),
    file_(path.string().c_str(), bip::read_write),
    region_(file_, bip::read_write, 0U, bfs::file_size(path)),
    owner_handle_(exists_ ? open_existing : open_new, region_),
    reader_handle_(region_)
{
    region_.flush();
}
catch (grid_db_condition& cond)
{
    cond << info_db_identity(path.string());
    throw cond;
}
catch (grid_db_error& err)
{
    err << info_db_identity(path.string());
    throw err;
}

template <class entry_t>
log_mmap_owner<entry_t>::~log_mmap_owner()
{ }

template <class entry_t>
boost::optional<log_index> log_mmap_owner<entry_t>::append(const entry_t& entry)
{
    return owner_handle_.append(entry);
}

template <class entry_t>
boost::optional<const entry_t&> log_mmap_owner<entry_t>::read(const log_index& index) const
{
    return reader_handle_.read(index);
}

template <class entry_t>
boost::optional<log_index> log_mmap_owner<entry_t>::get_front_index() const
{
    return reader_handle_.get_front_index();
}

template <class entry_t>
boost::optional<log_index> log_mmap_owner<entry_t>::get_back_index() const
{
    return reader_handle_.get_back_index();
}

template <class entry_t>
log_index log_mmap_owner<entry_t>::get_max_index() const
{
    return reader_handle_.get_max_index();
}

} // namespace grid_db
} // namespace simulation_grid

#endif
