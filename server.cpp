#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <memory>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/regex.hpp>
#include <boost/program_options.hpp>
#include "mixer.hpp"

using std::string;
using std::cout;
using std::to_string;
using std::size_t;
using std::vector;
using std::set;
using std::map;

using boost::asio::ip::tcp;
using boost::asio::ip::udp;

namespace ba = boost::asio;
namespace bs = boost::system;
namespace po = boost::program_options;

const int buffer_size = 70000;

int port = 14670;
unsigned int fifo_size = 10560;
unsigned int fifo_low = 0;
unsigned int fifo_high = 0;
unsigned int buf_len = 10;
int tx_interval = 5;

int client_id = 0;
int broadcasted_datagram = 0;

ba::io_service io_service;
tcp::endpoint tcp_endpoint;
udp::endpoint udp_endpoint;
udp::endpoint client_udp_endpoint;
tcp::acceptor acceptor(io_service);
tcp::socket* tcp_socket;
udp::socket udp_socket(io_service);
ba::deadline_timer broadcast_timer(io_service);
ba::deadline_timer report_timer(io_service, boost::posix_time::seconds(1));
ba::deadline_timer connection_timer(io_service, boost::posix_time::seconds(1));
ba::signal_set signals(io_service, SIGINT, SIGTERM);

char buffer[buffer_size];
map<int, string> copies;

class Client;
void packet_sent(const bs::error_code &, size_t, Client*, std::shared_ptr<string>);

class Client
{
	private:
		int id;
		string address;
		unsigned int port;
		string queue;
		bool queue_active;
		bool connected;
		bool active;
		bool broken;
		int expected_datagram;
		long unsigned int min_fifo_size;
		long unsigned int max_fifo_size;
		tcp::socket* tcp_socket;
		udp::endpoint udp_endpoint;
		
	public:
		Client(int id_given, tcp::socket* tcp_socket_given)
		{
			id = id_given;
			queue = "";
			queue_active = false;
			connected = false;
			active = false;
			broken = false;
			expected_datagram = 0;
			min_fifo_size = max_fifo_size = 0;
			tcp_socket = tcp_socket_given;
			
			try {
				address = tcp_socket->remote_endpoint().address().to_string();
				port = tcp_socket->remote_endpoint().port();
			} catch (bs::system_error e) {
				broken = true;
			}
		}
		
		~Client()
		{
			try {
				tcp_socket->close();
			} catch (bs::system_error e) {}
			
			delete tcp_socket;
		}
		
		int get_id()
		{
			return id;
		}
		int get_queue_size()
		{
			return queue.size();
		}
		unsigned long int get_free_space_size()
		{
			if (fifo_size < queue.size())
				return 0;
			else
				return fifo_size - queue.size();
		}
		
		bool is_queue_active()
		{
			return queue_active;
		}
		
		bool is_connected()
		{
			return connected;
		}
		
		void set_connected(bool value)
		{
			connected = value;
		}
		
		bool is_active()
		{
			return active;
		}
		
		void set_active(bool value)
		{
			active = value;
		}
		
		bool is_broken()
		{
			return broken;
		}
		
		void set_broken(bool value)
		{
			broken = value;
		}
		
		void load_data(string data)
		{
			queue += data.substr(0, std::min(data.size(), get_free_space_size()));
			max_fifo_size = std::max(max_fifo_size, queue.size());
			
			if (queue.size() >= fifo_high)
				queue_active = true;
		}
		
		void remove_data(int length)
		{
			queue = queue.substr(length);
			min_fifo_size = std::min(min_fifo_size, queue.size());
			
			if (queue.size() <= fifo_low)
				queue_active = false;
		}
		
		struct mixer_input make_input()
		{
			struct mixer_input input;
			input.data = (void*) queue.data();
			input.length = queue.size();
			input.consumed = 0;
			
			return input;
		}
		
		string make_report()
		{
			string report = address + ":" + to_string(port) + " FIFO: "
							+ to_string(queue.size()) + "/" + to_string(fifo_size)
							+ " (min. " + to_string(min_fifo_size) + ", max. "
							+ to_string(max_fifo_size) + ")\n";
			
			min_fifo_size = max_fifo_size = queue.size();
			return report;
		}
		
		void send_report(string report)
		{
			std::shared_ptr<string> message = std::make_shared<string>(report);
			tcp_socket->async_send(ba::buffer(*message),
							boost::bind(packet_sent, _1, _2, this, message));
		}
		
		void set_udp_endpoint(udp::endpoint endpoint)
		{
			udp_endpoint = endpoint;
		}
		
		udp::endpoint get_udp_endpoint()
		{
			return udp_endpoint;
		}
		
		void send_client()
		{
			std::shared_ptr<string> message =
					std::make_shared<string>("CLIENT " + to_string(id) + "\n");
			tcp_socket->async_send(ba::buffer(*message),
						boost::bind(packet_sent, _1, _2, this, message));
		}
		
