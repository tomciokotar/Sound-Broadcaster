# Sieci
Aplikacja napisana przy użyciu Boost.Asio do przesyłania dźwięku po sieci pomiędzy wieloma klientami (taki mini Skype). Cała trudność polegała na ręcznej obsłudze wszystkich pakietów TCP/UDP według zadanego protokołu.

Zadanie zaliczeniowe z sieci komputerowych.

Treść zadania w zadanko.html.

# Kompilacja
```make``` (wymagany Boost)

# Uruchamianie
1. Na początku uruchamiamy serwer ```./serwer```.
2. Uruchamiamy jeden ze skryptów z parametrem ```-s <nazwa lub IP serwera>``` (domyślnie localhost).
- player.sh - odtwarza sample.mp3 (jednocześnie wysyła i odbiera dźwięk)
- sender.sh - tylko wysyła dźwięk
- receiver.sh - tylko odbiera dźwięk

Można się pobawić odpalając kilka skryptów naraz.

# Parametry
```./serwer -p <port>```
```./player.sh -s <serwer> -p <port>```
```./sender.sh -s <serwer> -p <port>```
```./receiver.sh -s <serwer> -p <port>```

