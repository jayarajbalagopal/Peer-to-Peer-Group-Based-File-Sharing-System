# ASSIGNMENT 3

### NOTE
The application was developed in MAC OS which uses clang++ compiler internally.
If the code is to be compiled on ubuntu, use clang++ compiler instead of g++.

### COMPILING
```
clang++ --std=c++14 tracker.cpp -o tracker -lpthread
clang++ --std=c++14 client.cpp -o client -lpthread
```

### USAGE
#### TRACKER INFO FILE
```
jayarajbalagopal@Jayarajs-MacBook-Air peer_to_peer % cat tracker_info.txt 
127.0.0.1:8088
127.0.0.1:8089
```
#### STARTING TRACKER AND CLIENT
```
./tracker tracker_info.txt 0
./client 127.0.0.1:8083 tracker_info.txt
```
