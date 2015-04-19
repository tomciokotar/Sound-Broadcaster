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
std::shared_ptr<tcp::socket> tcp_sock;
std::shared_ptr<udp::socket> sock;
ba::posix::stream_descriptor in(io_service, ::dup(STDIN_FILENO));
ba::deadline_timer keepalive_timer(io_service);
ba::deadline_timer restart_timer(io_service);
ba::deadline_timer polaczenie_timer(io_service);

string serwer = "localhost";
string port = "14670";

const int rozm_bufora = 60000;
const long unsigned int max_wejscie = 10000;

int ostatni_otrzymany = -1;
int ostatni_wyslany = -1;
int maksymalny_widziany = -1;
int limit_retransmisji = 10;
long unsigned int wolne_miejsce = 0;

bool polaczony = false;
bool potwierdzony = true;
bool otrzymano_data = false;
bool serwer_aktywny = false;
bool pierwszy_datagram = true;
bool czy_czyta = false;

string ostatni_datagram;

char bufor_tcp[rozm_bufora];
char bufor_udp[rozm_bufora];
char bufor_dane[rozm_bufora];

string bufor = "";

void restart();

void wyslano_pakiet(const bs::error_code &ec, size_t ilosc_bajtow, std::shared_ptr<string> wskaznik)
{
	if (ec && ec != ba::error::operation_aborted)
		restart();
}

void wyslij_uploada(int nr, string dane)
{
	std::shared_ptr<string> wiadomosc = std::make_shared<string>("UPLOAD " + to_string(nr) + "\n" + dane);
	sock->async_send(ba::buffer(*wiadomosc), boost::bind(wyslano_pakiet, _1, _2, wiadomosc));
}

void wyslij_retransmita(int nr)
{
	std::shared_ptr<string> wiadomosc = std::make_shared<string>("RETRANSMIT " + to_string(nr) + "\n");
	sock->async_send(ba::buffer(*wiadomosc), boost::bind(wyslano_pakiet, _1, _2, wiadomosc));
}

void wyslij_keepalivea(const bs::error_code &ec)
{
	if (!ec) {
		std::shared_ptr<string> wiadomosc = std::make_shared<string>("KEEPALIVE\n");
		sock->async_send(ba::buffer(*wiadomosc), boost::bind(wyslano_pakiet, _1, _2, wiadomosc));
		
		keepalive_timer.expires_at(keepalive_timer.expires_at() + boost::posix_time::milliseconds(100));
		keepalive_timer.async_wait(wyslij_keepalivea);
	}
	else if (ec != ba::error::operation_aborted)
		restart();
}

void wypisz_dane(char* datagram, size_t ilosc_bajtow)
{
	unsigned int i = 0;
	while (datagram[i] != '\n')
		i++;
	
	if (i + 1 < ilosc_bajtow) {
		cout.write(datagram + i + 1, ilosc_bajtow - i - 1);
		fflush(stdout);
	}
}

void wyczysc(char* tekst)
{
	std::fill(tekst, tekst + rozm_bufora, 0);
}

void sprawdz_polaczenie(const bs::error_code &ec)
{
	if (!ec) {
		if (!serwer_aktywny) {
			restart();
			return;
		}
		else
			serwer_aktywny = false;
		
		polaczenie_timer.expires_at(polaczenie_timer.expires_at() + boost::posix_time::seconds(1));
		polaczenie_timer.async_wait(sprawdz_polaczenie);
	}
	else if (ec != ba::error::operation_aborted)
		restart();
}

void wyslij_dane(string dane)
{
	wyslij_uploada(ostatni_wyslany + 1, dane);
	ostatni_wyslany++;
	ostatni_datagram = dane;
	
	potwierdzony = false;
}

int ile_wczytac()
{
	if (bufor.size() > 0 || potwierdzony == false)
		return 0;
	else
		return std::min(wolne_miejsce, max_wejscie);
}

