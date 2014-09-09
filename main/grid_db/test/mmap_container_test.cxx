#include <iostream>
#include <exception>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <boost/array.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/atomic.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/lockfree/queue.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/thread_time.hpp>
#include <gtest/gtest.h>
#include <zmq.hpp>
#include <simulation_grid/core/compiler_extensions.hpp>
#include <simulation_grid/core/process_utility.hpp>
#include "exception.hpp"
#include "mmap_container.hpp"
#include "mmap_container.hxx"
#include "container_msg.hpp"

namespace bas = boost::asio;
namespace bfs = boost::filesystem;
namespace bpt = boost::posix_time;
namespace scp = simulation_grid::core::process_utility;
namespace sgd = simulation_grid::grid_db;

namespace {

typedef boost::uint16_t port_t;

static const port_t DEFAULT_PORT = 22220U;
static const size_t DEFAULT_SIZE = 1 << 24;

namespace ipc {

enum type
{
    shm = 0,
    mmap
};

inline std::ostream& operator<<(std::ostream& out, const ipc::type& source)
{
    switch (source)
    {
	case ipc::shm:
	{
	    out << "shm";
	    break;
	}
	case ipc::mmap:
	{
	    out << "mmap";
	    break;
	}
	default: { }
    }
    return out;
}

inline std::istream& operator>>(std::istream& in, ipc::type& target)
{
    if (!in.good())
    {
	return in;
    }
    std::string tmp;
    in >> tmp;
    boost::algorithm::to_lower(tmp);
    std::string shm("shm");
    std::string mmap("mmap");
    if (tmp == shm)
    {
	target = ipc::shm;
    }
    else if (tmp == mmap)
    {
	target = ipc::mmap;
    }
    else
    {
	in.setstate(std::ios::failbit);
    }
    return in;
}

} // namespace ipc

struct config
{
    config() : ipc(ipc::shm), port(DEFAULT_PORT), size(DEFAULT_SIZE) { }
    config(ipc::type ipc_, const std::string& name_, port_t port_ = DEFAULT_PORT,
	    size_t size_ = DEFAULT_SIZE) :
    	ipc(ipc_), name(name_), port(port_), size(size_)
    { }
    ipc::type ipc;
    std::string name;
    port_t port;
    size_t size;
};

class service_client
{
public:
    service_client(const config& config);
    ~service_client();
    sgd::result_msg send(sgd::instruction_msg& msg);
    void send_terminate(boost::uint32_t sequence);
    void send_write_string(boost::uint32_t sequence, const char* key, const string_value& value);
    void send_write_struct(boost::uint32_t sequence, const char* key, const struct_value& value);
    void send_process_read_metadata(boost::uint32_t sequence, sgd::reader_token_id from = 0, sgd::reader_token_id to = sgd::MVCC_READER_LIMIT);
    void send_process_write_metadata(boost::uint32_t sequence, std::size_t max_attempts = 0);
    boost::uint64_t send_get_global_oldest_revision_read(boost::uint32_t sequence);
    std::vector<std::string> send_get_registered_keys(boost::uint32_t sequence);
private:
    bool terminate_sent_;
    static int init_zmq_socket(zmq::socket_t& socket, const config& config);
    zmq::context_t context_;
    zmq::socket_t socket_;
    bas::io_service service_;
    bas::posix::stream_descriptor stream_;
};

service_client::service_client(const config& config) :
    terminate_sent_(false),
    context_(1),
    socket_(context_, ZMQ_REQ),
    service_(),
    stream_(service_, init_zmq_socket(socket_, config))
{ }

service_client::~service_client()
{
    if (!terminate_sent_)
    {
	send_terminate(99U);
    }
    stream_.release();
    service_.stop();
    socket_.close();
    context_.close();
}

int service_client::init_zmq_socket(zmq::socket_t& socket, const config& config)
{
    std::string address(str(boost::format("tcp://127.0.0.1:%d") % config.port));
    socket.connect(address.c_str());

    // TODO this is currently POSIX specific, add a Windows version
    int fd = 0;
    size_t size = sizeof(fd);
    socket.getsockopt(ZMQ_FD, &fd, &size);
    if (UNLIKELY_EXT(size != sizeof(fd)))
    {
	throw std::runtime_error("Can't find ZeroMQ socket file descriptor");
    }
    return fd;
}

sgd::result_msg service_client::send(sgd::instruction_msg& msg)
{
    msg.serialize(socket_);
    int event = 0;
    size_t size = sizeof(event);
    boost::system::error_code error;
    sgd::result_msg result;
    do
    {
	//stream_.read_some(boost::asio::null_buffers(), error);
	if (UNLIKELY_EXT(error))
	{
	    throw std::runtime_error("Unexpected connection close");
	}
	socket_.getsockopt(ZMQ_EVENTS, &event, &size);
	if (UNLIKELY_EXT(size != sizeof(event)))
	{
	    throw std::runtime_error("Unable to read socket options");
	}
    } while (!(event & ZMQ_POLLIN));
    // Finally received a whole message
    sgd::result_msg::msg_status status = result.deserialize(socket_);
    if (UNLIKELY_EXT(status == sgd::result_msg::MALFORMED))
    {
	throw std::runtime_error("Received malformed message");
    }
    return result;
}

void service_client::send_terminate(boost::uint32_t sequence)
{
    sgd::instruction_msg inmsg;
    sgd::terminate_instr instr;
    instr.set_sequence(sequence);
    inmsg.set_terminate(instr);
    sgd::result_msg outmsg(send(inmsg));
    EXPECT_TRUE(outmsg.is_confirmation()) << "Unexpected terminate result";
    EXPECT_EQ(inmsg.get_terminate().sequence(), outmsg.get_confirmation().sequence()) << "Sequence number mismatch";
    terminate_sent_ = true;
}

void service_client::send_write_string(boost::uint32_t sequence, const char* key, const string_value& value)
{
    sgd::instruction_msg inmsg;
    sgd::write_string_instr instr;
    instr.set_sequence(sequence);
    instr.set_key(key);
    instr.set_value(value.c_str);
    inmsg.set_write_string(instr);
    sgd::result_msg outmsg(send(inmsg));
    EXPECT_TRUE(outmsg.is_confirmation()) << "unexpected write result";
    EXPECT_EQ(inmsg.get_write_string().sequence(), outmsg.get_confirmation().sequence()) << "sequence number mismatch";
}

void service_client::send_write_struct(boost::uint32_t sequence, const char* key, const struct_value& value)
{
    sgd::instruction_msg inmsg;
    sgd::write_struct_instr instr;
    instr.set_sequence(sequence);
    instr.set_key(key);
    instr.set_value1(value.value1);
    instr.set_value2(value.value2);
    instr.set_value3(value.value3);
    inmsg.set_write_struct(instr);
    sgd::result_msg outmsg(send(inmsg));
    EXPECT_TRUE(outmsg.is_confirmation()) << "unexpected write result";
    EXPECT_EQ(inmsg.get_write_struct().sequence(), outmsg.get_confirmation().sequence()) << "sequence number mismatch";
}

void service_client::send_process_read_metadata(boost::uint32_t sequence, sgd::reader_token_id from, sgd::reader_token_id to)
{
    sgd::instruction_msg inmsg;
    sgd::process_read_metadata_instr instr;
    instr.set_sequence(sequence);
    instr.set_from(from);
    instr.set_to(to);
    inmsg.set_process_read_metadata(instr);
    sgd::result_msg outmsg(send(inmsg));
    EXPECT_TRUE(outmsg.is_confirmation()) << "unexpected process_read_metadata result";
    EXPECT_EQ(inmsg.get_process_read_metadata().sequence(), outmsg.get_confirmation().sequence()) << "sequence number mismatch";
}

void service_client::send_process_write_metadata(boost::uint32_t sequence, std::size_t max_attempts)
{
    sgd::instruction_msg inmsg;
    sgd::process_write_metadata_instr instr;
    instr.set_sequence(sequence);
    instr.set_max_attempts(max_attempts);
    inmsg.set_process_write_metadata(instr);
    sgd::result_msg outmsg(send(inmsg));
    EXPECT_TRUE(outmsg.is_confirmation()) << "unexpected process_read_metadata result";
    EXPECT_EQ(inmsg.get_process_write_metadata().sequence(), outmsg.get_confirmation().sequence()) << "sequence number mismatch";
}

boost::uint64_t service_client::send_get_global_oldest_revision_read(boost::uint32_t sequence)
{
    sgd::instruction_msg inmsg;
    sgd::get_global_oldest_revision_read_instr instr;
    instr.set_sequence(sequence);
    inmsg.set_get_global_oldest_revision_read(instr);
    sgd::result_msg outmsg(send(inmsg));
    EXPECT_TRUE(outmsg.is_revision()) << "unexpected get_global_oldest_revision_read result";
    EXPECT_EQ(inmsg.get_get_global_oldest_revision_read().sequence(), outmsg.get_revision().sequence()) << "sequence number mismatch";
    return outmsg.get_revision().revision();
}

std::vector<std::string> service_client::send_get_registered_keys(boost::uint32_t sequence)
{
    sgd::instruction_msg inmsg;
    sgd::get_registered_keys_instr instr;
    instr.set_sequence(sequence);
    inmsg.set_get_registered_keys(instr);
    sgd::result_msg outmsg(send(inmsg));
    EXPECT_TRUE(outmsg.is_key_list()) << "unexpected get_registered_keys result";
    EXPECT_EQ(inmsg.get_get_registered_keys().sequence(), outmsg.get_key_list().sequence()) << "sequence number mismatch";
    std::vector<std::string> result;
    for (int iter = 0; iter < outmsg.get_key_list().key_list().size(); ++iter)
    {
	result.push_back(outmsg.get_key_list().key_list().Get(iter));
    }
    return result;
}

class service_launcher
{
public:
    service_launcher(const config& config);
    ~service_launcher();
    int wait();
private:
    bool has_terminated;
    pid_t pid_;
};

service_launcher::service_launcher(const config& config) :
    has_terminated(false)
{
    static const char SLAVE_NAME[] = "mmap_container_service";
    boost::array <char, sizeof(SLAVE_NAME)> launcher_name;
    strncpy(launcher_name.c_array(), SLAVE_NAME, launcher_name.max_size());

    static const char PORT_OPT[] = "--port";
    boost::array<char, sizeof(PORT_OPT)> port_opt;
    strncpy(port_opt.c_array(), PORT_OPT, port_opt.max_size());

    boost::array<char, std::numeric_limits<port_t>::digits> port_arg;
    std::ostringstream port_buf;
    port_buf << config.port;
    strncpy(port_arg.c_array(), port_buf.str().c_str(), port_arg.max_size());

    static const char SIZE_OPT[] = "--size";
    boost::array<char, sizeof(SIZE_OPT)> size_opt;
    strncpy(size_opt.c_array(), SIZE_OPT, size_opt.max_size());

    boost::array<char, std::numeric_limits<size_t>::digits> size_arg;
    std::ostringstream size_buf;
    size_buf << config.size;
    strncpy(size_arg.c_array(), size_buf.str().c_str(), size_arg.max_size());

    // TODO: calculate the array size properly with constexpr after moving to C++11
    boost::array<char, 256> ipc_arg;
    std::ostringstream ipc_buf;
    ipc_buf << config.ipc;
    strncpy(ipc_arg.c_array(), ipc_buf.str().c_str(), ipc_arg.max_size());

    std::vector<char> name_arg(config.name.size() + 1, '\0');
    std::copy(config.name.begin(), config.name.end(), name_arg.begin());

    boost::array<char*, 9> arg_list = boost::assign::list_of
    		(launcher_name.c_array())
		(port_opt.c_array())(port_arg.c_array())
		(size_opt.c_array())(size_arg.c_array())
		(ipc_arg.c_array())
		(&name_arg[0])
		(0);
    bfs::path launcher_path = scp::current_exe_path().parent_path() / bfs::path(SLAVE_NAME);
    // stupid arg_list argument has to be an array of mutable C strings
    if (posix_spawn(&pid_, launcher_path.string().c_str(), 0, 0, arg_list.c_array(), environ))
    {
	throw std::runtime_error("Could not spawn service_launcher");
    }
    bfs::path ipcpath(config.name);
    if (config.ipc == ipc::shm)
    {
	ipcpath = bfs::path("/dev/shm") / bfs::path(config.name);
    }
    while (!bfs::exists(ipcpath))
    {
	boost::this_thread::sleep_for(boost::chrono::milliseconds(10));
    }
}

service_launcher::~service_launcher()
{
    if (!has_terminated)
    {
	wait();
    }
}

int service_launcher::wait()
{
    int exit_code = -1;
    int status = 0;
    if (waitpid(pid_, &status, 0) == pid_)
    {
	if (WIFEXITED(status))
	{
	    exit_code = WEXITSTATUS(status);
	}
	has_terminated = true;
    }
    return exit_code;
}

} // anonymous namespace

