#include <nano/lib/interval.hpp>
#include <nano/node/messages.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/tcp.hpp>
#include <nano/node/transport/tcp_listener.hpp>
#include <nano/node/transport/tcp_server.hpp>

#include <boost/asio/use_future.hpp>

#include <memory>

using namespace std::chrono_literals;

/*
 * tcp_listener
 */

nano::transport::tcp_listener::tcp_listener (uint16_t port_a, nano::node & node_a, std::size_t max_inbound_connections) :
	node{ node_a },
	stats{ node_a.stats },
	logger{ node_a.logger },
	port{ port_a },
	max_inbound_connections{ max_inbound_connections },
	acceptor{ node_a.io_ctx },
	local{ asio::ip::tcp::endpoint{ asio::ip::address_v6::any (), port_a } }
{
	connection_accepted.add ([this] (auto const & socket, auto const & server) {
		node.observers.socket_accepted.notify (*socket);
	});
}

nano::transport::tcp_listener::~tcp_listener ()
{
	// Thread should be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::transport::tcp_listener::start ()
{
	debug_assert (!thread.joinable ());
	debug_assert (!cleanup_thread.joinable ());

	try
	{
		acceptor.open (local.protocol ());
		acceptor.set_option (asio::ip::tcp::acceptor::reuse_address (true));
		acceptor.bind (local);
		acceptor.listen (asio::socket_base::max_listen_connections);

		logger.info (nano::log::type::tcp_listener, "Listening for incoming connections on: {}", fmt::streamed (acceptor.local_endpoint ()));
	}
	catch (boost::system::system_error const & ex)
	{
		logger.critical (nano::log::type::tcp_listener, "Error while binding for incoming TCP: {} (port: {})", ex.what (), port);

		throw std::runtime_error (ex.code ().message ());
	}

	thread = std::thread ([this] {
		nano::thread_role::set (nano::thread_role::name::tcp_listener);
		try
		{
			logger.debug (nano::log::type::tcp_listener, "Starting acceptor thread");
			run ();
			logger.debug (nano::log::type::tcp_listener, "Stopped acceptor thread");
		}
		catch (std::exception const & ex)
		{
			logger.critical (nano::log::type::tcp_listener, "Error: {}", ex.what ());
			release_assert (false); // Should be handled earlier
		}
		catch (...)
		{
			logger.critical (nano::log::type::tcp_listener, "Unknown error");
			release_assert (false); // Should be handled earlier
		}
	});

	cleanup_thread = std::thread ([this] {
		nano::thread_role::set (nano::thread_role::name::tcp_listener);
		run_cleanup ();
	});
}

void nano::transport::tcp_listener::stop ()
{
	debug_assert (!stopped);

	logger.info (nano::log::type::tcp_listener, "Stopping listening for incoming connections and closing all sockets...");
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;

		boost::system::error_code ec;
		acceptor.close (ec); // Best effort to close the acceptor, ignore errors
		if (ec)
		{
			logger.error (nano::log::type::tcp_listener, "Error while closing acceptor: {}", ec.message ());
		}
	}
	condition.notify_all ();

	if (thread.joinable ())
	{
		thread.join ();
	}
	if (cleanup_thread.joinable ())
	{
		cleanup_thread.join ();
	}

	decltype (connections) connections_l;
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		connections_l.swap (connections);
	}

	for (auto & connection : connections_l)
	{
		if (auto socket = connection.socket.lock ())
		{
			socket->close ();
		}
		if (auto server = connection.server.lock ())
		{
			server->stop ();
		}
	}
}

void nano::transport::tcp_listener::run_cleanup ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::cleanup);
		cleanup ();
		condition.wait_for (lock, 1s, [this] () { return stopped.load (); });
	}
}

void nano::transport::tcp_listener::cleanup ()
{
	debug_assert (!mutex.try_lock ());

	erase_if (connections, [this] (auto const & connection) {
		if (connection.socket.expired () && connection.server.expired ())
		{
			stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::erase_dead);
			logger.debug (nano::log::type::tcp_listener, "Evicting dead connection: {}", fmt::streamed (connection.endpoint));
			return true;
		}
		else
		{
			return false;
		}
	});
}

void nano::transport::tcp_listener::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped && acceptor.is_open ())
	{
		lock.unlock ();

		wait_available_slots ();

		if (stopped)
		{
			return;
		}

		try
		{
			auto result = accept_one ();
			if (result != accept_result::accepted)
			{
				stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::accept_failure, nano::stat::dir::in);
				// Refusal reason should be logged earlier
			}
		}
		catch (boost::system::system_error const & ex)
		{
			stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::accept_error, nano::stat::dir::in);
			logger.log (nano::log::level::debug, nano::log::type::tcp_listener, "Error accepting incoming connection: {}", ex.what ());
		}

		lock.lock ();

		// Sleep for a while to prevent busy loop
		condition.wait_for (lock, 10ms, [this] () { return stopped.load (); });
	}
	if (!stopped)
	{
		logger.error (nano::log::type::tcp_listener, "Acceptor stopped unexpectedly");
		debug_assert (false, "acceptor stopped unexpectedly");
	}
}

asio::ip::tcp::socket nano::transport::tcp_listener::accept_socket ()
{
	std::future<asio::ip::tcp::socket> future;
	{
		nano::unique_lock<nano::mutex> lock{ mutex };
		future = acceptor.async_accept (asio::use_future);
	}
	future.wait ();
	return future.get ();
}

