// Ver GuardDetect.h.  Sin stdafx.h.

#include "guard/GuardDetect.h"

#include "guard/GuardProtocol.h"
#include "net/crypto/monocypher.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#else
#include <dirent.h>
#include <fcntl.h>
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

// Las pasadas caras (drivers, módulos, procesos, /proc) van cada tantas
// pasadas (cada 10 s).
constexpr int kSlowEvery = 5;

// FILES: cuánto se lee por pasada (cada 2 s) y cada cuánto se vuelve a verificar.
constexpr std::size_t kFilesBytesPerPass = 4 * 1024 * 1024;
constexpr int kFilesRecheckSeconds = 600;

const char* const kDetectionNames[] = { "?", "DEBUGGER", "INJECT", "MEMEDIT", "MEMSCAN", "PATCH", "FILES", "TOOL", "KILLED" };

bool Contains(const std::vector<std::string>& list, const std::string& value)
{
	for (const std::string& item : list)
		if (item == value) return true;
	return false;
}

// Alguna entrada de la lista es parte de `value`.
const std::string* FindPart(const std::vector<std::string>& list, const std::string& value)
{
	for (const std::string& item : list)
		if (!item.empty() && value.find(item) != std::string::npos) return &item;
	return nullptr;
}

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
using WinVerifyTrustFn = LONG(WINAPI*)(HWND, GUID*, LPVOID);
using ProvDataFromStateDataFn = CRYPT_PROVIDER_DATA*(WINAPI*)(HANDLE);
using ProvSignerFromChainFn = CRYPT_PROVIDER_SGNR*(WINAPI*)(CRYPT_PROVIDER_DATA*, DWORD, BOOL, DWORD);
using ProvCertFromChainFn = CRYPT_PROVIDER_CERT*(WINAPI*)(CRYPT_PROVIDER_SGNR*, DWORD);
using CertGetNameStringWFn = DWORD(WINAPI*)(PCCERT_CONTEXT, DWORD, DWORD, void*, LPWSTR, DWORD);

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
	std::uint32_t life[GuardProto::LifeCounters] = {};   // contadores de vida (KILLED)
	bool memScanCloses = true;
	bool toolCloses = true;
	bool filesCloses = true;
	// FILES: lo que dice el server (Files.txt) y por dónde va la verificación.
	struct FileEntry
	{
		std::string path;   // relativa a la carpeta del juego, con '/'
		std::uint8_t hash[GuardProto::HashSize];
	};
	std::vector<FileEntry> files;
	std::size_t fileCursor = 0;
	FILE* fileOpen = nullptr;
	crypto_blake2b_ctx fileHash;
	std::chrono::steady_clock::time_point filesNext {};
	// Listas del server (Lists.toml), en minúsculas.
	std::vector<std::string> processes, windows, moduleBlock, moduleAllow;
	std::map<std::string, std::string> names;   // "idioma\tDETECCION" -> nombre
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
	// Hilos del juego: el handle se abre una vez, con lo que el guardián
	// necesita, antes de ponerle la DACL (después ya no se podría abrir).
	PACL threadDacl = nullptr;
	std::map<DWORD, HANDLE> threads;
	// INJECT: módulos fuera del sistema firmados por Microsoft (por ruta).
	std::map<std::string, bool> microsoft;
	HMODULE wintrust = nullptr;
	HMODULE crypt32 = nullptr;
	WinVerifyTrustFn verifyTrust = nullptr;
	ProvDataFromStateDataFn provData = nullptr;
	ProvSignerFromChainFn provSigner = nullptr;
	ProvCertFromChainFn provCert = nullptr;
	CertGetNameStringWFn certName = nullptr;