TEST(mmap_container_test, access_historical)
{
    config conf(ipc::mmap, bfs::absolute(bfs::unique_path()).string());
    service_launcher launcher(conf);
    service_client client(conf);
    mvcc_mmap_reader readerA(bfs::path(conf.name.c_str()));
    const char* key = "access_historical";

    string_value expected1("11");
    client.send_write_string(1U, key, expected1);
    ASSERT_TRUE(readerA.exists<string_value>(key)) << "write failed";
    const string_value& actual1 = readerA.read<string_value>(key);
    EXPECT_EQ(expected1, actual1) << "read value is not the value just written";

    string_value expected2("22");
    client.send_write_string(2U, key, expected2);
    ASSERT_TRUE(readerA.exists<string_value>(key)) << "write failed";
    const string_value& actual2 = readerA.read<string_value>(key);
    EXPECT_EQ(expected2, actual2) << "read value is not the value just written";
    EXPECT_EQ(expected1, actual1) << "incorrect historical value";

    client.send_terminate(3U);
}

TEST(mmap_container_test, atomic_global_revision)
{
    boost::atomic<mvcc_revision> tmp;
    ASSERT_TRUE(tmp.is_lock_free()) << "mvcc_revision is not atomic";
}

TEST(mmap_container_test, process_read_metadata_single_key)
{
    config conf(ipc::mmap, bfs::absolute(bfs::unique_path()).string());
    service_launcher launcher(conf);
    service_client client(conf);
    mvcc_mmap_reader readerA(bfs::path(conf.name.c_str()));
    mvcc_mmap_reader readerB(bfs::path(conf.name.c_str()));
    const char* key = "process_read_metadata_single";

    string_value expected1("abc1");
    client.send_write_string(10U, key, expected1);
    const string_value& readerA_actual1 = readerA.read<string_value>(key);
    const string_value& readerB_actual1 = readerB.read<string_value>(key);
    EXPECT_EQ(readerA_actual1, expected1) << "value read is not the value just written";
    EXPECT_EQ(readerB_actual1, expected1) << "value read is not the value just written";
    client.send_process_read_metadata(11U);
    boost::uint64_t readerA_rev1 = readerA.get_last_read_revision();
    boost::uint64_t readerB_rev1 = readerB.get_last_read_revision();
    boost::uint64_t oldest_rev1 = client.send_get_global_oldest_revision_read(12U);
    EXPECT_NE(oldest_rev1, 0U) << "process_read_metadata failed";
    EXPECT_EQ(readerA_rev1, oldest_rev1) << "oldest global read revision is not correct";
    EXPECT_EQ(readerB_rev1, oldest_rev1) << "oldest global read revision is not correct";

    string_value expected2("abc2");
    client.send_write_string(20U, key, expected2);
    const string_value& readerA_actual2 = readerA.read<string_value>(key);
    const string_value& readerB_actual2 = readerB.read<string_value>(key);
    EXPECT_EQ(readerA_actual2, expected2) << "value read is not the value just written";
    EXPECT_EQ(readerB_actual2, expected2) << "value read is not the value just written";
    client.send_process_read_metadata(21U);
    boost::uint64_t readerA_rev2 = readerA.get_last_read_revision();
    boost::uint64_t readerB_rev2 = readerB.get_last_read_revision();
    boost::uint64_t oldest_rev2 = client.send_get_global_oldest_revision_read(22U);
    EXPECT_NE(oldest_rev2, 0U) << "process_read_metadata failed";
    EXPECT_EQ(readerA_rev2, oldest_rev2) << "oldest global read revision is not correct";
    EXPECT_EQ(readerB_rev2, oldest_rev2) << "oldest global read revision is not correct";

    string_value expected3("abc3");
    client.send_write_string(30U, key, expected3);
    const string_value& readerA_actual3 = readerA.read<string_value>(key);
    EXPECT_EQ(readerA_actual3, expected3) << "value read is not the value just written";
    client.send_process_read_metadata(31U);
    boost::uint64_t readerA_rev3 = readerA.get_last_read_revision();
    boost::uint64_t readerB_rev3 = readerB.get_last_read_revision();
    boost::uint64_t oldest_rev3 = client.send_get_global_oldest_revision_read(32U);
    EXPECT_NE(readerA_rev3, oldest_rev3) << "oldest global read revision is not correct";
    EXPECT_EQ(readerB_rev3, oldest_rev3) << "oldest global read revision is not correct";
    EXPECT_EQ(readerB_rev2, readerB_rev3) << "last read revision is not correct";

    client.send_terminate(40U);
}

