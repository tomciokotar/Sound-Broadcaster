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

const int rozm_bufora = 70000;

int port = 14670;
unsigned int fifo_size = 10560;
unsigned int fifo_low = 0;
unsigned int fifo_high = 0;
unsigned int buf_len = 10;
int tx_interval = 5;

int id_klienta = 0;
int rozsylany_datagram = 0;

ba::io_service io_service;
tcp::endpoint tcp_endpoint;
udp::endpoint udp_endpoint;
udp::endpoint endpoint_klienta;
tcp::acceptor acceptor(io_service);
tcp::socket* tcp_sock;
udp::socket udp_sock(io_service);
ba::deadline_timer timer(io_service);
ba::deadline_timer raport_timer(io_service, boost::posix_time::seconds(1));
ba::deadline_timer polaczenie_timer(io_service, boost::posix_time::seconds(1));
ba::signal_set sygnaly(io_service, SIGINT, SIGTERM);

char bufor[rozm_bufora];
map<int, string> kopie;

class Klient;
void wyslano_pakiet(const bs::error_code &, size_t, Klient*, std::shared_ptr<string>);

class Klient
{
	private:
		int id;
		string adres;
		unsigned int port;
		string kolejka;
		bool kolejka_aktywna;
		bool polaczony;
		bool aktywny;
		bool zepsuty;
		int oczekiwany_datagram;
		long unsigned int min_rozm_fifo;
		long unsigned int max_rozm_fifo;
		tcp::socket* sock;
		udp::endpoint endpoint;
		
	public:
		Klient(int przyznane_id, tcp::socket* tcp_sock)
		{
			id = przyznane_id;
			kolejka = "";
			kolejka_aktywna = false;
			polaczony = false;
			aktywny = false;
			zepsuty = false;
			oczekiwany_datagram = 0;
			min_rozm_fifo = max_rozm_fifo = 0;
			sock = tcp_sock;
			
			try {
				adres = sock->remote_endpoint().address().to_string();
				port = sock->remote_endpoint().port();
			} catch (bs::system_error e) {
				zepsuty = true;
			}
		}
		
		~Klient()
		{
			try {
				sock->close();
			} catch (bs::system_error e) {}
			
			delete sock;
		}
		
		int daj_id()
		{
			return id;
		}
		int dlugosc()
		{
			return kolejka.size();
		}
		unsigned long int wolne_miejsce()
		{
			if (fifo_size < kolejka.size())
				return 0;
			else
				return fifo_size - kolejka.size();
		}
		
		bool czy_kolejka_aktywna()
		{
			return kolejka_aktywna;
		}
		
		bool czy_polaczony()
		{
			return polaczony;
		}
		
		void ustaw_polaczony(bool wartosc)
		{
			polaczony = wartosc;
		}
		
		bool czy_aktywny()
		{
			return aktywny;
		}
		
		void ustaw_aktywny(bool wartosc)
		{
			aktywny = wartosc;
		}
		
		bool czy_zepsuty()
		{
			return zepsuty;
		}
		
		void ustaw_zepsuty(bool wartosc)
		{
			zepsuty = wartosc;
		}
		
		void wrzuc_dane(string dane)
		{
			kolejka += dane.substr(0, std::min(dane.size(), wolne_miejsce()));
			max_rozm_fifo = std::max(max_rozm_fifo, kolejka.size());
			
			if (kolejka.size() >= fifo_high)
				kolejka_aktywna = true;
		}
		
		void wywal_dane(int ile)
		{
			kolejka = kolejka.substr(ile);
			min_rozm_fifo = std::min(min_rozm_fifo, kolejka.size());
			
			if (kolejka.size() <= fifo_low)
				kolejka_aktywna = false;
		}
		
		struct mixer_input zrob_inputa()
		{
			struct mixer_input input;
			input.data = (void*) kolejka.data();
			input.len = kolejka.size();
			input.consumed = 0;
			
			return input;
		}
		
		string zrob_raport()
		{
			string raport = adres + ":" + to_string(port) + " FIFO: "
							+ to_string(kolejka.size()) + "/" + to_string(fifo_size)
							+ " (min. " + to_string(min_rozm_fifo) + ", max. "
							+ to_string(max_rozm_fifo) + ")\n";
			
			min_rozm_fifo = max_rozm_fifo = kolejka.size();
			return raport;
		}
		
		void wyslij_raport(string raport)
		{
			std::shared_ptr<string> wiadomosc = std::make_shared<string>(raport);
			sock->async_send(ba::buffer(*wiadomosc),
							boost::bind(wyslano_pakiet, _1, _2, this, wiadomosc));
		}
		
		void ustaw_endpointa(udp::endpoint endp)
		{
			endpoint = endp;
		}
		
		udp::endpoint daj_endpointa()
		{
			return endpoint;
		}
		
		void wyslij_clienta()
		{
			std::shared_ptr<string> wiadomosc =
					std::make_shared<string>("CLIENT " + to_string(id) + "\n");
			sock->async_send(ba::buffer(*wiadomosc),
						boost::bind(wyslano_pakiet, _1, _2, this, wiadomosc));
		}
		
