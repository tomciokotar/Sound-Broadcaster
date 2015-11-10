# Sound Broadcaster
An application developed using Boost.Asio being able to broadcast sound via network to multiple clients (aka mini-Skype). It uses it's own, manually implemented TCP/UDP protocol.

A project from Computer networks course.

Full task description (in Polish): task.html

Requirements: Boost and sox with mp3 support

# Compilation
```$ make```

# How to run
1. Run the server with ```$ ./server```.
2. Run one of the scripts with parameter ```-s <server's name or IP address>``` (localhost by default).

Scripts:
- player.sh - plays sample.mp3 (sends and receives sound at the same time)
- sender.sh - just sends the sound from sample.mp3
- receiver.sh - receives the sound only

You can try running a few different scripts at the same time.

# Parameters
- ```$ ./server -p <port>```
- ```$ ./player.sh -s <server> -p <port>```
- ```$ ./sender.sh -s <server> -p <port>```
- ```$ ./receiver.sh -s <server> -p <port>```