TEST(mmap_container_test, process_read_metadata_multi_key)
{
    config conf(ipc::mmap, bfs::absolute(bfs::unique_path()).string());
    service_launcher launcher(conf);
    service_client client(conf);
    mvcc_mmap_reader readerA(bfs::path(conf.name.c_str()));
    mvcc_mmap_reader readerB(bfs::path(conf.name.c_str()));
    const char* keyA = "process_read_metadata_multi_A";
    const char* keyB = "process_read_metadata_multi_B";
    const char* keyC = "process_read_metadata_multi_C";

    string_value expectedA1("abc1");
    client.send_write_string(10U, keyA, expectedA1);
    const string_value& readerA_actual1 = readerA.read<string_value>(keyA);
    const string_value& readerB_actual1 = readerB.read<string_value>(keyA);
    EXPECT_EQ(readerA_actual1, expectedA1) << "value read is not the value just written";
    EXPECT_EQ(readerB_actual1, expectedA1) << "value read is not the value just written";
    client.send_process_read_metadata(11U);
    boost::uint64_t readerA_rev1 = readerA.get_last_read_revision();
    boost::uint64_t readerB_rev1 = readerB.get_last_read_revision();
    boost::uint64_t oldest_rev1 = client.send_get_global_oldest_revision_read(12U);
    EXPECT_NE(oldest_rev1, 0U) << "process_read_metadata failed";
    EXPECT_EQ(readerA_rev1, oldest_rev1) << "oldest global read revision is not correct";
    EXPECT_EQ(readerB_rev1, oldest_rev1) << "oldest global read revision is not correct";

    string_value expectedB1("xyz1");
    client.send_write_string(20U, keyB, expectedB1);
    const string_value& readerA_actual2 = readerA.read<string_value>(keyB);
    const string_value& readerB_actual2 = readerB.read<string_value>(keyB);
    EXPECT_EQ(readerA_actual2, expectedB1) << "value read is not the value just written";
    EXPECT_EQ(readerB_actual2, expectedB1) << "value read is not the value just written";
    client.send_process_read_metadata(21U);
    boost::uint64_t readerA_rev2 = readerA.get_last_read_revision();
    boost::uint64_t readerB_rev2 = readerB.get_last_read_revision();
    boost::uint64_t oldest_rev2 = client.send_get_global_oldest_revision_read(22U);
    EXPECT_NE(oldest_rev2, 0U) << "process_read_metadata failed";
    EXPECT_TRUE(oldest_rev1 < oldest_rev2) << "process_read_metadata did not detect read change";
    EXPECT_EQ(readerA_rev2, oldest_rev2) << "oldest global read revision is not correct";
    EXPECT_EQ(readerB_rev2, oldest_rev2) << "oldest global read revision is not correct";

    string_value expectedC1("!@#1");
    client.send_write_string(30U, keyC, expectedC1);
    const string_value& readerB_actual3 = readerB.read<string_value>(keyC);
    EXPECT_EQ(readerB_actual3, expectedC1) << "value read is not the value just written";
    client.send_process_read_metadata(31U);
    boost::uint64_t readerA_rev3 = readerA.get_last_read_revision();
    boost::uint64_t readerB_rev3 = readerB.get_last_read_revision();
    boost::uint64_t oldest_rev3 = client.send_get_global_oldest_revision_read(32U);
    EXPECT_NE(oldest_rev3, 0U) << "process_read_metadata failed";
    EXPECT_EQ(oldest_rev2, oldest_rev3) << "process_read_metadata incorrectly changed the oldest found";
    EXPECT_TRUE(oldest_rev3 < readerB_rev3) << "oldest global read revision is not correct";
    EXPECT_EQ(readerA_rev3,  oldest_rev3) << "oldest global read revision is not correct";

    client.send_terminate(40U);
}