		void przyjmij_uploada(int nr, string dane)
		{
			aktywny = true;
			
			if (nr == oczekiwany_datagram) {
				wrzuc_dane(dane);
				oczekiwany_datagram++;
			}
			
			std::shared_ptr<string> wiadomosc =
				std::make_shared<string>("ACK " + to_string(oczekiwany_datagram)
										  + " " + to_string(wolne_miejsce()) + "\n");
										  
			udp_sock.async_send_to(ba::buffer(*wiadomosc), endpoint,
								boost::bind(wyslano_pakiet, _1, _2, this, wiadomosc));
		}
		
		void przyjmij_retransmita(int nr)
		{
			aktywny = true;
			map<int, string>::iterator it = kopie.find(nr);
			
			while (it != kopie.end()) {
				wyslij_dane(it->first, it->second);
				++it;
			}
		}
		
		void przyjmij_keepalivea()
		{
			aktywny = true;
		}
		
		void wyslij_dane(int nr, string dane)
		{	
			std::shared_ptr<string> wiadomosc = std::make_shared<string>("DATA "
							+ to_string(nr) + " " + to_string(oczekiwany_datagram)
							+ " " + to_string(wolne_miejsce()) + "\n" + dane);
							
			udp_sock.async_send_to(ba::buffer(*wiadomosc), endpoint,
							boost::bind(wyslano_pakiet, _1, _2, this, wiadomosc));
		}
};

set<Klient*> klienci;


Klient* daj_klienta(int id)
{
	set<Klient*>::iterator it;
	
	for (it = klienci.begin(); it != klienci.end(); ++it)
		if ((*it)->daj_id() == id)
			return *it;
	
	return NULL;
}

Klient* daj_klienta(udp::endpoint endpoint)
{
	set<Klient*>::iterator it;
	
	for (it = klienci.begin(); it != klienci.end(); ++it)
		if ((*it)->daj_endpointa() == endpoint)
			return *it;
	
	return NULL;
}

void wyczysc(char* tekst)
{
	std::fill(tekst, tekst + rozm_bufora, 0);
}

void wywal(Klient* usuwany)
{
	klienci.erase(klienci.find(usuwany));
	
	if (usuwany != NULL)
		delete usuwany;
}

void wyslano_pakiet(const bs::error_code &ec, size_t ilosc_bajtow, Klient* klient, std::shared_ptr<string> wskaznik)
{
	if (klient != NULL && ec)
		klient->ustaw_zepsuty(true);
}

void przechwycono_sygnal(const bs::error_code &ec, int nr)
{
	if (!ec) {
		if (tcp_sock != NULL)
			delete tcp_sock;
		
		set<Klient*>::iterator it;
		vector<Klient*> do_wywalenia;
		
		for (it = klienci.begin(); it != klienci.end(); ++it)
			do_wywalenia.push_back(*it);
		
		while (!do_wywalenia.empty()) {
			wywal(do_wywalenia.back());
			do_wywalenia.pop_back();
		}
		
		io_service.stop();
	}
}

void wygeneruj_raport(const bs::error_code &ec)
{
	if (!ec) {
		string raport = "\n";
		
		set<Klient*>::iterator it;
			
		for (it = klienci.begin(); it != klienci.end(); ++it)
			raport += (*it)->zrob_raport();
		
		for (it = klienci.begin(); it != klienci.end(); ++it)
			(*it)->wyslij_raport(raport);
		
		//std::cerr << raport.c_str();
		
		raport_timer.expires_at(raport_timer.expires_at() + boost::posix_time::seconds(1));
		raport_timer.async_wait(wygeneruj_raport);
	}
}

void sprawdz_polaczenia(const bs::error_code &ec)
{
	if (!ec) {
		set<Klient*>::iterator it;
		vector<Klient*> do_wywalenia;
			
		for (it = klienci.begin(); it != klienci.end(); ++it)
			if ((*it)->czy_polaczony() == true || (*it)->czy_zepsuty() == true) {
				if ((*it)->czy_aktywny() == false || (*it)->czy_zepsuty() == true)
					do_wywalenia.push_back(*it);
				else
					(*it)->ustaw_aktywny(false);
			}
		
		while (!do_wywalenia.empty()) {
			wywal(do_wywalenia.back());
			do_wywalenia.pop_back();
		}
		
		polaczenie_timer.expires_at(polaczenie_timer.expires_at() + boost::posix_time::seconds(1));
		polaczenie_timer.async_wait(sprawdz_polaczenia);
	}
}

void przyjmij_polaczenie(const bs::error_code &ec)
{
	if (!ec) {
		try {
			tcp_sock->set_option(tcp::no_delay(true));
		} catch (bs::system_error e) {
			delete tcp_sock;
			tcp_sock = new tcp::socket(io_service);
			acceptor.async_accept(*tcp_sock, przyjmij_polaczenie);
			return;
		}
		
		Klient* nowy = new Klient(id_klienta, tcp_sock);
		id_klienta++;
		
		klienci.insert(nowy);
		nowy->wyslij_clienta();
		
		tcp_sock = new tcp::socket(io_service);
		acceptor.async_accept(*tcp_sock, przyjmij_polaczenie);
	}
}