void wczytaj_wejscie(const bs::error_code &ec, size_t ilosc_bajtow)
{
	if (!ec) {
		czy_czyta = false;
		
		if (bufor.size() > 0 && potwierdzony == true) {
			wyslij_dane(bufor.substr(0, std::min(bufor.size(), wolne_miejsce)));
			bufor = bufor.substr(std::min(bufor.size(), wolne_miejsce));
		}
		else if (ilosc_bajtow > 0 && potwierdzony == true) {
			if (ilosc_bajtow <= wolne_miejsce)
				wyslij_dane(string(bufor_dane, ilosc_bajtow));
			else {
				wyslij_dane(string(bufor_dane, wolne_miejsce));
				bufor += string(bufor_dane + wolne_miejsce, ilosc_bajtow - wolne_miejsce);
			}
		}
		
		wyczysc(bufor_dane);
		
		if (potwierdzony == true) {
			czy_czyta = true;
			async_read(in, ba::buffer(bufor_dane),
					  ba::transfer_exactly(ile_wczytac()), wczytaj_wejscie);
		}
	}
	else if (ec != ba::error::operation_aborted && ec != ba::error::eof)
		restart();
}

void odczytaj_dane_udp(const bs::error_code &ec, size_t ilosc_bajtow)
{
	if (!ec) {
		boost::regex wzorzec_data("DATA \\d+ \\d+ \\d+\n.*");
		boost::regex wzorzec_ack("ACK \\d+ \\d+\n");
		boost::cmatch pasuje;
		
		if (boost::regex_match(bufor_udp, pasuje, wzorzec_data)) {
			serwer_aktywny = true;
			
			int nr, ack, win;
			sscanf(bufor_udp + 5, "%d%d%d", &nr, &ack, &win);
			
			if (otrzymano_data == false)
				otrzymano_data = true;
			else if (potwierdzony == false)
				wyslij_uploada(ostatni_wyslany, ostatni_datagram);
				
			
			if (nr == ostatni_otrzymany + 1 || pierwszy_datagram == true) {
				wypisz_dane(bufor_udp, ilosc_bajtow);
				ostatni_otrzymany = nr;
				wolne_miejsce = win;
				
				if (pierwszy_datagram == true) {
					pierwszy_datagram = false;
					
					if (!czy_czyta) {
						czy_czyta = true;
						async_read(in, ba::buffer(bufor_dane),
								ba::transfer_exactly(ile_wczytac()), wczytaj_wejscie);
					}
				}
			}
			else if (nr > ostatni_otrzymany + 1) {
				if (ostatni_otrzymany + 1 >= nr - limit_retransmisji && nr > maksymalny_widziany) {
					wyslij_retransmita(ostatni_otrzymany + 1);
				}
				else {
					wypisz_dane(bufor_udp, ilosc_bajtow);
					potwierdzony = true;
					ostatni_otrzymany = nr;
					wolne_miejsce = win;
					
					if (!czy_czyta) {
						czy_czyta = true;
						async_read(in, ba::buffer(bufor_dane),
								ba::transfer_exactly(ile_wczytac()), wczytaj_wejscie);
					}
				}
			}
			
			maksymalny_widziany = std::max(nr, maksymalny_widziany);
		}
		else if (boost::regex_match(bufor_udp, pasuje, wzorzec_ack)) {
			serwer_aktywny = true;
			
			int ack, win;
			sscanf(bufor_udp + 4, "%d%d", &ack, &win);
			
			
			if (ack == ostatni_wyslany + 1) {
				potwierdzony = true;
				otrzymano_data = false;
				wolne_miejsce = win;
				
				if (!czy_czyta) {
					czy_czyta = true;
					async_read(in, ba::buffer(bufor_dane),
							 ba::transfer_exactly(ile_wczytac()), wczytaj_wejscie);
				}
			}
		}
		
		wyczysc(bufor_udp);
		sock->async_receive(ba::buffer(bufor_udp), odczytaj_dane_udp);
	}
	else if (ec != ba::error::operation_aborted)
		restart();
}

