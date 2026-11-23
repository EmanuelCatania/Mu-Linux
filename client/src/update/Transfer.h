#pragma once

// Transporte de las descargas y limitador de velocidad.
//
// El transporte es una interfaz: el launcher usa HTTP (libcurl, en su propio
// target) y este núcleo trae el de archivos locales, que sirve para instalar
// desde una copia (un pendrive, una carpeta compartida) y para las pruebas.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace Update {

// Recibe los bytes en orden.  Devolver false corta la descarga (cancelación).
using DataSink = std::function<bool(const std::uint8_t* data, std::size_t size)>;

class Transport
{
public:
	virtual ~Transport() = default;
	// Pide los bytes [offset, offset + length) de url (un solo HTTP Range) y los
	// entrega a sink.  true sólo si llegaron exactamente length bytes.  Se llama
	// desde varios hilos a la vez.
	virtual bool Fetch(const std::string& url, std::uint64_t offset, std::uint64_t length,
	                   const DataSink& sink, std::string* error) = 0;
};

// Rutas locales y URLs file:// ("file:///C:/mu/x.zip", "file:///home/x.zip").
class FileTransport : public Transport
{
public:
	bool Fetch(const std::string& url, std::uint64_t offset, std::uint64_t length,
	           const DataSink& sink, std::string* error) override;
};

// "file:///C:/a" -> "C:/a", "file:///a" -> "/a"; vacío si no es una URL file://.
std::string FileUrlToPath(const std::string& url);

// Cubo de fichas común a todas las conexiones: el límite es del total, no de
// cada una.  Se puede cambiar en marcha (al abrir el juego baja a 512 KB/s) y
// pausar.
class RateLimiter
{
public:
	explicit RateLimiter(std::uint64_t bytesPerSecond = 0);

	void SetRate(std::uint64_t bytesPerSecond);   // 0: sin límite
	std::uint64_t Rate() const;
	void SetPaused(bool paused);
	bool Paused() const;

	// Espera hasta poder pasar size bytes.  false si cancel se prendió mientras
	// esperaba.
	bool Acquire(std::size_t size, const std::atomic<bool>* cancel);

	// Despierta a los que esperan (para que vean una cancelación).
	void Wake();

private:
	using Clock = std::chrono::steady_clock;
	void Refill(Clock::time_point now);

	mutable std::mutex m_Mutex;
	std::condition_variable m_Cv;
	std::uint64_t m_Rate = 0;
	double m_Tokens = 0;
	double m_Burst = 0;
	bool m_Paused = false;
	Clock::time_point m_Last;
};

} // namespace Update
