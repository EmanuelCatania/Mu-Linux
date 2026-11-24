// Ver GuardDetect.h.  Sin stdafx.h.

#include "guard/GuardDetect.h"

#include "guard/GuardProtocol.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <fstream>
#include <unistd.h>
#endif

namespace Guard {

namespace {

// Módulos de Cheat Engine: su depurador por excepciones (VEH), el speedhack y
// el resto de lo que inyecta en el proceso que edita.
const char* const kKnownBadModules[] = {
	"vehdebug", "speedhack", "cheatengine", "allochook", "luaclient", "winhook",
};

// Drivers de Cheat Engine: leen la memoria sin abrir el proceso, así que la
// DACL no los frena.
const char* const kKnownBadDrivers[] = { "dbk64.sys", "dbk32.sys" };

// Las pasadas caras (drivers, módulos, /proc) van cada tantas pasadas.
constexpr int kSlowEvery = 5;

std::string Lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

std::string BaseName(const std::string& path)
{
	const std::size_t slash = path.find_last_of("\\/");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool IsKnownBadModule(const std::string& lowerPath)
{
	const std::string name = BaseName(lowerPath);
	for (const char* bad : kKnownBadModules)
		if (name.find(bad) != std::string::npos) return true;
	return false;
}

bool StartsWith(const std::string& s, const std::string& prefix)
{
	return !prefix.empty() && s.compare(0, prefix.size(), prefix) == 0;
}

void Add(std::vector<Finding>& found, std::uint8_t code, bool closes, const std::string& detail)
{
	Finding f;
	f.code = code;
	f.closes = closes;
	f.detail = detail.size() > GuardProto::MaxDetail ? detail.substr(0, GuardProto::MaxDetail) : detail;
	found.push_back(f);
}

#ifdef _WIN32

std::string Narrow(const wchar_t* wide)
{
	char out[1024];
	const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, sizeof(out), nullptr, nullptr);
	return n > 0 ? std::string(out) : std::string();
}

using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using QueryWorkingSetExFn = BOOL(WINAPI*)(HANDLE, PVOID, DWORD);
using EnumProcessModulesExFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD, DWORD);
using GetModuleFileNameExWFn = DWORD(WINAPI*)(HANDLE, HMODULE, LPWSTR, DWORD);

constexpr ULONG kProcessDebugPort = 7;
constexpr ULONG kProcessDebugObjectHandle = 30;
constexpr ULONG kProcessDebugFlags = 31;
constexpr ULONG kSystemModuleInformation = 11;

struct ModuleEntry
{
	HANDLE section;
	PVOID mappedBase;
	PVOID imageBase;
	ULONG imageSize;
	ULONG flags;
	USHORT loadOrderIndex;
	USHORT initOrderIndex;
	USHORT loadCount;
	USHORT offsetToFileName;
	UCHAR fullPathName[256];
};

struct ModuleList
{
	ULONG count;
	ModuleEntry modules[1];
};

// Páginas señuelo (MEMSCAN): memoria del juego que el juego nunca toca.  Si
// aparece en su working set es que alguien la leyó, que es lo que hace el
// "First Scan" de un editor de memoria.
constexpr int kDecoyRegions = 8;
constexpr SIZE_T kDecoyRegion = 64 * 1024;

#endif

} // namespace

struct Detector::State
{
	int passes = 0;
	bool memScanCloses = true;
	std::string gameDir;   // en minúsculas, con barra final
	std::set<std::string> reported;
	unsigned long pid = 0;
#ifdef _WIN32
	HANDLE process = nullptr;
	HANDLE mainThread = nullptr;
	std::string windowsDir;
	NtQueryInformationProcessFn queryProcess = nullptr;
	NtQuerySystemInformationFn querySystem = nullptr;
	QueryWorkingSetExFn queryWorkingSet = nullptr;
	EnumProcessModulesExFn enumModules = nullptr;
	GetModuleFileNameExWFn moduleName = nullptr;
	std::vector<void*> decoys;
	bool decoyReported = false;
#endif