auto nano::transport::tcp_listener::accept_one () -> accept_result
{
	auto raw_socket = accept_socket ();
	auto const remote_endpoint = raw_socket.remote_endpoint ();
	auto const local_endpoint = raw_socket.local_endpoint ();

	if (auto result = check_limits (remote_endpoint.address ()); result != accept_result::accepted)
	{
		stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::accept_limits_exceeded, nano::stat::dir::in);
		// Refusal reason should be logged earlier

		try
		{
			// Best effor attempt to gracefully close the socket, shutdown before closing to avoid zombie sockets
			raw_socket.shutdown (asio::ip::tcp::socket::shutdown_both);
			raw_socket.close ();
		}
		catch (boost::system::system_error const & ex)
		{
			stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::close_error, nano::stat::dir::in);
			logger.debug (nano::log::type::tcp_listener, "Error while closing socket after refusing connection: {}", ex.what ());
		}

		return result;
	}

	stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::accept_success, nano::stat::dir::in);
	logger.debug (nano::log::type::tcp_listener, "Accepted incoming connection from: {}", fmt::streamed (remote_endpoint));

	auto socket = std::make_shared<nano::transport::socket> (node, std::move (raw_socket), remote_endpoint, local_endpoint, socket_endpoint::server);
	auto server = std::make_shared<nano::transport::tcp_server> (socket, node.shared (), true);

	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		connections.emplace (entry{ remote_endpoint, socket, server });
	}

	socket->set_timeout (node.network_params.network.idle_timeout);
	socket->start ();
	server->start ();

	connection_accepted.notify (socket, server);

	return accept_result::accepted;
}

void nano::transport::tcp_listener::wait_available_slots ()
{
	nano::interval log_interval;
	while (connection_count () >= max_inbound_connections && !stopped)
	{
		if (log_interval.elapsed (node.network_params.network.is_dev_network () ? 1s : 15s))
		{
			logger.warn (nano::log::type::tcp_listener, "Waiting for available slots to accept new connections (current: {} / max: {})",
			connection_count (), max_inbound_connections);
		}

		std::this_thread::sleep_for (100ms);
	}
}

auto nano::transport::tcp_listener::check_limits (asio::ip::address const & ip) -> accept_result
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	cleanup ();

	debug_assert (connections.size () <= max_inbound_connections); // Should be checked earlier (wait_available_slots)

	if (node.network.excluded_peers.check (ip)) // true => error
	{
		stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::excluded, nano::stat::dir::in);
		logger.debug (nano::log::type::tcp_listener, "Rejected connection from excluded peer: {}", ip.to_string ());

		return accept_result::excluded;
	}

	if (!node.flags.disable_max_peers_per_ip)
	{
		if (auto count = count_per_ip (ip); count >= node.network_params.network.max_peers_per_ip)
		{
			stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::max_per_ip, nano::stat::dir::in);
			logger.debug (nano::log::type::tcp_listener, "Max connections per IP reached (ip: {}, count: {}), unable to open new connection",
			ip.to_string (), count);

			return accept_result::too_many_per_ip;
		}
	}

	// If the address is IPv4 we don't check for a network limit, since its address space isn't big as IPv6/64.
	if (!node.flags.disable_max_peers_per_subnetwork && !nano::transport::is_ipv4_or_v4_mapped_address (ip))
	{
		if (auto count = count_per_subnetwork (ip); count >= node.network_params.network.max_peers_per_subnetwork)
		{
			stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::max_per_subnetwork, nano::stat::dir::in);
			logger.debug (nano::log::type::tcp_listener, "Max connections per subnetwork reached (ip: {}, count: {}), unable to open new connection",
			ip.to_string (), count);

			return accept_result::too_many_per_subnetwork;
		}
	}

	return accept_result::accepted;
}

size_t nano::transport::tcp_listener::connection_count () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return connections.size ();
}

size_t nano::transport::tcp_listener::realtime_count () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	return std::count_if (connections.begin (), connections.end (), [] (auto const & connection) {
		if (auto socket = connection.socket.lock ())
		{
			return socket->is_realtime_connection ();
		}
		return false;
	});
}

size_t nano::transport::tcp_listener::bootstrap_count () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	return std::count_if (connections.begin (), connections.end (), [] (auto const & connection) {
		if (auto socket = connection.socket.lock ())
		{
			return socket->is_bootstrap_connection ();
		}
		return false;
	});
}

size_t nano::transport::tcp_listener::count_per_ip (asio::ip::address const & ip) const
{
	debug_assert (!mutex.try_lock ());

	return std::count_if (connections.begin (), connections.end (), [&ip] (auto const & connection) {
		return nano::transport::is_same_ip (connection.address (), ip);
	});
}

size_t nano::transport::tcp_listener::count_per_subnetwork (asio::ip::address const & ip) const
{
	debug_assert (!mutex.try_lock ());

	return std::count_if (connections.begin (), connections.end (), [this, &ip] (auto const & connection) {
		return nano::transport::is_same_subnetwork (connection.address (), ip);
	});
}

asio::ip::tcp::endpoint nano::transport::tcp_listener::endpoint () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	if (!stopped)
	{
		return { asio::ip::address_v6::loopback (), acceptor.local_endpoint ().port () };
	}
	else
	{
		return { asio::ip::address_v6::loopback (), 0 };
	}
}

std::unique_ptr<nano::container_info_component> nano::transport::tcp_listener::collect_container_info (std::string const & name)
{
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "connections", connection_count (), sizeof (decltype (connections)::value_type) }));
	return composite;
}