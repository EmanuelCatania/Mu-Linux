#pragma once

// El trabajo del launcher sin la ventana: baja y valida los canales, arma el
// plan y lo instala, en un hilo aparte.  La ventana sólo lee el estado.
//
// Orden de los canales: base, música, HD (de los Releases del proyecto) y el
// overlay del operador.  Si no se puede verificar nada pero ya hay algo
// instalado, deja jugar con lo que hay y lo avisa.

#include "LauncherConfig.h"

#include "update/Transfer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Launcher {

enum class Phase
{
	Checking,      // bajando y validando manifiestos
	Downloading,
	Ready,         // todo al día
	Offline,       // no se pudo verificar; se puede jugar con lo instalado
	Failed,        // no se puede jugar
};

struct Status
{
	Phase phase = Phase::Checking;
	std::string message;
	std::uint64_t done = 0;
	std::uint64_t total = 0;
	bool canPlay = false;
	bool newLauncher = false;          // el overlay trae un launcher más nuevo
	std::vector<std::string> notices;  // avisos (configuración de prueba, etc.)
};

// Elige el transporte por esquema: file:// y rutas locales, o HTTP(S).
class AnyTransport : public Update::Transport
{
public:
	explicit AnyTransport(std::unique_ptr<Update::Transport> http);
	bool Fetch(const std::string& url, std::uint64_t offset, std::uint64_t length,
	           const Update::DataSink& sink, std::string* error) override;
	bool Get(const std::string& url, std::uint64_t maxSize, std::vector<std::uint8_t>& out,
	         std::string* error) override;

private:
	Update::Transport& Pick(const std::string& url);
	Update::FileTransport m_File;
	std::unique_ptr<Update::Transport> m_Http;
};

class UpdateFlow
{
public:
	UpdateFlow(Config config, std::string root, std::unique_ptr<Update::Transport> transport);
	~UpdateFlow();

	void Start();
	Status Snapshot() const;
	bool Finished() const { return m_Finished.load(); }
	void Cancel();
	// Bloquea hasta que termine (para las pruebas).
	void Wait();

private:
	void Run();
	void Set(Phase phase, const std::string& message);

	Config m_Config;
	std::string m_Root;
	std::unique_ptr<Update::Transport> m_Transport;
	Update::RateLimiter m_Limiter;
	mutable std::mutex m_Mutex;
	Status m_Status;
	std::atomic<bool> m_Cancel{ false };
	std::atomic<bool> m_Finished{ false };
	std::thread m_Thread;
};

} // namespace Launcher