	bool Once(const std::string& key)
	{
		return reported.insert(key).second;
	}

#ifdef _WIN32
	void Debugger(std::vector<Finding>& found)
	{
		if (IsDebuggerPresent())
		{
			if (Once("debugger:self")) Add(found, GuardProto::Debugger, true, "depurador sobre el guardian");
			return;
		}
		BOOL remote = FALSE;
		if (CheckRemoteDebuggerPresent(process, &remote) && remote)
		{
			if (Once("debugger:remote")) Add(found, GuardProto::Debugger, true, "depurador sobre el juego");
			return;
		}
		if (queryProcess)
		{
			ULONG_PTR port = 0;
			if (queryProcess(process, kProcessDebugPort, &port, sizeof(port), nullptr) >= 0 && port != 0)
			{
				if (Once("debugger:port")) Add(found, GuardProto::Debugger, true, "puerto de depuracion del juego");
				return;
			}
			HANDLE object = nullptr;
			if (queryProcess(process, kProcessDebugObjectHandle, &object, sizeof(object), nullptr) >= 0 && object)
			{
				CloseHandle(object);
				if (Once("debugger:object")) Add(found, GuardProto::Debugger, true, "objeto de depuracion del juego");
				return;
			}
			ULONG flags = 1;
			if (queryProcess(process, kProcessDebugFlags, &flags, sizeof(flags), nullptr) >= 0 && flags == 0)
			{
				if (Once("debugger:flags")) Add(found, GuardProto::Debugger, true, "banderas de depuracion del juego");
				return;
			}
		}
		// Registros de depuración (Dr0-Dr3 con Dr7 encendido): los usan los
		// depuradores por hardware y el VEH de Cheat Engine.
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snapshot == INVALID_HANDLE_VALUE) return;
		THREADENTRY32 entry{};
		entry.dwSize = sizeof(entry);
		for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
		{
			if (entry.th32OwnerProcessID != pid) continue;
			HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
			// El primer hilo tiene la DACL del juego: se usa el handle de CreateProcess.
			if (!thread && mainThread && GetThreadId(mainThread) == entry.th32ThreadID) thread = mainThread;
			if (!thread) continue;
			CONTEXT context{};
			context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
			const bool hardware = GetThreadContext(thread, &context) && (context.Dr7 & 0xFF) != 0 &&
			                      (context.Dr0 || context.Dr1 || context.Dr2 || context.Dr3);
			if (thread != mainThread) CloseHandle(thread);
			if (hardware)
			{
				if (Once("debugger:hardware")) Add(found, GuardProto::Debugger, true, "registros de depuracion en un hilo del juego");
				break;
			}
		}
		CloseHandle(snapshot);
	}

	void Drivers(std::vector<Finding>& found)
	{
		if (!querySystem) return;
		ULONG size = 0;
		querySystem(kSystemModuleInformation, nullptr, 0, &size);
		if (size == 0) return;
		std::vector<std::uint8_t> buffer(size + 4096);
		if (querySystem(kSystemModuleInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &size) < 0) return;
		const ModuleList* list = reinterpret_cast<const ModuleList*>(buffer.data());
		for (ULONG i = 0; i < list->count; ++i)
		{
			const ModuleEntry& module = list->modules[i];
			const char* path = reinterpret_cast<const char*>(module.fullPathName);
			const std::string name = Lower(path + (module.offsetToFileName < 256 ? module.offsetToFileName : 0));
			for (const char* bad : kKnownBadDrivers)
			{
				if (name == bad && Once(std::string("driver:") + bad))
					Add(found, GuardProto::MemEdit, true, std::string("driver de Cheat Engine cargado: ") + name);
			}
		}
	}

	void PlantDecoys()
	{
		for (int i = 0; i < kDecoyRegions; ++i)
		{
			void* region = VirtualAllocEx(process, nullptr, kDecoyRegion, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			if (region) decoys.push_back(region);
		}
	}

	void Decoys(std::vector<Finding>& found)
	{
		if (!queryWorkingSet || decoys.empty() || decoyReported) return;
		const SIZE_T pages = kDecoyRegion / 4096;
		std::vector<PSAPI_WORKING_SET_EX_INFORMATION> info(decoys.size() * pages);
		for (std::size_t r = 0; r < decoys.size(); ++r)
			for (SIZE_T p = 0; p < pages; ++p)
				info[r * pages + p].VirtualAddress = static_cast<std::uint8_t*>(decoys[r]) + p * 4096;
		if (!queryWorkingSet(process, info.data(), static_cast<DWORD>(info.size() * sizeof(info[0])))) return;
		int touched = 0;
		for (const auto& page : info)
			if (page.VirtualAttributes.Valid) ++touched;
		if (touched > 0)
		{
			decoyReported = true;
			char detail[96];
			std::snprintf(detail, sizeof(detail), "%d de %u paginas senuelo leidas", touched, static_cast<unsigned>(info.size()));
			// Cierra salvo que el server lo haya apagado (Guard.toml MemScanCloses).
			Add(found, GuardProto::MemScan, memScanCloses, detail);
		}
	}

	// Funciones de tiempo enganchadas (el speedhack de Cheat Engine): las DLL del
	// sistema están en la misma dirección en todos los procesos de la sesión, así
	// que el comienzo de cada función en el juego tiene que ser igual al del
	// guardián.  El speedhack no aparece como módulo, pero tiene que reescribir
	// estas funciones para acelerar el juego.  Los overlays (OBS, Discord)
	// enganchan funciones de dibujo, no éstas.
	void TimeHooks(std::vector<Finding>& found)
	{
		static const struct { const wchar_t* module; const char* function; } kFunctions[] = {
			{ L"kernel32.dll", "QueryPerformanceCounter" }, { L"kernel32.dll", "GetTickCount" },
			{ L"kernel32.dll", "GetTickCount64" }, { L"kernelbase.dll", "QueryPerformanceCounter" },
			{ L"kernelbase.dll", "GetTickCount" }, { L"kernelbase.dll", "GetTickCount64" },
			{ L"ntdll.dll", "RtlQueryPerformanceCounter" }, { L"ntdll.dll", "NtQueryPerformanceCounter" },
			{ L"winmm.dll", "timeGetTime" },
		};
		constexpr SIZE_T kBytes = 16;
		for (const auto& entry : kFunctions)
		{
			HMODULE module = GetModuleHandleW(entry.module);
			const void* address = module ? reinterpret_cast<const void*>(GetProcAddress(module, entry.function)) : nullptr;
			if (!address) continue;
			std::uint8_t mine[kBytes], theirs[kBytes];
			SIZE_T read = 0;
			std::memcpy(mine, address, kBytes);
			if (!ReadProcessMemory(process, address, theirs, kBytes, &read) || read != kBytes) continue;
			if (std::memcmp(mine, theirs, kBytes) != 0)
			{
				const std::string name = Narrow(entry.module) + "!" + entry.function;
				if (Once("hook:" + name)) Add(found, GuardProto::Inject, true, "funcion de tiempo enganchada (speedhack): " + name);
			}
		}
	}

	void Modules(std::vector<Finding>& found)
	{
		if (!enumModules || !moduleName) return;
		std::vector<HMODULE> modules(1024);
		DWORD needed = 0;
		if (!enumModules(process, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL)) return;
		modules.resize(std::min<std::size_t>(modules.size(), needed / sizeof(HMODULE)));
		for (HMODULE module : modules)
		{
			wchar_t wide[MAX_PATH * 2];
			if (!moduleName(process, module, wide, static_cast<DWORD>(sizeof(wide) / sizeof(wide[0])))) continue;
			const std::string path = Lower(Narrow(wide));
			if (IsKnownBadModule(path))
			{
				if (Once("module:" + path)) Add(found, GuardProto::Inject, true, "modulo de Cheat Engine: " + BaseName(path));
			}
			else if (!StartsWith(path, windowsDir) && !StartsWith(path, gameDir) && Once("module:" + path))
			{
				Add(found, GuardProto::Inject, false, "modulo fuera del juego y del sistema: " + path);
			}
		}
	}
#else
	static std::string ReadText(const std::string& path)
	{
		std::ifstream file(path, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	}

	static long TracerPid(const std::string& statusPath)
	{
		const std::string status = ReadText(statusPath);
		const std::size_t at = status.find("TracerPid:");
		return at == std::string::npos ? 0 : std::strtol(status.c_str() + at + 10, nullptr, 10);
	}

	void Debugger(std::vector<Finding>& found)
	{
		if (TracerPid("/proc/self/status") != 0)
		{
			if (Once("debugger:self")) Add(found, GuardProto::Debugger, true, "depurador sobre el guardian");
			return;
		}
		const long tracer = TracerPid("/proc/" + std::to_string(pid) + "/status");
		if (tracer != 0 && tracer != ::getpid() && Once("debugger:game"))
			Add(found, GuardProto::Debugger, true, "depurador sobre el juego (pid " + std::to_string(tracer) + ")");
	}

	// Otro proceso con /proc/<juego>/mem abierto: un editor de memoria (scanmem,
	// GameConqueror, PINCE).  Sólo se ven los del mismo usuario.
	void MemFiles(std::vector<Finding>& found)
	{
		const std::string target = "/proc/" + std::to_string(pid) + "/mem";
		DIR* proc = opendir("/proc");
		if (!proc) return;
		const std::string self = std::to_string(::getpid()), game = std::to_string(pid);
		while (dirent* entry = readdir(proc))
		{
			if (!std::isdigit(static_cast<unsigned char>(entry->d_name[0])) || self == entry->d_name || game == entry->d_name) continue;
			const std::string fdDir = std::string("/proc/") + entry->d_name + "/fd";
			DIR* fds = opendir(fdDir.c_str());
			if (!fds) continue;
			while (dirent* fd = readdir(fds))
			{
				if (fd->d_name[0] == '.') continue;
				char link[512];
				const ssize_t n = ::readlink((fdDir + "/" + fd->d_name).c_str(), link, sizeof(link) - 1);
				if (n <= 0) continue;
				link[n] = 0;
				if (target == link)
				{
					const std::string comm = ReadText(std::string("/proc/") + entry->d_name + "/comm");
					if (Once(std::string("memedit:") + entry->d_name))
						Add(found, GuardProto::MemEdit, true, "memoria del juego abierta por " + comm.substr(0, comm.find('\n')));
					break;
				}
			}
			closedir(fds);
		}
		closedir(proc);
	}

	void Modules(std::vector<Finding>& found)
	{
		std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
		std::string line;
		while (std::getline(maps, line))
		{
			const std::size_t slash = line.find('/');
			if (slash == std::string::npos) continue;
			const std::string path = line.substr(slash);
			if (path.find(".so") == std::string::npos) continue;
			const std::string lower = Lower(path);
			if (IsKnownBadModule(lower))
			{
				if (Once("module:" + path)) Add(found, GuardProto::Inject, true, "modulo de Cheat Engine: " + BaseName(path));
			}
			else if (!StartsWith(lower, "/usr/") && !StartsWith(lower, "/lib") && !StartsWith(lower, gameDir) && Once("module:" + path))
			{
				Add(found, GuardProto::Inject, false, "modulo fuera del juego y del sistema: " + path);
			}
		}
		// LD_PRELOAD: lo usan herramientas legítimas (MangoHud, gamemode); se
		// reporta para armar la lista.
		const std::string environ = ReadText("/proc/" + std::to_string(pid) + "/environ");
		for (std::size_t at = 0; at < environ.size();)
		{
			const std::size_t end = environ.find('\0', at);
			const std::string var = environ.substr(at, end == std::string::npos ? std::string::npos : end - at);
			if (StartsWith(var, "LD_PRELOAD=") && Once("preload:" + var))
				Add(found, GuardProto::Inject, IsKnownBadModule(Lower(var)), "LD_PRELOAD: " + var.substr(11));
			if (end == std::string::npos) break;
			at = end + 1;
		}
	}
#endif
};

Detector::Detector() : m(new State) {}

Detector::~Detector()
{
	delete m;
}

void Detector::Attach(void* process, void* mainThread, unsigned long pid, const std::string& gameDir)
{
	m->pid = pid;
	m->gameDir = Lower(gameDir);
	if (!m->gameDir.empty() && m->gameDir.back() != '/' && m->gameDir.back() != '\\') m->gameDir += '/';
#ifdef _WIN32
	std::replace(m->gameDir.begin(), m->gameDir.end(), '/', '\\');
	m->process = static_cast<HANDLE>(process);
	m->mainThread = static_cast<HANDLE>(mainThread);
	wchar_t windows[MAX_PATH];
	if (GetWindowsDirectoryW(windows, MAX_PATH)) m->windowsDir = Lower(Narrow(windows)) + "\\";
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	m->queryProcess = reinterpret_cast<NtQueryInformationProcessFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryInformationProcess")));
	m->querySystem = reinterpret_cast<NtQuerySystemInformationFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQuerySystemInformation")));
	m->queryWorkingSet = reinterpret_cast<QueryWorkingSetExFn>(reinterpret_cast<void*>(GetProcAddress(kernel, "K32QueryWorkingSetEx")));
	m->enumModules = reinterpret_cast<EnumProcessModulesExFn>(reinterpret_cast<void*>(GetProcAddress(kernel, "K32EnumProcessModulesEx")));
	m->moduleName = reinterpret_cast<GetModuleFileNameExWFn>(reinterpret_cast<void*>(GetProcAddress(kernel, "K32GetModuleFileNameExW")));
	m->PlantDecoys();
#else
	(void)process;
	(void)mainThread;
#endif
}

void Detector::SetMemScanCloses(bool closes)
{
	m->memScanCloses = closes;
}

void Detector::Scan(std::vector<Finding>& found)
{
	const bool slow = (m->passes++ % kSlowEvery) == 0;
	m->Debugger(found);
#ifdef _WIN32
	m->Decoys(found);
	m->TimeHooks(found);
	if (slow)
	{
		m->Drivers(found);
		m->Modules(found);
	}
#else
	if (slow)
	{
		m->MemFiles(found);
		m->Modules(found);
	}
#endif
}

} // namespace Guard