TEST(mmap_container_test, process_read_metadata_subset)
{
    config conf(ipc::mmap, bfs::absolute(bfs::unique_path()).string());
    service_launcher launcher(conf);
    service_client client(conf);
    mvcc_mmap_reader readerA(bfs::path(conf.name.c_str()));
    mvcc_mmap_reader readerB(bfs::path(conf.name.c_str()));
    mvcc_mmap_reader readerC(bfs::path(conf.name.c_str()));
    const char* keyA = "process_read_metadata_multi_A";
    const char* keyB = "process_read_metadata_multi_B";

    string_value expectedA1("abc123");
    client.send_write_string(10U, keyA, expectedA1);
    readerA.read<string_value>(keyA);
    readerB.read<string_value>(keyA);
    readerC.read<string_value>(keyA);

    string_value expectedB1("def456");
    client.send_write_string(20U, keyB, expectedB1);
    readerA.read<string_value>(keyB);
    readerB.read<string_value>(keyB);

    string_value expectedA2("xyz123");
    client.send_write_string(30U, keyA, expectedA2);
    readerA.read<string_value>(keyA);

    client.send_process_read_metadata(40U, readerA.get_reader_token_id(), readerC.get_reader_token_id());
    boost::uint64_t readerA_rev = readerA.get_last_read_revision();
    boost::uint64_t readerB_rev = readerB.get_last_read_revision();
    boost::uint64_t readerC_rev = readerC.get_last_read_revision();
    boost::uint64_t oldest_rev = client.send_get_global_oldest_revision_read(41U);
    EXPECT_TRUE(oldest_rev < readerA_rev) << "process_read_metadata did not detected global oldest";
    EXPECT_EQ(oldest_rev, readerB_rev) << "process_read_metadata did not detected global oldest";
    EXPECT_TRUE(readerC_rev < oldest_rev) << "process_read_metadata did not detected global oldest";

    client.send_terminate(50U);
}

