#include <cstdio>
#include <iostream>
#include <string>
#include <memory>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/regex.hpp>
#include <boost/program_options.hpp>

using std::string;
using std::cout;
using std::to_string;
using std::size_t;

using boost::asio::ip::tcp;
using boost::asio::ip::udp;

namespace ba = boost::asio;
namespace bs = boost::system;
namespace po = boost::program_options;

ba::io_service io_service;
std::shared_ptr<tcp::socket> tcp_socket;
std::shared_ptr<udp::socket> udp_socket;
ba::posix::stream_descriptor in(io_service, ::dup(STDIN_FILENO));
ba::deadline_timer keepalive_timer(io_service);
ba::deadline_timer restart_timer(io_service);
ba::deadline_timer connection_timer(io_service);

string server = "localhost";
string port = "14670";

const int buffer_size = 60000;
const long unsigned int max_input = 10000;

int last_received = -1;
int last_sent = -1;
int biggest_seen = -1;
int retransmission_limit = 10;
long unsigned int free_space = 0;

bool connected = false;
bool acknowledged = true;
bool data_received = false;
bool server_active = false;
bool first_datagram = true;
bool is_reading = false;

string last_datagram;

char tcp_buffer[buffer_size];
char udp_buffer[buffer_size];
char data_buffer[buffer_size];

string buffer = "";

void restart();

void packet_sent(const bs::error_code &ec, size_t bytes_count, std::shared_ptr<string> pointer)
{
	if (ec && ec != ba::error::operation_aborted)
		restart();
}

void send_upload(int number, string data)
{
	std::shared_ptr<string> message = std::make_shared<string>("UPLOAD " + to_string(number) + "\n" + data);
	udp_socket->async_send(ba::buffer(*message), boost::bind(packet_sent, _1, _2, message));
}

void send_retransmit(int number)
{
	std::shared_ptr<string> message = std::make_shared<string>("RETRANSMIT " + to_string(number) + "\n");
	udp_socket->async_send(ba::buffer(*message), boost::bind(packet_sent, _1, _2, message));
}

void send_keepalive(const bs::error_code &ec)
{
	if (!ec) {
		std::shared_ptr<string> message = std::make_shared<string>("KEEPALIVE\n");
		udp_socket->async_send(ba::buffer(*message), boost::bind(packet_sent, _1, _2, message));
		
		keepalive_timer.expires_at(keepalive_timer.expires_at() + boost::posix_time::milliseconds(100));
		keepalive_timer.async_wait(send_keepalive);
	}
	else if (ec != ba::error::operation_aborted)
		restart();
}

void print_data(char* datagram, size_t bytes_count)
{
	unsigned int i = 0;
	while (datagram[i] != '\n')
		i++;
	
	if (i + 1 < bytes_count) {
		cout.write(datagram + i + 1, bytes_count - i - 1);
		fflush(stdout);
	}
}

void clear(char* text)
{
	std::fill(text, text + buffer_size, 0);
}

void check_connection(const bs::error_code &ec)
{
	if (!ec) {
		if (!server_active) {
			restart();
			return;
		}
		else
			server_active = false;
		
		connection_timer.expires_at(connection_timer.expires_at() + boost::posix_time::seconds(1));
		connection_timer.async_wait(check_connection);
	}
	else if (ec != ba::error::operation_aborted)
		restart();
}

void send_data(string data)
{
	send_upload(last_sent + 1, data);
	last_sent++;
	last_datagram = data;
	
	acknowledged = false;
}

int how_much_to_scan()
{
	if (buffer.size() > 0 || acknowledged == false)
		return 0;
	else
		return std::min(free_space, max_input);
}

void scan_input(const bs::error_code &ec, size_t bytes_count)
{
	if (!ec) {
		is_reading = false;
		
		if (buffer.size() > 0 && acknowledged == true) {
			send_data(buffer.substr(0, std::min(buffer.size(), free_space)));
			buffer = buffer.substr(std::min(buffer.size(), free_space));
		}
		else if (bytes_count > 0 && acknowledged == true) {
			if (bytes_count <= free_space)
				send_data(string(data_buffer, bytes_count));
			else {
				send_data(string(data_buffer, free_space));
				buffer += string(data_buffer + free_space, bytes_count - free_space);
			}
		}
		
		clear(data_buffer);
		
		if (acknowledged == true) {
			is_reading = true;
			async_read(in, ba::buffer(data_buffer),
					  ba::transfer_exactly(how_much_to_scan()), scan_input);
		}
	}
	else if (ec != ba::error::operation_aborted && ec != ba::error::eof)
		restart();
}