		void receive_upload(int number, string data)
		{
			active = true;
			
			if (number == expected_datagram) {
				load_data(data);
				expected_datagram++;
			}
			
			std::shared_ptr<string> message =
				std::make_shared<string>("ACK " + to_string(expected_datagram)
										  + " " + to_string(get_free_space_size()) + "\n");
										  
			udp_socket.async_send_to(ba::buffer(*message), udp_endpoint,
								boost::bind(packet_sent, _1, _2, this, message));
		}
		
		void receive_retransmit(int number)
		{
			active = true;
			map<int, string>::iterator it = copies.find(number);
			
			while (it != copies.end()) {
				send_data(it->first, it->second);
				++it;
			}
		}
		
		void receive_keepalive()
		{
			active = true;
		}
		
		void send_data(int number, string data)
		{	
			std::shared_ptr<string> message = std::make_shared<string>("DATA "
							+ to_string(number) + " " + to_string(expected_datagram)
							+ " " + to_string(get_free_space_size()) + "\n" + data);
							
			udp_socket.async_send_to(ba::buffer(*message), udp_endpoint,
							boost::bind(packet_sent, _1, _2, this, message));
		}
};

set<Client*> clients;


Client* get_client(int id)
{
	set<Client*>::iterator it;
	
	for (it = clients.begin(); it != clients.end(); ++it)
		if ((*it)->get_id() == id)
			return *it;
	
	return NULL;
}

Client* get_client(udp::endpoint endpoint)
{
	set<Client*>::iterator it;
	
	for (it = clients.begin(); it != clients.end(); ++it)
		if ((*it)->get_udp_endpoint() == endpoint)
			return *it;
	
	return NULL;
}

void clear(char* text)
{
	std::fill(text, text + buffer_size, 0);
}

void remove_client(Client* client)
{
	clients.erase(clients.find(client));
	
	if (client != NULL)
		delete client;
}

void packet_sent(const bs::error_code &ec, size_t bytes_count, Client* client, std::shared_ptr<string> pointer)
{
	if (client != NULL && ec)
		client->set_broken(true);
}

void signal_received(const bs::error_code &ec, int number)
{
	if (!ec) {
		if (tcp_socket != NULL)
			delete tcp_socket;
		
		set<Client*>::iterator it;
		vector<Client*> clients_to_remove;
		
		for (it = clients.begin(); it != clients.end(); ++it)
			clients_to_remove.push_back(*it);
		
		while (!clients_to_remove.empty()) {
			remove_client(clients_to_remove.back());
			clients_to_remove.pop_back();
		}
		
		io_service.stop();
	}
}

void generate_report(const bs::error_code &ec)
{
	if (!ec) {
		string report = "\n";
		
		set<Client*>::iterator it;
		
		for (it = clients.begin(); it != clients.end(); ++it)
			report += (*it)->make_report();
		
		for (it = clients.begin(); it != clients.end(); ++it)
			(*it)->send_report(report);
		
		//std::cerr << report.c_str();
		
		report_timer.expires_at(report_timer.expires_at() + boost::posix_time::seconds(1));
		report_timer.async_wait(generate_report);
	}
}

void check_connections(const bs::error_code &ec)
{
	if (!ec) {
		set<Client*>::iterator it;
		vector<Client*> clients_to_remove;
			
		for (it = clients.begin(); it != clients.end(); ++it)
			if ((*it)->is_connected() == true || (*it)->is_broken() == true) {
				if ((*it)->is_active() == false || (*it)->is_broken() == true)
					clients_to_remove.push_back(*it);
				else
					(*it)->set_active(false);
			}
		
		while (!clients_to_remove.empty()) {
			remove_client(clients_to_remove.back());
			clients_to_remove.pop_back();
		}
		
		connection_timer.expires_at(connection_timer.expires_at() + boost::posix_time::seconds(1));
		connection_timer.async_wait(check_connections);
	}
}

void receive_connection(const bs::error_code &ec)
{
	if (!ec) {
		try {
			tcp_socket->set_option(tcp::no_delay(true));
		} catch (bs::system_error e) {
			delete tcp_socket;
			tcp_socket = new tcp::socket(io_service);
			acceptor.async_accept(*tcp_socket, receive_connection);
			return;
		}
		
		Client* new_client = new Client(client_id, tcp_socket);
		client_id++;
		
		clients.insert(new_client);
		new_client->send_client();
		
		tcp_socket = new tcp::socket(io_service);
		acceptor.async_accept(*tcp_socket, receive_connection);
	}
}