TEST(mmap_container_test, process_write_metadata_single_key)
{
    config conf(ipc::mmap, bfs::absolute(bfs::unique_path()).string());
    service_launcher launcher(conf);
    service_client client(conf);
    std::string key("process_write_metadata_single");

    string_value value1("abc123");
    client.send_write_string(10U, key.c_str(), value1);
    client.send_process_write_metadata(11U);
    std::vector<std::string> registered1(client.send_get_registered_keys(12U));
    EXPECT_EQ(registered1.size(), 1U);
    EXPECT_EQ(registered1.at(0), key);

    string_value value2("abc456");
    client.send_write_string(20U, key.c_str(), value2);
    client.send_process_write_metadata(21U);
    std::vector<std::string> registered2(client.send_get_registered_keys(22U));
    EXPECT_EQ(registered1.size(), 1U);
    EXPECT_EQ(registered1.at(0), key);

    client.send_terminate(30U);
}

TEST(mmap_container_test, process_write_metadata_multi_key)
{
    config conf(ipc::mmap, bfs::absolute(bfs::unique_path()).string());
    service_launcher launcher(conf);
    service_client client(conf);
    std::string keyA("process_write_metadata_A");
    std::string keyB("process_write_metadata_B");
    std::string keyC("process_write_metadata_C");

    string_value valueC1("abc123");
    client.send_write_string(10U, keyC.c_str(), valueC1);
    string_value valueB1("def123");
    client.send_write_string(11U, keyB.c_str(), valueB1);
    client.send_process_write_metadata(12U);
    std::vector<std::string> registered1(client.send_get_registered_keys(13U));
    EXPECT_EQ(registered1.size(), 2U);
    EXPECT_EQ(registered1.at(0), keyB);
    EXPECT_EQ(registered1.at(1), keyC);

    string_value valueC2("abc456");
    client.send_write_string(20U, keyC.c_str(), valueC2);
    string_value valueA1("ghi123");
    client.send_write_string(21U, keyA.c_str(), valueA1);
    client.send_process_write_metadata(22U);
    std::vector<std::string> registered2(client.send_get_registered_keys(23U));
    EXPECT_EQ(registered2.size(), 3U);
    EXPECT_EQ(registered2.at(0), keyA);
    EXPECT_EQ(registered2.at(1), keyB);
    EXPECT_EQ(registered2.at(2), keyC);

    client.send_terminate(30U);
}