void read_udp_data(const bs::error_code &ec, size_t bytes_count)
{
	if (!ec) {
		boost::regex data_regex("DATA \\d+ \\d+ \\d+\n.*");
		boost::regex ack_regex("ACK \\d+ \\d+\n");
		boost::cmatch matches;
		
		if (boost::regex_match(udp_buffer, matches, data_regex)) {
			server_active = true;
			
			int number, ack, win;
			sscanf(udp_buffer + 5, "%d%d%d", &number, &ack, &win);
			
			if (data_received == false)
				data_received = true;
			else if (acknowledged == false)
				send_upload(last_sent, last_datagram);
				
			
			if (number == last_received + 1 || first_datagram == true) {
				print_data(udp_buffer, bytes_count);
				last_received = number;
				free_space = win;
				
				if (first_datagram == true) {
					first_datagram = false;
					
					if (!is_reading) {
						is_reading = true;
						async_read(in, ba::buffer(data_buffer),
								ba::transfer_exactly(how_much_to_scan()), scan_input);
					}
				}
			}
			else if (number > last_received + 1) {
				if (last_received + 1 >= number - retransmission_limit && number > biggest_seen) {
					send_retransmit(last_received + 1);
				}
				else {
					print_data(udp_buffer, bytes_count);
					acknowledged = true;
					last_received = number;
					free_space = win;
					
					if (!is_reading) {
						is_reading = true;
						async_read(in, ba::buffer(data_buffer),
								ba::transfer_exactly(how_much_to_scan()), scan_input);
					}
				}
			}
			
			biggest_seen = std::max(number, biggest_seen);
		}
		else if (boost::regex_match(udp_buffer, matches, ack_regex)) {
			server_active = true;
			
			int ack, win;
			sscanf(udp_buffer + 4, "%d%d", &ack, &win);
			
			
			if (ack == last_sent + 1) {
				acknowledged = true;
				data_received = false;
				free_space = win;
				
				if (!is_reading) {
					is_reading = true;
					async_read(in, ba::buffer(data_buffer),
							 ba::transfer_exactly(how_much_to_scan()), scan_input);
				}
			}
		}
		
		clear(udp_buffer);
		udp_socket->async_receive(ba::buffer(udp_buffer), read_udp_data);
	}
	else if (ec != ba::error::operation_aborted)
		restart();
}

void read_tcp_data(const bs::error_code &ec, size_t bytes_count)
{
	if (!ec) {
		boost::regex client_regex("CLIENT \\d+\n");
		boost::cmatch matches;
		
		if (boost::regex_match(tcp_buffer, matches, client_regex) && connected == false) {
			connected = true;
			
			udp::resolver resolver(io_service);
			udp::resolver::query query(server, port);
			
			try {
				udp::resolver::iterator it = resolver.resolve(query);
				udp_socket->connect(*it);
			} catch (bs::system_error e) {
				restart();
				return;
			}
			
			std::shared_ptr<string> message = std::make_shared<string>(tcp_buffer);
			udp_socket->async_send(ba::buffer(*message), boost::bind(packet_sent, _1, _2, message));
			
			server_active = true;
			
			udp_socket->async_receive(ba::buffer(udp_buffer), read_udp_data);
			keepalive_timer.expires_from_now(boost::posix_time::milliseconds(100));
			keepalive_timer.async_wait(send_keepalive);
			connection_timer.expires_from_now(boost::posix_time::seconds(1));
			connection_timer.async_wait(check_connection);
		}
		else {
			std::cerr << string(tcp_buffer, bytes_count);
			server_active = true;
		}
		
		clear(tcp_buffer);
		tcp_socket->async_receive(ba::buffer(tcp_buffer), read_tcp_data);
	}
	else if (ec != ba::error::operation_aborted)
		restart();
}

void connect(string server, string port)
{
	tcp_socket = std::shared_ptr<tcp::socket>(new tcp::socket(io_service));
	udp_socket = std::shared_ptr<udp::socket>(new udp::socket(io_service));
	
	tcp::resolver resolver(io_service);
	tcp::resolver::query query(server, port);
	
	try {
		tcp::resolver::iterator it = resolver.resolve(query);
		tcp_socket->connect(*it);
		tcp_socket->set_option(tcp::no_delay(true));
	} catch (bs::system_error e) {
		restart();
		return;
	}
	
	tcp_socket->async_receive(ba::buffer(tcp_buffer), read_tcp_data);
}

void restart()
{	
	std::cerr << "Restarting...\n";
	
	keepalive_timer.cancel();
	connection_timer.cancel();
	
	if (udp_socket->is_open()) {
		try {
			udp_socket->cancel();
		} catch (bs::system_error e) {}
	}
	if (tcp_socket->is_open()) {
		try {
			tcp_socket->cancel();
		} catch (bs::system_error e) {}
	}
	
	try {
		udp_socket->close();
	} catch (bs::system_error e) {}
	
	try {
		tcp_socket->close();
	} catch (bs::system_error e) {}
	
	udp_socket.reset();
	tcp_socket.reset();
	
	last_received = last_sent = biggest_seen = -1;
	free_space = 0;
	connected = data_received = server_active = is_reading = false;
	acknowledged = first_datagram = true;
	
	clear(tcp_buffer);
	clear(udp_buffer);
	clear(data_buffer);
	
	buffer = "";
	
	restart_timer.expires_from_now(boost::posix_time::milliseconds(500));
	restart_timer.wait();
	
	connect(server, port);
}

int main(int argc, char** argv)
{
	po::options_description desc("Options");
	
	desc.add_options()
		("server,s", po::value<string>(), "Server name")
		("port,p", po::value<string>(), "Port")
		("retransmit,X", po::value<int>(), "Retransmit limit");
	
	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	po::notify(vm);
	
	if (vm.count("server"))
		server = vm["server"].as<string>();
	if (vm.count("port"))
		port = vm["port"].as<string>();
	if (vm.count("retransmit"))
		retransmission_limit = vm["retransmit"].as<int>();
	
	
	connect(server, port);
	
	io_service.run();
}
	