void odczytaj_dane_udp(const bs::error_code &ec, size_t ilosc_bajtow)
{
	if (!ec) {
		boost::regex wzorzec_client("CLIENT \\d+\n");
		boost::regex wzorzec_upload("UPLOAD \\d+\n.*");
		boost::regex wzorzec_retransmit("RETRANSMIT \\d+\n");
		boost::regex wzorzec_keepalive("KEEPALIVE\n");
		boost::cmatch pasuje;
		
		
		if (boost::regex_match(bufor, pasuje, wzorzec_client)) {
			int id;
			sscanf(bufor + 7, "%d", &id);
			
			Klient* klient = daj_klienta(id);
			
			if (klient != NULL && klient->czy_polaczony() == false) {
				klient->ustaw_endpointa(endpoint_klienta);
				klient->ustaw_polaczony(true);
				klient->ustaw_aktywny(true);
			}
		}
		
		else if (boost::regex_match(bufor, pasuje, wzorzec_upload)) {
			int nr;
			sscanf(bufor + 7, "%d", &nr);
			
			int i = 0;
			while (bufor[i] != '\n')
				i++;
			
			string dane = string(bufor + i + 1, ilosc_bajtow - i - 1);
			Klient* klient = daj_klienta(endpoint_klienta);
			
			if (klient != NULL)
				klient->przyjmij_uploada(nr, dane);
		}
		
		else if (boost::regex_match(bufor, pasuje, wzorzec_retransmit)) {
			int nr;
			sscanf(bufor + 11, "%d", &nr);
			
			Klient* klient = daj_klienta(endpoint_klienta);
			
			if (klient != NULL)
				klient->przyjmij_retransmita(nr);
		}
		
		else if (boost::regex_match(bufor, pasuje, wzorzec_keepalive)) {
			Klient* klient = daj_klienta(endpoint_klienta);
			
			if (klient != NULL)
				klient->przyjmij_keepalivea();
		}
		
		wyczysc(bufor);
		udp_sock.async_receive_from(ba::buffer(bufor), endpoint_klienta, odczytaj_dane_udp);
	}
}

void rozeslij_dane(const bs::error_code &ec)
{
	if (!ec) {
		vector<Klient*> aktywni_klienci;
		
		set<Klient*>::iterator it;
		
		for (it = klienci.begin(); it != klienci.end(); ++it)
			if ((*it)->czy_polaczony() == true && (*it)->czy_kolejka_aktywna() == true)
				aktywni_klienci.push_back(*it);
		
		struct mixer_input inputs[aktywni_klienci.size()];
		
		for (unsigned int i = 0; i < aktywni_klienci.size(); i++)
			inputs[i] = aktywni_klienci[i]->zrob_inputa();
		
		char dane_wyjsciowe[200 * tx_interval];
		std::fill(dane_wyjsciowe, dane_wyjsciowe + 200 * tx_interval, 0);
		
		size_t wyjsciowy_rozmiar = sizeof(dane_wyjsciowe);
		mixer(inputs, aktywni_klienci.size(), (void*) dane_wyjsciowe, &wyjsciowy_rozmiar, tx_interval);
			
		
		for (unsigned int i = 0; i < aktywni_klienci.size(); i++)
			aktywni_klienci[i]->wywal_dane(inputs[i].consumed);
		
		
		for (it = klienci.begin(); it != klienci.end(); ++it)
			if ((*it)->czy_polaczony() == true)
				(*it)->wyslij_dane(rozsylany_datagram, string(dane_wyjsciowe, wyjsciowy_rozmiar));
		
		
		kopie[rozsylany_datagram] = string(dane_wyjsciowe, wyjsciowy_rozmiar);
		if (kopie.size() > buf_len)
			kopie.erase(kopie.begin());
		
		rozsylany_datagram++;
		
		timer.expires_at(timer.expires_at() + boost::posix_time::milliseconds(tx_interval));
		timer.async_wait(rozeslij_dane);
	}
}

int main(int argc, char** argv)
{
	po::options_description desc("Opcje");
	
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
	udp_sock = udp::socket(io_service, udp_endpoint);
	
	tcp_sock = new tcp::socket(io_service);
	
	acceptor.listen();
	acceptor.async_accept(*tcp_sock, przyjmij_polaczenie);
	udp_sock.async_receive_from(ba::buffer(bufor), endpoint_klienta, odczytaj_dane_udp);
	polaczenie_timer.async_wait(sprawdz_polaczenia);
	raport_timer.async_wait(wygeneruj_raport);
	
	sygnaly.async_wait(przechwycono_sygnal);
	
	timer.expires_from_now(boost::posix_time::milliseconds(tx_interval));
	timer.async_wait(rozeslij_dane);
	
	io_service.run();
}