TEST(mmap_container_test, process_write_metadata_subset)
{
    config conf(ipc::mmap, bfs::absolute(bfs::unique_path()).string());
    service_launcher launcher(conf);
    service_client client(conf);
    std::string keyA("process_write_metadata_A");
    std::string keyB("process_write_metadata_B");
    std::string keyC("process_write_metadata_C");

    string_value valueC1("abc123");
    client.send_write_string(10U, keyC.c_str(), valueC1);
    string_value valueB1("def123");
    client.send_write_string(11U, keyB.c_str(), valueB1);
    client.send_process_write_metadata(12U, 1U);
    std::vector<std::string> registered1(client.send_get_registered_keys(13U));
    EXPECT_EQ(registered1.size(), 1U);
    EXPECT_EQ(registered1.at(0), keyC);

    string_value valueC2("abc456");
    client.send_write_string(20U, keyC.c_str(), valueC2);
    string_value valueA1("ghi123");
    client.send_write_string(21U, keyA.c_str(), valueA1);
    client.send_process_write_metadata(22U, 1U);
    std::vector<std::string> registered2(client.send_get_registered_keys(23U));
    EXPECT_EQ(registered2.size(), 2U);
    EXPECT_EQ(registered2.at(0), keyB);
    EXPECT_EQ(registered2.at(1), keyC);

    client.send_process_write_metadata(40U, 5U);
    std::vector<std::string> registered3(client.send_get_registered_keys(41U));
    EXPECT_EQ(registered3.size(), 3U);
    EXPECT_EQ(registered3.at(0), keyA);
    EXPECT_EQ(registered3.at(1), keyB);
    EXPECT_EQ(registered3.at(2), keyC);

    client.send_terminate(50U);
}
