#include "stdafx.h"
#include "WebGateway.h"
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#endif

namespace
{
	const std::string& Key()
	{
		static const std::string key = []
		{
			const char* value = std::getenv("MU_WEB_GATEWAY_KEY");
			return std::string(value ? value : "");
		}();
		return key;
	}

	// Comparación sin cortar al primer byte distinto.
	bool SameKey(const std::string& given)
	{
		const std::string& key = Key();
		unsigned char diff = static_cast<unsigned char>(given.size() != key.size());
		for (size_t i = 0; i < key.size(); ++i)
			diff |= static_cast<unsigned char>(key[i] ^ (i < given.size() ? given[i] : 0));
		return diff == 0;
	}
}

bool WebGateway::Enabled()
{
#ifdef _WIN32
	return false;
#else
	return Key().size() >= 16;
#endif
}

WORD WebGateway::Port(WORD serverPort)
{
	const char* value = std::getenv("MU_WEB_PORT_OFFSET");
	const int offset = (value && *value) ? std::atoi(value) : 100;
	return static_cast<WORD>(serverPort + offset);
}

#ifdef _WIN32

SOCKET WebGateway::Listen(WORD) { return INVALID_SOCKET; }
SOCKET WebGateway::Accept(SOCKET, char*, int) { return INVALID_SOCKET; }

#else

SOCKET WebGateway::Listen(WORD port)
{
	SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET) return INVALID_SOCKET;
	int opt = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
	SOCKADDR_IN addr {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(0);
	addr.sin_port = htons(port);
	if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR || listen(s, 16) == SOCKET_ERROR)
	{
		close(s);
		return INVALID_SOCKET;
	}
	return s;
}

SOCKET WebGateway::Accept(SOCKET listenSocket, char* ip, int ipSize)
{
	if (listenSocket == INVALID_SOCKET) return INVALID_SOCKET;
	SOCKET s = accept(listenSocket, nullptr, nullptr);
	if (s == SOCKET_ERROR) return INVALID_SOCKET;

	// El encabezado llega enseguida (mu-web lo escribe al conectar); se lee de a
	// un byte para no consumir nada del protocolo que viene detrás.  Tope de
	// 2 s y 160 bytes: el puerto es interno, pero no tiene que trabar el accept.
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) & ~O_NONBLOCK);
	std::string line;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	bool complete = false;
	while (line.size() < 160)
	{
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
		if (left <= 0) break;
		pollfd fd { s, POLLIN, 0 };
		if (poll(&fd, 1, static_cast<int>(left)) <= 0) break;
		char c;
		if (recv(s, &c, 1, 0) != 1) break;
		if (c == '\n') { complete = true; break; }
		if (c != '\r') line.push_back(c);
	}

	// "MUWEB <clave> <ip>"
	const size_t first = line.find(' ');
	const size_t second = first == std::string::npos ? std::string::npos : line.find(' ', first + 1);
	in_addr parsed {};
	if (!complete || second == std::string::npos || line.compare(0, first, "MUWEB") != 0
		|| !SameKey(line.substr(first + 1, second - first - 1))
		|| inet_pton(AF_INET, line.c_str() + second + 1, &parsed) != 1
		|| static_cast<int>(line.size() - second) > ipSize)
	{
		close(s);
		return INVALID_SOCKET;
	}

	std::strcpy(ip, line.c_str() + second + 1);
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
	return s;
}

#endif
