#ifndef COMMON_H
#define COMMON_H
#define _WIN32_WINNT 0x0601
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <sstream>
#include <map>
#include <vector>
#include <chrono>
#include <ctime>

#pragma comment(lib, "ws2_32.lib")

const int BUFFER_SIZE = 4096;

//Initializes Winsock (must be called before using sockets)
bool initialize_winsock();

//Cleans up Winsock
void cleanup_winsock();

/*
Sends a JSON-like string over a socket.
We manually serialize data into string format.
 */
bool send_json(SOCKET sock, const std::string& data);


// Receives JSON-like string from socket.
// Returns empty string if connection closed.
std::string recv_json(SOCKET sock);

//Creates SHA-256 hash of a message
std::string hash_message(const std::string& msg);

// Returns current timestamp as a formatted string "YYYY-MM-DD HH:MM:SS.mmm"
std::string get_timestamp();

#endif