#endif

	bool Once(const std::string& key)
	{
		return reported.insert(key).second;
	}

	void FilesReset()
	{
		if (fileOpen) std::fclose(fileOpen);
		fileOpen = nullptr;
		fileCursor = 0;
		filesNext = std::chrono::steady_clock::time_point {};
	}

	// Verifica de a kFilesBytesPerPass: el juego ya tiene la carpeta de trabajo
	// en la del cliente, así que las rutas relativas del server sirven tal cual.
	void Files(std::vector<Finding>& found)
	{
		++life[GuardProto::LifeFiles];
		if (files.empty()) return;
		const auto now = std::chrono::steady_clock::now();
		if (fileCursor >= files.size())
		{
			if (now < filesNext) return;
			fileCursor = 0;
		}
		std::size_t budget = kFilesBytesPerPass;
		std::vector<std::uint8_t> buffer(64 * 1024);
		while (budget > 0 && fileCursor < files.size())
		{
			const FileEntry& entry = files[fileCursor];
			if (!fileOpen)
			{
				fileOpen = std::fopen(entry.path.c_str(), "rb");
				if (!fileOpen)
				{
					if (Once("files:" + entry.path))
						Add(found, GuardProto::Files, filesCloses, "falta un archivo del juego: " + entry.path);
					++fileCursor;
					continue;
				}
				crypto_blake2b_init(&fileHash, GuardProto::HashSize);
			}
			const std::size_t want = (std::min)(budget, buffer.size());
			const std::size_t read = std::fread(buffer.data(), 1, want, fileOpen);
			if (read > 0)
			{
				crypto_blake2b_update(&fileHash, buffer.data(), read);
				budget -= read;
			}
			if (read < want)
			{
				std::fclose(fileOpen);
				fileOpen = nullptr;
				std::uint8_t hash[GuardProto::HashSize];
				crypto_blake2b_final(&fileHash, hash);
				if (std::memcmp(hash, entry.hash, sizeof(hash)) != 0 && Once("files:" + entry.path))
					Add(found, GuardProto::Files, filesCloses, "archivo del juego modificado: " + entry.path);
				++fileCursor;
			}
		}
		if (fileCursor >= files.size()) filesNext = now + std::chrono::seconds(kFilesRecheckSeconds);
	}

