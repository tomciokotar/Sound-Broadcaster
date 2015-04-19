TARGET: klient serwer

klient: klient.cpp
	g++ klient.cpp -std=c++11 -o klient -lboost_system -lpthread -lboost_regex -lboost_program_options

serwer: serwer.cpp mixer.hpp
	g++ serwer.cpp -std=c++11 -o serwer -lboost_system -lpthread -lboost_regex -lboost_program_options 

.PHONY: clean TARGET
clean:
	rm -f klient serwer *.o *~