void odczytaj_dane_tcp(const bs::error_code &ec, size_t ilosc_bajtow)
{
	if (!ec) {
		boost::regex wzorzec("CLIENT \\d+\n");
		boost::cmatch pasuje;
		
		if (boost::regex_match(bufor_tcp, pasuje, wzorzec) && polaczony == false) {
			polaczony = true;
			
			udp::resolver resolver(io_service);
			udp::resolver::query query(serwer, port);
			
			try {
				udp::resolver::iterator it = resolver.resolve(query);
				sock->connect(*it);
			} catch (bs::system_error e) {
				restart();
				return;
			}
			
			std::shared_ptr<string> wiadomosc = std::make_shared<string>(bufor_tcp);
			sock->async_send(ba::buffer(*wiadomosc), boost::bind(wyslano_pakiet, _1, _2, wiadomosc));
			
			serwer_aktywny = true;
			
			sock->async_receive(ba::buffer(bufor_udp), odczytaj_dane_udp);
			keepalive_timer.expires_from_now(boost::posix_time::milliseconds(100));
			keepalive_timer.async_wait(wyslij_keepalivea);
			polaczenie_timer.expires_from_now(boost::posix_time::seconds(1));
			polaczenie_timer.async_wait(sprawdz_polaczenie);
		}
		else {
			std::cerr << string(bufor_tcp, ilosc_bajtow);
			serwer_aktywny = true;
		}
		
		wyczysc(bufor_tcp);
		tcp_sock->async_receive(ba::buffer(bufor_tcp), odczytaj_dane_tcp);
	}
	else if (ec != ba::error::operation_aborted)
		restart();
}

void polacz(string serwer, string port)
{
	tcp_sock = std::shared_ptr<tcp::socket>(new tcp::socket(io_service));
	sock = std::shared_ptr<udp::socket>(new udp::socket(io_service));
	
	tcp::resolver resolver(io_service);
	tcp::resolver::query query(serwer, port);
	
	try {
		tcp::resolver::iterator it = resolver.resolve(query);
		tcp_sock->connect(*it);
		tcp_sock->set_option(tcp::no_delay(true));
	} catch (bs::system_error e) {
		restart();
		return;
	}
	
	tcp_sock->async_receive(ba::buffer(bufor_tcp), odczytaj_dane_tcp);
}

void restart()
{	
	std::cerr << "Restartujemy...\n";
	
	keepalive_timer.cancel();
	polaczenie_timer.cancel();
	
	if (sock->is_open()) {
		try {
			sock->cancel();
		} catch (bs::system_error e) {}
	}
	if (tcp_sock->is_open()) {
		try {
			tcp_sock->cancel();
		} catch (bs::system_error e) {}
	}
	
	try {
		sock->close();
	} catch (bs::system_error e) {}
	
	try {
		tcp_sock->close();
	} catch (bs::system_error e) {}
	
	sock.reset();
	tcp_sock.reset();
	
	ostatni_otrzymany = ostatni_wyslany = maksymalny_widziany = -1;
	wolne_miejsce = 0;
	polaczony = otrzymano_data = serwer_aktywny = czy_czyta = false;
	potwierdzony = pierwszy_datagram = true;
	
	wyczysc(bufor_tcp);
	wyczysc(bufor_udp);
	wyczysc(bufor_dane);
	
	bufor = "";
	
	restart_timer.expires_from_now(boost::posix_time::milliseconds(500));
	restart_timer.wait();
	
	polacz(serwer, port);
}

int main(int argc, char** argv)
{
	po::options_description desc("Opcje");
	
	desc.add_options()
		("server,s", po::value<string>(), "Server name")
		("port,p", po::value<string>(), "Port")
		("retransmit,X", po::value<int>(), "Retransmit limit");
	
	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	po::notify(vm);
	
	if (vm.count("server"))
		serwer = vm["server"].as<string>();
	if (vm.count("port"))
		port = vm["port"].as<string>();
	if (vm.count("retransmit"))
		limit_retransmisji = vm["retransmit"].as<int>();
	
	
	polacz(serwer, port);
	
	io_service.run();
}
	
