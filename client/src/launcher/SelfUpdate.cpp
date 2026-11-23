#include "SelfUpdate.h"

#include "update/FileUtil.h"

namespace Launcher {

using namespace Update;

SelfPaths SelfPathsFor(const std::string& exe, const std::string& cacheDir)
{
	SelfPaths p;
	p.exe = exe;
	p.old = exe + ".old";
	p.bad = exe + ".bad";
	p.pending = JoinPath(cacheDir, "launcher-update.txt");
	p.blocked = JoinPath(cacheDir, "launcher-blocked.txt");
	return p;
}

bool MarkPending(const SelfPaths& p)
{
	static const char kText[] = "intentos=0\n";
	return WriteFileAtomic(p.pending, kText, sizeof(kText) - 1);
}

StartupCheck CheckAtStartup(const SelfPaths& p)
{
	std::string text;
	if (!FileExists(p.pending) || !ReadWholeFile(p.pending, text)) return StartupCheck::Normal;
	if (text.find("intentos=0") != std::string::npos)
	{
		static const char kTaken[] = "intentos=1\n";
		WriteFileAtomic(p.pending, kTaken, sizeof(kTaken) - 1);
		return StartupCheck::Confirming;
	}
	// La marca ya estaba tomada: el arranque anterior no llegó a confirmarse.
	if (!FileExists(p.old))
	{
		RemoveFile(p.pending);
		return StartupCheck::RollbackFailed;
	}
	std::string err;
	Digest bad{};
	std::uint64_t size = 0;
	const bool hashed = HashFile(p.exe, bad, &size);
	RemoveFile(p.bad);
	if (!MoveFileOver(p.exe, p.bad, &err) || !MoveFileOver(p.old, p.exe, &err))
		return StartupCheck::RollbackFailed;
	if (hashed)
	{
		const std::string line = FormatDigest(bad) + "\n";
		WriteFileAtomic(p.blocked, line.data(), line.size());
	}
	RemoveFile(p.pending);
	return StartupCheck::RolledBack;
}

bool BlockedVersion(const SelfPaths& p, Digest& out)
{
	std::string text;
	if (!FileExists(p.blocked) || !ReadWholeFile(p.blocked, text)) return false;
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
	return ParseDigest(text, out);
}

void ConfirmHealthy(const SelfPaths& p)
{
	RemoveFile(p.pending);
	RemoveFile(p.old);
	RemoveFile(p.bad);
}

} // namespace Launcher