#ifdef _WIN32
	// Hilos del juego: abre los nuevos y les pone la misma DACL que al primero
	// (diseño 3.1).  CreateProcess sólo protege el hilo que crea; los que el
	// juego crea después nacen con la DACL por defecto del usuario, y con eso
	// cualquier programa suyo los puede suspender o leerles el contexto.  Queda
	// una ventana de hasta una pasada (2 s) entre que nace un hilo y su DACL.
	void Threads()
	{
		for (auto it = threads.begin(); it != threads.end();)
		{
			if (WaitForSingleObject(it->second, 0) == WAIT_OBJECT_0)
			{
				CloseHandle(it->second);
				it = threads.erase(it);
			}
			else ++it;
		}
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snapshot == INVALID_HANDLE_VALUE) return;
		THREADENTRY32 entry{};
		entry.dwSize = sizeof(entry);
		const DWORD mainId = mainThread ? GetThreadId(mainThread) : 0;
		for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
		{
			if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == mainId || threads.count(entry.th32ThreadID)) continue;
			HANDLE thread = OpenThread(WRITE_DAC | READ_CONTROL | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
			                           FALSE, entry.th32ThreadID);
			if (!thread) continue;
			if (threadDacl)
				SetSecurityInfo(thread, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
				                nullptr, nullptr, threadDacl, nullptr);
			threads[entry.th32ThreadID] = thread;
		}
		CloseHandle(snapshot);
	}

	void Debugger(std::vector<Finding>& found)
	{
		++life[GuardProto::LifeDebugger];
		if (IsDebuggerPresent())
		{
			if (Once("debugger:self")) Add(found, GuardProto::Debugger, true, "depurador sobre el guardi\xC3\xA1n");
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
				if (Once("debugger:port")) Add(found, GuardProto::Debugger, true, "puerto de depuraci\xC3\xB3n del juego");
				return;
			}
			HANDLE object = nullptr;
			if (queryProcess(process, kProcessDebugObjectHandle, &object, sizeof(object), nullptr) >= 0 && object)
			{
				CloseHandle(object);
				if (Once("debugger:object")) Add(found, GuardProto::Debugger, true, "objeto de depuraci\xC3\xB3n del juego");
				return;
			}
			ULONG flags = 1;
			if (queryProcess(process, kProcessDebugFlags, &flags, sizeof(flags), nullptr) >= 0 && flags == 0)
			{
				if (Once("debugger:flags")) Add(found, GuardProto::Debugger, true, "banderas de depuraci\xC3\xB3n del juego");
				return;
			}
		}
		// Registros de depuración (Dr0-Dr3 con Dr7 encendido): los usan los
		// depuradores por hardware y el VEH de Cheat Engine.  Por los handles de
		// Threads(): el primer hilo, por el de CreateProcess.
		std::vector<HANDLE> handles;
		if (mainThread) handles.push_back(mainThread);
		for (const auto& thread : threads) handles.push_back(thread.second);
		for (HANDLE thread : handles)
		{
			CONTEXT context{};
			context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
			if (GetThreadContext(thread, &context) && (context.Dr7 & 0xFF) != 0 &&
			    (context.Dr0 || context.Dr1 || context.Dr2 || context.Dr3))
			{
				if (Once("debugger:hardware")) Add(found, GuardProto::Debugger, true, "registros de depuraci\xC3\xB3n en un hilo del juego");
				break;
			}
		}
	}

	void Drivers(std::vector<Finding>& found)
	{
		++life[GuardProto::LifeDrivers];
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
		++life[GuardProto::LifeDecoys];
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
			std::snprintf(detail, sizeof(detail), "%d de %u p\xC3\xA1ginas se\xC3\xB1uelo le\xC3\xAD" "das", touched, static_cast<unsigned>(info.size()));
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
		++life[GuardProto::LifeTimeHooks];
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
				if (Once("hook:" + name)) Add(found, GuardProto::Inject, true, "funci\xC3\xB3n de tiempo enganchada (speedhack): " + name);
			}
		}
	}

	// Firmado por Microsoft: firma válida (embebida) cuyo firmante es Microsoft y
	// cuya raíz es una raíz de Microsoft.  Sin revocación ni red: es para no
	// reportar módulos legítimos que viven fuera de Windows (Office, OneDrive,
	// Edge WebView, la capa D3D de WindowsApps...), no para dar fe de nada.
	bool MicrosoftSigned(const std::string& path, const wchar_t* wide)
	{
		auto cached = microsoft.find(path);
		if (cached != microsoft.end()) return cached->second;
		bool result = false;
		if (!wintrust)
		{
			wintrust = LoadLibraryW(L"wintrust.dll");
			crypt32 = LoadLibraryW(L"crypt32.dll");
			if (wintrust && crypt32)
			{
				verifyTrust = reinterpret_cast<WinVerifyTrustFn>(reinterpret_cast<void*>(GetProcAddress(wintrust, "WinVerifyTrust")));
				provData = reinterpret_cast<ProvDataFromStateDataFn>(reinterpret_cast<void*>(GetProcAddress(wintrust, "WTHelperProvDataFromStateData")));
				provSigner = reinterpret_cast<ProvSignerFromChainFn>(reinterpret_cast<void*>(GetProcAddress(wintrust, "WTHelperGetProvSignerFromChain")));
				provCert = reinterpret_cast<ProvCertFromChainFn>(reinterpret_cast<void*>(GetProcAddress(wintrust, "WTHelperGetProvCertFromChain")));
				certName = reinterpret_cast<CertGetNameStringWFn>(reinterpret_cast<void*>(GetProcAddress(crypt32, "CertGetNameStringW")));
			}
		}
		if (verifyTrust && provData && provSigner && provCert && certName)
		{
			WINTRUST_FILE_INFO file{};
			file.cbStruct = sizeof(file);
			file.pcwszFilePath = wide;
			WINTRUST_DATA data{};
			data.cbStruct = sizeof(data);
			data.dwUIChoice = WTD_UI_NONE;
			data.fdwRevocationChecks = WTD_REVOKE_NONE;
			data.dwUnionChoice = WTD_CHOICE_FILE;
			data.pFile = &file;
			data.dwStateAction = WTD_STATEACTION_VERIFY;
			data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
			GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
			if (verifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data) == 0)
			{
				CRYPT_PROVIDER_DATA* provider = provData(data.hWVTStateData);
				CRYPT_PROVIDER_SGNR* signer = provider ? provSigner(provider, 0, FALSE, 0) : nullptr;
				if (signer && signer->csCertChain > 0)
				{
					auto name = [&](DWORD index) {
						CRYPT_PROVIDER_CERT* cert = provCert(signer, index);
						wchar_t text[256] = {};
						if (cert && cert->pCert) certName(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, text, 256);
						return Narrow(text);
					};
					result = StartsWith(name(0), "Microsoft ") && StartsWith(name(signer->csCertChain - 1), "Microsoft Root");
				}
			}
			data.dwStateAction = WTD_STATEACTION_CLOSE;
			verifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
		}
		microsoft[path] = result;
		return result;
	}

	void Modules(std::vector<Finding>& found)
	{
		++life[GuardProto::LifeModules];
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
				if (Once("module:" + path)) Add(found, GuardProto::Inject, true, "m\xC3\xB3" "dulo de Cheat Engine: " + BaseName(path));
			}
			else if (FindPart(moduleBlock, BaseName(path)))
			{
				if (Once("module:" + path)) Add(found, GuardProto::Inject, true, "m\xC3\xB3" "dulo bloqueado por el server: " + BaseName(path));
			}
			else if (FindPart(moduleAllow, path))
			{
				continue;
			}
			else if (!StartsWith(path, windowsDir) && !StartsWith(path, gameDir) && !reported.count("module:" + path) &&
			         !MicrosoftSigned(path, wide) && Once("module:" + path))
			{
				Add(found, GuardProto::Inject, false, "m\xC3\xB3" "dulo fuera del juego y del sistema: " + path);
			}
		}
	}

	// Herramientas de las listas del server: procesos por nombre del ejecutable,
	// ventanas por parte del título.
	void Tools(std::vector<Finding>& found)
	{
		++life[GuardProto::LifeTools];
		if (!processes.empty())
		{
			HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
			if (snapshot != INVALID_HANDLE_VALUE)
			{
				PROCESSENTRY32W entry{};
				entry.dwSize = sizeof(entry);
				for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
				{
					if (entry.th32ProcessID == pid || entry.th32ProcessID == GetCurrentProcessId()) continue;
					const std::string name = Lower(Narrow(entry.szExeFile));
					if (Contains(processes, name) && Once("tool:" + name))
						Add(found, GuardProto::Tool, toolCloses, "herramienta abierta: " + name);
				}
				CloseHandle(snapshot);
			}
		}
		if (!windows.empty())
		{
			struct Search { State* state; std::vector<Finding>* found; } search{ this, &found };
			EnumWindows([](HWND window, LPARAM param) -> BOOL {
				Search& s = *reinterpret_cast<Search*>(param);
				if (!IsWindowVisible(window)) return TRUE;
				wchar_t title[256];
				if (GetWindowTextW(window, title, 256) <= 0) return TRUE;
				const std::string text = Narrow(title);
				if (const std::string* part = FindPart(s.state->windows, Lower(text)))
				{
					if (s.state->Once("window:" + *part))
						Add(*s.found, GuardProto::Tool, s.state->toolCloses, "ventana abierta: " + text);
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&search));
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
			if (Once("debugger:self")) Add(found, GuardProto::Debugger, true, "depurador sobre el guardi\xC3\xA1n");
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
		++life[GuardProto::LifeDrivers];
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
		++life[GuardProto::LifeModules];
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
				if (Once("module:" + path)) Add(found, GuardProto::Inject, true, "m\xC3\xB3" "dulo de Cheat Engine: " + BaseName(path));
			}
			else if (FindPart(moduleBlock, BaseName(lower)))
			{
				if (Once("module:" + path)) Add(found, GuardProto::Inject, true, "m\xC3\xB3" "dulo bloqueado por el server: " + BaseName(path));
			}
			else if (FindPart(moduleAllow, lower))
			{
				continue;
			}
			else if (!StartsWith(lower, "/usr/") && !StartsWith(lower, "/lib") && !StartsWith(lower, gameDir) && Once("module:" + path))
			{
				Add(found, GuardProto::Inject, false, "m\xC3\xB3" "dulo fuera del juego y del sistema: " + path);
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

	// Herramientas de las listas del server: procesos del mismo usuario por el
	// nombre del ejecutable (o comm, que viene cortado a 15 letras).
	void Tools(std::vector<Finding>& found)
	{
		++life[GuardProto::LifeTools];
		if (processes.empty()) return;
		DIR* proc = opendir("/proc");
		if (!proc) return;
		const std::string self = std::to_string(::getpid()), game = std::to_string(pid);
		while (dirent* entry = readdir(proc))
		{
			if (!std::isdigit(static_cast<unsigned char>(entry->d_name[0])) || self == entry->d_name || game == entry->d_name) continue;
			const std::string base = std::string("/proc/") + entry->d_name;
			char exe[1024];
			const ssize_t n = ::readlink((base + "/exe").c_str(), exe, sizeof(exe) - 1);
			const std::string exeName = n > 0 ? Lower(BaseName(std::string(exe, static_cast<std::size_t>(n)))) : std::string();
			const std::string comm = ReadText(base + "/comm");
			const std::string commName = Lower(comm.substr(0, comm.find('\n')));
			// Un script (GameConqueror, PINCE) corre como python3: se mira también comm.
			for (const std::string& name : { exeName, commName })
			{
				if (!name.empty() && Contains(processes, name) && Once("tool:" + name))
					Add(found, GuardProto::Tool, toolCloses, "herramienta abierta: " + name);
			}
		}
		closedir(proc);
	}
#endif
};

Detector::Detector() : m(new State) {}

Detector::~Detector()
{
	m->FilesReset();
#ifdef _WIN32
	for (const auto& thread : m->threads) CloseHandle(thread.second);
	if (m->wintrust) FreeLibrary(m->wintrust);
	if (m->crypt32) FreeLibrary(m->crypt32);
#endif
	delete m;
}

void Detector::SetThreadDacl(void* dacl)
{
#ifdef _WIN32
	m->threadDacl = static_cast<PACL>(dacl);
#else
	(void)dacl;
#endif
}

void Detector::Life(std::uint32_t out[GuardProto::LifeCounters]) const
{
	std::memcpy(out, m->life, sizeof(m->life));
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

bool Detector::ImageBase(std::uint64_t& base)
{
#ifdef _WIN32
	// El primer módulo del proceso es el ejecutable.
	HMODULE first = nullptr;
	DWORD needed = 0;
	if (!m->enumModules || !m->enumModules(m->process, &first, sizeof(first), &needed, LIST_MODULES_DEFAULT) || needed == 0) return false;
	base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(first));
	return true;
#else
	// La primera región del ejecutable (desplazamiento 0) en /proc/<pid>/maps.
	char exe[4096];
	const ssize_t n = ::readlink(("/proc/" + std::to_string(m->pid) + "/exe").c_str(), exe, sizeof(exe) - 1);
	if (n <= 0) return false;
	const std::string path(exe, static_cast<std::size_t>(n));
	std::ifstream maps("/proc/" + std::to_string(m->pid) + "/maps");
	std::string line;
	while (std::getline(maps, line))
	{
		unsigned long long start = 0, end = 0, offset = 0;
		char perms[8] = {};
		int consumed = 0;
		if (std::sscanf(line.c_str(), "%llx-%llx %7s %llx %*s %*s %n", &start, &end, perms, &offset, &consumed) < 4 || consumed == 0) continue;
		if (offset == 0 && line.compare(static_cast<std::size_t>(consumed), std::string::npos, path) == 0)
		{
			base = start;
			return true;
		}
	}
	return false;
#endif
}

bool Detector::Read(std::uint64_t address, void* data, std::size_t size)
{
#ifdef _WIN32
	SIZE_T read = 0;
	return ReadProcessMemory(m->process, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)), data, size, &read) && read == size;
#else
	const int fd = ::open(("/proc/" + std::to_string(m->pid) + "/mem").c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) return false;
	std::size_t done = 0;
	while (done < size)
	{
		const ssize_t got = ::pread(fd, static_cast<char*>(data) + done, size - done, static_cast<off_t>(address + done));
		if (got <= 0) break;
		done += static_cast<std::size_t>(got);
	}
	::close(fd);
	return done == size;
#endif
}