void read_udp_data(const bs::error_code &ec, size_t bytes_count)
{
	if (!ec) {
		boost::regex client_regex("CLIENT \\d+\n");
		boost::regex upload_regex("UPLOAD \\d+\n.*");
		boost::regex retransmit_regex("RETRANSMIT \\d+\n");
		boost::regex keepalive_regex("KEEPALIVE\n");
		boost::cmatch matches;
		
		
		if (boost::regex_match(buffer, matches, client_regex)) {
			int id;
			sscanf(buffer + 7, "%d", &id);
			
			Client* client = get_client(id);
			
			if (client != NULL && client->is_connected() == false) {
				client->set_udp_endpoint(client_udp_endpoint);
				client->set_connected(true);
				client->set_active(true);
			}
		}
		
		else if (boost::regex_match(buffer, matches, upload_regex)) {
			int number;
			sscanf(buffer + 7, "%d", &number);
			
			int i = 0;
			while (buffer[i] != '\n')
				i++;
			
			string data = string(buffer + i + 1, bytes_count - i - 1);
			Client* client = get_client(client_udp_endpoint);
			
			if (client != NULL)
				client->receive_upload(number, data);
		}
		
		else if (boost::regex_match(buffer, matches, retransmit_regex)) {
			int number;
			sscanf(buffer + 11, "%d", &number);
			
			Client* client = get_client(client_udp_endpoint);
			
			if (client != NULL)
				client->receive_retransmit(number);
		}
		
		else if (boost::regex_match(buffer, matches, keepalive_regex)) {
			Client* client = get_client(client_udp_endpoint);
			
			if (client != NULL)
				client->receive_keepalive();
		}
		
		clear(buffer);
		udp_socket.async_receive_from(ba::buffer(buffer), client_udp_endpoint, read_udp_data);
	}
}

void broadcast_data(const bs::error_code &ec)
{
	if (!ec) {
		vector<Client*> active_clients;
		
		set<Client*>::iterator it;
		
		for (it = clients.begin(); it != clients.end(); ++it)
			if ((*it)->is_connected() == true && (*it)->is_queue_active() == true)
				active_clients.push_back(*it);
		
		struct mixer_input inputs[active_clients.size()];
		
		for (unsigned int i = 0; i < active_clients.size(); i++)
			inputs[i] = active_clients[i]->make_input();
		
		char output_data[200 * tx_interval];
		std::fill(output_data, output_data + 200 * tx_interval, 0);
		
		size_t output_size = sizeof(output_data);
		mixer(inputs, active_clients.size(), (void*) output_data, &output_size, tx_interval);
		
		
		for (unsigned int i = 0; i < active_clients.size(); i++)
			active_clients[i]->remove_data(inputs[i].consumed);
		
		
		for (it = clients.begin(); it != clients.end(); ++it)
			if ((*it)->is_connected() == true)
				(*it)->send_data(broadcasted_datagram, string(output_data, output_size));
		
		
		copies[broadcasted_datagram] = string(output_data, output_size);
		if (copies.size() > buf_len)
			copies.erase(copies.begin());
		
		broadcasted_datagram++;
		
		broadcast_timer.expires_at(broadcast_timer.expires_at() + boost::posix_time::milliseconds(tx_interval));
		broadcast_timer.async_wait(broadcast_data);
	}
}

int main(int argc, char** argv)
{
	po::options_description desc("Options");
	
	desc.add_options()
		("port,p", po::value<int>(), "Port")
		("fifo_size,F", po::value<int>(), "FIFO size")
		("fifo_low_watermark,L", po::value<unsigned int>(), "FIFO low watermark")
		("fifo_high_watermark,H", po::value<unsigned int>(), "FIFO high watermark")
		("buf_len,X", po::value<unsigned int>(), "Buffer length")
		("tx_interval,i", po::value<int>(), "Mixer call interval");
	
	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	po::notify(vm);
	
	if (vm.count("port"))
		port = vm["port"].as<int>();
	if (vm.count("fifo_size"))
		fifo_size = vm["fifo_size"].as<int>();
		
	fifo_high = fifo_size;
		
	if (vm.count("fifo_low_watermark"))
		fifo_low = vm["fifo_low_watermark"].as<unsigned int>();
	if (vm.count("fifo_high_watermark"))
		fifo_high = vm["fifo_high_watermark"].as<unsigned int>();
	if (vm.count("buf_len"))
		buf_len = vm["buf_len"].as<unsigned int>();
	if (vm.count("tx_interval"))
		tx_interval = vm["tx_interval"].as<int>();
	
	tcp_endpoint = tcp::endpoint(tcp::v6(), port);
	udp_endpoint = udp::endpoint(udp::v6(), port);
	acceptor = tcp::acceptor(io_service, tcp_endpoint);
	udp_socket = udp::socket(io_service, udp_endpoint);
	
	tcp_socket = new tcp::socket(io_service);
	
	acceptor.listen();
	acceptor.async_accept(*tcp_socket, receive_connection);
	udp_socket.async_receive_from(ba::buffer(buffer), client_udp_endpoint, read_udp_data);
	connection_timer.async_wait(check_connections);
	report_timer.async_wait(generate_report);
	
	signals.async_wait(signal_received);
	
	broadcast_timer.expires_from_now(boost::posix_time::milliseconds(tx_interval));
	broadcast_timer.async_wait(broadcast_data);
	
	io_service.run();
}
