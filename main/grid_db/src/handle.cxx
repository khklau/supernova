#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <simulation_grid/grid_db/header.pb.h>
#include <simulation_grid/grid_db/exception.hpp>
#include "handle.hpp"

using boost::interprocess::shared_memory_object;
using boost::interprocess::mapped_region;
using boost::interprocess::open_only;
using boost::interprocess::read_only;
using simulation_grid::grid_db::header;

namespace simulation_grid {
namespace grid_db {

class read_handle_impl
{
public:
    read_handle_impl(const std::string& db_id) :
	    db_id_(db_id),
	    shmem_(open_only, db_id.c_str(), read_only), region_()
    {
	region_ = mapped_region(shmem_, read_only);
    }

    ~read_handle_impl() { }

    const header& get_db_header() const
    {
	const header* address = static_cast<const header*>(region_.get_address());
	if (!address)
	{
	    throw handle_error("Could not get address of header") << info_db_id(db_id_);
	}
	return *address;
    }

private:
    std::string db_id_;
    shared_memory_object shmem_;
    mapped_region region_;
};

read_handle::read_handle(const std::string& db_id) :
	impl_(0)
{
    impl_ = new read_handle_impl(db_id);
}

read_handle::~read_handle()
{
    delete impl_;
}

const header& read_handle::get_db_header() const
{
    return impl_->get_db_header();
}

} // namespace grid_db
} // namespace simulation_grid
