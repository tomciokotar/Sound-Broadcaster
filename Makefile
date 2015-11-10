TARGET: client server

client: client.cpp
	g++ client.cpp -std=c++11 -o client -lboost_system -lpthread -lboost_regex -lboost_program_options

server: server.cpp mixer.hpp
	g++ server.cpp -std=c++11 -o server -lboost_system -lpthread -lboost_regex -lboost_program_options 

.PHONY: clean TARGET
clean:
	rm -f client server *.o *~