void Detector::SetMemScanCloses(bool closes)
{
	m->memScanCloses = closes;
}

void Detector::SetToolCloses(bool closes)
{
	m->toolCloses = closes;
}

void Detector::SetFilesCloses(bool closes)
{
	m->filesCloses = closes;
}

void Detector::Rule(std::uint8_t kind, const std::string& text)
{
	switch (kind)
	{
	case GuardProto::RuleReset:
		m->processes.clear();
		m->windows.clear();
		m->moduleBlock.clear();
		m->moduleAllow.clear();
		m->names.clear();
		m->files.clear();
		m->FilesReset();
		break;
	case GuardProto::RuleProcess: m->processes.push_back(Lower(text)); break;
	case GuardProto::RuleWindow: m->windows.push_back(Lower(text)); break;
	case GuardProto::RuleModuleBlock: m->moduleBlock.push_back(Lower(text)); break;
	case GuardProto::RuleModuleAllow: m->moduleAllow.push_back(Lower(text)); break;
	case GuardProto::RuleFile:
	{
		// hash en hex TAB ruta
		State::FileEntry entry;
		if (text.size() < 2 * GuardProto::HashSize + 2 || text[2 * GuardProto::HashSize] != '\t') break;
		bool ok = true;
		for (std::size_t i = 0; i < GuardProto::HashSize && ok; ++i)
		{
			unsigned value = 0;
			ok = std::sscanf(text.c_str() + 2 * i, "%2x", &value) == 1;
			entry.hash[i] = static_cast<std::uint8_t>(value);
		}
		entry.path = text.substr(2 * GuardProto::HashSize + 1);
		// Sólo rutas dentro de la carpeta del juego.
		if (ok && !entry.path.empty() && entry.path[0] != '/' && entry.path[0] != '\\' &&
		    entry.path.find("..") == std::string::npos && entry.path.find(':') == std::string::npos)
		{
			m->files.push_back(entry);
			m->FilesReset();
		}
		break;
	}
	case GuardProto::RuleName:
	{
		// idioma TAB DETECCION TAB nombre
		const std::size_t first = text.find('\t');
		const std::size_t second = first == std::string::npos ? first : text.find('\t', first + 1);
		if (second != std::string::npos) m->names[text.substr(0, second)] = text.substr(second + 1);
		break;
	}
	default: break;
	}
}

std::string Detector::Name(std::uint8_t code, int language) const
{
	static const char* const kLanguages[] = { "es", "en", "pt" };
	if (code >= sizeof(kDetectionNames) / sizeof(kDetectionNames[0])) return std::string();
	const int lang = (language >= 0 && language < 3) ? language : 0;
	auto it = m->names.find(std::string(kLanguages[lang]) + "\t" + kDetectionNames[code]);
	return it == m->names.end() ? std::string() : it->second;
}

void Detector::Scan(std::vector<Finding>& found)
{
	const bool slow = (m->passes++ % kSlowEvery) == 0;
	++m->life[GuardProto::LifePasses];
#ifdef _WIN32
	m->Threads();
#endif
	m->Debugger(found);
	m->Files(found);
#ifdef _WIN32
	m->Decoys(found);
	m->TimeHooks(found);
	if (slow)
	{
		m->Drivers(found);
		m->Modules(found);
		m->Tools(found);
	}
#else
	if (slow)
	{
		m->MemFiles(found);
		m->Modules(found);
		m->Tools(found);
	}
#endif
}

} // namespace Guard
