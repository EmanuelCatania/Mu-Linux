#pragma once

// El trabajo del launcher sin la ventana: baja y valida los canales, arma el
// plan y lo instala, en un hilo aparte.  La ventana sólo lee el estado.
//
// Orden de los canales: base, música, HD (de los Releases del proyecto) y el
// overlay del operador.  Si el overlay trae una versión nueva del launcher que
// está corriendo (Config::selfPath), se instala sólo esa y el flujo termina en
// Restart: el nuevo procesa el resto (ver SelfUpdate.h).  Si no se puede
// verificar nada pero ya hay algo instalado, deja jugar con lo que hay y lo
// avisa.
//
// Antes que nada pide la configuración del server (launcher.json, ver
// ServerAnnounce.h): de ahí salen la URL y el canal de los parches, el origen
// de la base y las noticias.  Lo de launcher.toml (pruebas) sigue mandando
// sobre el origen de la base.
//
// Jugar mientras baja (diseño, 4b): con el juego cerrado se instala primero lo
// obligatorio; ahí se habilita Jugar y sigue lo opcional (música, HD).  Con el
// juego abierto (User/Cache/running.lock, platform/RunningLock.h) se baja con
// una conexión y el límite de "jugando", y no se pisa ningún archivo que ya
// existe: esos se instalan cuando el juego se cierra.

#include "LauncherConfig.h"

#include "update/Transfer.h"

#include <atomic>
#include <chrono>
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
	Restart,       // se instaló un launcher nuevo: hay que arrancarlo y salir
};

struct Status
{
	Phase phase = Phase::Checking;
	std::string message;
	std::uint64_t done = 0;
	std::uint64_t total = 0;
	bool canPlay = false;
	std::vector<std::string> notices;  // avisos (configuración de prueba, etc.)
	std::string news;                  // lo que anuncia el server (launcher.json)
	bool background = false;           // bajando lo opcional: ya se puede jugar
	bool playing = false;              // hay un cliente corriendo
	bool paused = false;
	std::size_t waitingForClose = 0;   // archivos que esperan a que se cierre el juego
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
	// El jugador apretó Jugar en este launcher: se baja como "jugando" ya, sin
	// esperar a que el cliente tome su lock.
	void SetPlaying();
	void SetPaused(bool paused);

private:
	void Run();
	void Set(Phase phase, const std::string& message);
	// Mira el lock del cliente (como mucho cada medio segundo) y ajusta la
	// velocidad y las conexiones.
	void PollGame(bool force = false);

	Config m_Config;
	std::string m_Root;
	std::unique_ptr<Update::Transport> m_Transport;
	Update::RateLimiter m_Limiter;
	mutable std::mutex m_Mutex;
	Status m_Status;
	std::atomic<bool> m_Cancel{ false };
	std::atomic<bool> m_Finished{ false };
	std::string m_LockPath;
	std::atomic<bool> m_GameRunning{ false };
	std::atomic<int> m_MaxConnections{ 4 };
	std::atomic<bool> m_Paused{ false };
	std::atomic<std::int64_t> m_PlayingSince{ 0 };   // ms del reloj; 0 = nunca
	std::mutex m_PollMutex;
	std::chrono::steady_clock::time_point m_LastPoll{};
	std::thread m_Thread;
};

} // namespace Launcher
