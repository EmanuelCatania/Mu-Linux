// Núcleo del actualizador (src/update) y mu-patch (tools/mu-patch): JSON, zips,
// deltas, firma, validación del manifiesto y un canal armado y verificado de
// punta a punta, con una segunda versión que reusa zips y lleva deltas; y la
// instalación: índice local, plan por capas, descargas con cortes, deltas y
// borrados.

#include "PatchBuilder.h"
#include "Publish.h"

#include "update/Announce.h"
#include "update/ChannelFetch.h"
#include "update/Delta.h"
#include "update/FileUtil.h"
#include "update/Hash.h"
#include "update/InstallIndex.h"
#include "update/InstallPlan.h"
#include "update/Installer.h"
#include "update/Json.h"
#include "update/Manifest.h"
#include "update/Signing.h"
#include "update/Transfer.h"
#include "update/Zip.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Update;

namespace {

int s_Failures = 0;

void Require(bool ok, const char* what)
{
	if (!ok)
	{
		std::printf("FALLA: %s\n", what);
		++s_Failures;
	}
}

std::vector<std::uint8_t> Random(std::size_t size, unsigned seed)
{
	std::mt19937 rng(seed);
	std::vector<std::uint8_t> v(size);
	for (auto& b : v) b = static_cast<std::uint8_t>(rng());
	return v;
}

// Datos que se comprimen (texto repetido con algo de variación).
std::vector<std::uint8_t> Compressible(std::size_t size, unsigned seed)
{
	std::mt19937 rng(seed);
	std::vector<std::uint8_t> v(size);
	for (std::size_t i = 0; i < size; ++i) v[i] = static_cast<std::uint8_t>("abcdefgh"[(i / 7 + (rng() % 50 == 0)) % 8]);
	return v;
}

void Write(const std::string& path, const std::vector<std::uint8_t>& data)
{
	Require(WriteFileAtomic(path, data.data(), data.size()), "escribir archivo de prueba");
}

void Write(const std::string& path, const std::string& text)
{
	Require(WriteFileAtomic(path, text.data(), text.size()), "escribir archivo de prueba");
}

std::vector<std::uint8_t> FakeExe(const char* release)
{
	std::vector<std::uint8_t> d(0x200, 0);
	d[0] = 'M';
	d[1] = 'Z';
	d[0x3C] = 0x40;
	std::memcpy(&d[0x40], "PE\0\0", 4);
	d[0x44] = 0x64;
	d[0x45] = 0x86;
	if (release)
	{
		const std::string marker = std::string("MuRelease1:") + release;
		d.insert(d.end(), marker.begin(), marker.end());
		d.push_back(0);
	}
	return d;
}

void TestJson()
{
	JsonValue v;
	std::string err;
	Require(ParseJson("{\"a\": [1, -2, true, null, \"x\\u00f1\\ud83d\\ude00\"], \"b\": {}}", v, &err), "JSON válido");
	Require(v.Find("a") && v.Find("a")->Items().size() == 5, "JSON arreglo");
	Require(v.Find("a")->Items()[1].AsInt() == -2, "JSON negativo");
	Require(v.Find("a")->Items()[4].AsString() == "x\xc3\xb1\xf0\x9f\x98\x80", "JSON \\u a UTF-8");
	JsonValue again;
	Require(ParseJson(v.Dump(), again, &err) && again.Dump() == v.Dump(), "JSON ida y vuelta");
	Require(!ParseJson("{\"a\":1,\"a\":2}", v, &err), "JSON rechaza clave repetida");
	Require(!ParseJson("[1.5]", v, &err), "JSON rechaza decimales");
	Require(!ParseJson("[1] x", v, &err), "JSON rechaza texto sobrante");
	Require(!ParseJson("[\"\\x\"]", v, &err), "JSON rechaza escape no válido");
	Require(!ParseJson("[\"\\ud83d\"]", v, &err), "JSON rechaza surrogate suelto");
	Require(!ParseJson("[9223372036854775808]", v, &err), "JSON rechaza número fuera de rango");
	std::string deep(100, '[');
	deep += std::string(100, ']');
	Require(!ParseJson(deep, v, &err), "JSON rechaza demasiado anidado");
}

void TestPaths()
{
	Require(IsSafeRelativePath("Content/item/Sword01.ozj"), "ruta normal");
	Require(IsSafeRelativePath("Content/\xc3\xb1" "and\xc3\xba.txt"), "ruta UTF-8");
	Require(!IsSafeRelativePath("../main.exe"), "rechaza ..");
	Require(!IsSafeRelativePath("/etc/passwd"), "rechaza absoluta");
	Require(!IsSafeRelativePath("C:/x"), "rechaza unidad");
	Require(!IsSafeRelativePath("a\\b"), "rechaza barra invertida");
	Require(!IsSafeRelativePath("a//b"), "rechaza segmento vacío");
	Require(!IsSafeRelativePath("a/./b"), "rechaza .");
	Require(!IsSafeRelativePath("Content/con.txt"), "rechaza nombre reservado de Windows");
	Require(!IsSafeRelativePath("Content/x."), "rechaza punto final");

	Require(MuPatch::GlobMatch("Content/**", "Content/a/b.ozj"), "glob **");
	Require(MuPatch::GlobMatch("**/*-hd.*", "Content/item/Sword-hd.ozj"), "glob **/*-hd.*");
	Require(MuPatch::GlobMatch("**/*-hd.*", "Sword-hd.ozj"), "glob **/ sin carpetas");
	Require(!MuPatch::GlobMatch("Content/*", "Content/a/b.ozj"), "glob * no cruza carpetas");
	Require(MuPatch::GlobMatch("Content/*.ozj", "Content/b.ozj"), "glob *.ozj");
	Require(!MuPatch::GlobMatch("Content/?.ozj", "Content/ab.ozj"), "glob ?");

	Require(IsValidVersion("0.97.50") && !IsValidVersion("0.97.") && !IsValidVersion("v1.2") && !IsValidVersion("1"), "versiones");
	Require(CompareVersions("0.97.9", "0.97.10") < 0 && CompareVersions("0.97.50", "0.97.50") == 0, "comparar versiones");
	Require(IsValidChannel("mu-linux/base") && !IsValidChannel("mu-linux") && !IsValidChannel("Mu/Base") && !IsValidChannel("a/b/c"), "canales");
}

void TestZipAndDelta(const std::string& dir)
{
	const std::string path = JoinPath(dir, "t.zip");
	const auto raw = Compressible(300000, 1);
	std::vector<std::uint8_t> packed;
	std::string err;
	Require(ZstdCompress(raw.data(), raw.size(), 3, packed, &err) && packed.size() < raw.size() / 4, "zstd comprime");

	ZipWriter w;
	ZipEntry a, b, c;
	Require(w.Open(path), "zip abrir");
	Require(w.Add("Content/a.bin", ZipMethod::Zstd, packed.data(), packed.size(), raw.size(), Crc32(raw.data(), raw.size()), &a), "zip agregar zstd");
	const std::string text = "hola";
	Require(w.Add("Content/b.txt", ZipMethod::Store, reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), text.size(),
	              Crc32(text.data(), text.size()), &b), "zip agregar guardado");
	Require(w.Add("vacío.txt", ZipMethod::Store, nullptr, 0, 0, 0, &c), "zip agregar vacío");
	Require(w.Finish(), "zip cerrar");

	ZipReader r;
	Require(r.Open(path, &err), "zip leer");
	Require(r.Entries().size() == 3, "zip 3 entradas");
	const ZipEntry* ra = r.Find("Content/a.bin");
	Require(ra && ra->dataOffset == a.dataOffset && ra->method == ZipMethod::Zstd, "zip offset de datos");
	std::vector<std::uint8_t> out;
	Require(ra && ZipExtract(path, *ra, out, &err) && out == raw, "zip extraer zstd");
	const ZipEntry* rb = r.Find("Content/b.txt");
	Require(rb && ZipExtract(path, *rb, out, &err) && std::string(out.begin(), out.end()) == text, "zip extraer guardado");
	// Leer sólo el tramo (lo que hace el launcher con un Range).
	std::vector<std::uint8_t> range;
	Require(ReadFileRange(path, a.dataOffset, a.packedSize, range) &&
	        ZipDecode(ZipMethod::Zstd, range.data(), range.size(), raw.size(), out, &err) && out == raw, "zip tramo suelto");
	Require(!ZipDecode(ZipMethod::Zstd, range.data(), range.size(), raw.size() + 1, out, &err), "zstd rechaza tamaño equivocado");

	// Zip64: más de 65535 entradas.
	const std::string many = JoinPath(dir, "many.zip");
	ZipWriter w2;
	Require(w2.Open(many), "zip64 abrir");
	for (int i = 0; i < 70000; ++i)
	{
		const std::string name = "f" + std::to_string(i);
		w2.Add(name, ZipMethod::Store, reinterpret_cast<const std::uint8_t*>(name.data()), name.size(), name.size(),
		       Crc32(name.data(), name.size()), nullptr);
	}
	Require(w2.Finish(), "zip64 cerrar");
	ZipReader r2;
	Require(r2.Open(many, &err) && r2.Entries().size() == 70000, "zip64 leer 70000 entradas");
	const ZipEntry* last = r2.Find("f69999");
	Require(last && ZipExtract(many, *last, out, &err) && std::string(out.begin(), out.end()) == "f69999", "zip64 extraer");

	// Delta.
	auto v1 = Random(2 << 20, 7);
	auto v2 = v1;
	for (std::size_t i = 100000; i < 100500; ++i) v2[i] ^= 0x5A;
	v2.insert(v2.begin() + 500000, 4096, 0x11);
	std::vector<std::uint8_t> patch, rebuilt;
	Require(DeltaCreate(v1, v2, 19, patch, &err), "delta crear");
	Require(patch.size() < v2.size() / 20, "delta chico");
	Require(DeltaApply(v1, patch.data(), patch.size(), v2.size(), rebuilt, &err) && rebuilt == v2, "delta aplicar");
	auto wrongBase = v1;
	wrongBase[10] ^= 1;
	Require(!DeltaApply(wrongBase, patch.data(), patch.size(), v2.size(), rebuilt, &err) || rebuilt != v2, "delta sobre otra base no pasa por bueno");
}

void TestSigning(const std::string& dir)
{
	KeyPair k;
	Require(GenerateKey(k), "generar clave");
	const std::string keyPath = JoinPath(dir, "k.key");
	Require(SaveKeyFile(keyPath, k), "guardar clave");
	KeyPair loaded;
	std::string err;
	Require(LoadKeyFile(keyPath, loaded, &err) && std::memcmp(loaded.publicKey, k.publicKey, kKeySize) == 0, "leer clave");

	// El formato de Data/Keys/ServerIdentity.key también sirve.
	Write(JoinPath(dir, "ServerIdentity.key"), "# comentario\n# otra\nseed=" + ToHex(k.seed, kKeySize) + "\nprevious=00\n");
	Require(LoadKeyFile(JoinPath(dir, "ServerIdentity.key"), loaded, &err) &&
	        std::memcmp(loaded.publicKey, k.publicKey, kKeySize) == 0, "leer ServerIdentity.key");

	const std::string msg = "{\"x\":1}\n";
	std::uint8_t sig[kSignatureSize];
	Sign(kPatchContext, msg.data(), msg.size(), k, sig);
	Require(Verify(kPatchContext, msg.data(), msg.size(), sig, k.publicKey), "firma válida");
	Require(!Verify(kLauncherContext, msg.data(), msg.size(), sig, k.publicKey), "otro contexto no vale");
	std::string tampered = msg;
	tampered[2] = 'y';
	Require(!Verify(kPatchContext, tampered.data(), tampered.size(), sig, k.publicKey), "mensaje cambiado no vale");
	std::uint8_t parsed[kSignatureSize];
	Require(ParseSignatureFile(FormatSignatureFile(sig), parsed) && std::memcmp(parsed, sig, sizeof(sig)) == 0, "archivo .sig");
}

std::string MinimalManifest(const std::string& filesJson, const std::string& extra = "")
{
	return "{\"format\":1,\"channel\":\"mu-linux/base\",\"version\":\"0.97.50\",\"sequence\":1,"
	       "\"bundles\":[{\"name\":\"b-1.zip\",\"size\":1000,\"hash\":\"blake2b-256:" + std::string(64, '0') + "\"}],"
	       "\"files\":[" + filesJson + "]" + extra + "}";
}

std::string FileJson(const std::string& path, int offset = 0, int packed = 10, const std::string& platform = "")
{
	return "{\"path\":\"" + path + "\",\"size\":10,\"hash\":\"blake2b-256:" + std::string(64, '1') +
	       "\",\"bundle\":0,\"offset\":" + std::to_string(offset) + ",\"packed\":" + std::to_string(packed) +
	       ",\"method\":\"store\"" + (platform.empty() ? "" : ",\"platform\":\"" + platform + "\"") + "}";
}

void TestManifestValidation()
{
	Manifest m;
	std::string err;
	bool unknown = false;
	Require(ParseManifest(MinimalManifest(FileJson("Content/a.ozj")), m, &err), "manifiesto mínimo");
	Require(ParseManifest(DumpManifest(m), m, &err), "manifiesto ida y vuelta");
	Require(!ParseManifest(MinimalManifest(FileJson("../a")), m, &err), "rechaza ruta con ..");
	Require(!ParseManifest(MinimalManifest(FileJson("User/Config.ini")), m, &err), "rechaza User/");
	Require(!ParseManifest(MinimalManifest(FileJson("a", 995)), m, &err), "rechaza tramo fuera del zip");
	Require(!ParseManifest(MinimalManifest(FileJson("a", 0, 9)), m, &err), "rechaza store con tamaños distintos");
	Require(!ParseManifest(MinimalManifest(FileJson("Content/A") + "," + FileJson("content/a")), m, &err), "rechaza ruta repetida");
	Require(ParseManifest(MinimalManifest(FileJson("main", 0, 10, "linux-x64") + "," + FileJson("main", 0, 10, "windows-x64")), m, &err),
	        "misma ruta en dos plataformas");
	Require(!ParseManifest(MinimalManifest(FileJson("main", 0, 10, "linux-x64") + "," + FileJson("main")), m, &err),
	        "rechaza ruta común y de plataforma a la vez");
	Require(!ParseManifest(MinimalManifest(FileJson("a"), ",\"deleted\":[\"a\"]"), m, &err), "rechaza borrar e instalar");
	std::string future = MinimalManifest(FileJson("a"));
	future.replace(future.find("\"format\":1"), 10, "\"format\":2");
	Require(!ParseManifest(future, m, &err, &unknown) && unknown, "formato nuevo se reconoce como desconocido");
}

bool Build(MuPatch::BuildOptions o, std::string* err)
{
	o.threads = 2;
	o.level = 3;
	return MuPatch::BuildPatch(o, err);
}

void TestEndToEnd(const std::string& dir)
{
	std::string err;
	KeyPair k;
	Require(GenerateKey(k), "clave e2e");
	const std::string key = JoinPath(dir, "release.key");
	Require(SaveKeyFile(key, k), "guardar clave e2e");

	// Versión 1 de la base.
	const std::string in1 = JoinPath(dir, "in1");
	const auto big1 = Random(3 << 20, 11);
	Write(JoinPath(in1, "Content/item/Sword01.ozj"), Random(50000, 1));
	Write(JoinPath(in1, "Content/data/big.bin"), big1);
	Write(JoinPath(in1, "Content/text/\xc3\xb1" "and\xc3\xba.txt"), std::string("texto"));
	Write(JoinPath(in1, "Content/empty.txt"), std::string());
	Write(JoinPath(in1, "Content/music/main.ogg"), Compressible(200000, 3));
	Write(JoinPath(in1, "System/x.cfg"), std::string("a=1\n"));
	Write(JoinPath(in1, "main.exe.prev"), std::string("no entra"));

	MuPatch::BuildOptions o;
	o.channel = "mu-linux/base";
	o.version = "0.97.50";
	o.in = in1;
	o.out = JoinPath(dir, "out1");
	o.keyPath = key;
	o.include = { "Content/**", "System/**" };
	o.optional = { "Content/music/**" };
	o.bundleSize = 1 << 20;
	Require(Build(o, &err), ("build v1: " + err).c_str());

	Manifest m1;
	std::string text;
	Require(ReadWholeFile(JoinPath(o.out, "manifest.json"), text) && ParseManifest(text, m1, &err), "leer manifiesto v1");
	Require(m1.files.size() == 6 && m1.sequence == 1, "v1: 6 archivos por lista blanca, secuencia 1");
	Require(m1.bundles.size() == 2, "v1: el archivo de 3 MB va solo en su zip y el resto en otro");
	for (const auto& f : m1.files)
		if (f.path == "Content/music/main.ogg") Require(!f.required, "música opcional");

	MuPatch::VerifyOptions v;
	std::memcpy(v.publicKey, k.publicKey, kKeySize);
	v.in = o.out;
	Require(MuPatch::VerifyPatch(v, &err), ("verify v1: " + err).c_str());

	// Otra clave no verifica.
	KeyPair other;
	GenerateKey(other);
	MuPatch::VerifyOptions vOther = v;
	std::memcpy(vOther.publicKey, other.publicKey, kKeySize);
	Require(!MuPatch::VerifyPatch(vOther, &err), "verify con otra clave falla");

	// Un byte del manifiesto cambiado invalida la firma.
	const std::string tamperedDir = JoinPath(dir, "tampered");
	for (const char* name : { "manifest.json", "manifest.json.sig" })
		CopyFileTo(JoinPath(o.out, name), JoinPath(tamperedDir, name));
	std::string t;
	ReadWholeFile(JoinPath(tamperedDir, "manifest.json"), t);
	const std::size_t seqAt = t.find("\"sequence\": 1");
	Require(seqAt != std::string::npos, "manifiesto con secuencia 1");
	if (seqAt != std::string::npos) t.replace(seqAt, 13, "\"sequence\": 9");
	Write(JoinPath(tamperedDir, "manifest.json"), t);
	MuPatch::VerifyOptions vt = v;
	vt.in = tamperedDir;
	Require(!MuPatch::VerifyPatch(vt, &err), "manifiesto alterado no verifica");

	// Versión 2: cambia el archivo grande (delta), se agrega uno, el resto se reusa.
	const std::string in2 = JoinPath(dir, "in2");
	auto big2 = big1;
	for (std::size_t i = 200000; i < 201000; ++i) big2[i] ^= 0x33;
	Write(JoinPath(in2, "Content/item/Sword01.ozj"), Random(50000, 1));
	Write(JoinPath(in2, "Content/data/big.bin"), big2);
	Write(JoinPath(in2, "Content/text/\xc3\xb1" "and\xc3\xba.txt"), std::string("texto"));
	Write(JoinPath(in2, "Content/empty.txt"), std::string());
	Write(JoinPath(in2, "Content/music/main.ogg"), Compressible(200000, 3));
	Write(JoinPath(in2, "System/x.cfg"), std::string("a=2\n"));
	Write(JoinPath(in2, "Content/new.txt"), std::string("nuevo"));

	MuPatch::BuildOptions o2 = o;
	o2.version = "0.97.51";
	o2.in = in2;
	o2.out = JoinPath(dir, "out2");
	o2.prev = { { o.out, "" } };
	Require(Build(o2, &err), ("build v2: " + err).c_str());

	Manifest m2;
	Require(ReadWholeFile(JoinPath(o2.out, "manifest.json"), text) && ParseManifest(text, m2, &err), "leer manifiesto v2");
	Require(m2.sequence == 2, "v2: secuencia 2");
	std::size_t reused = 0, deltas = 0;
	for (const auto& f : m2.files)
	{
		if (m2.bundles[f.bundle].name.find("0.97.50") != std::string::npos) ++reused;
		deltas += f.deltas.size();
		if (f.path == "Content/data/big.bin") Require(f.deltas.size() == 1 && f.deltas[0].from == HashBytes(big1.data(), big1.size()), "v2: delta del archivo grande");
	}
	Require(reused == 4, "v2: los 4 archivos que no cambiaron vienen de los zips de v1");
	Require(deltas == 1, "v2: un solo delta");

	MuPatch::VerifyOptions v2 = v;
	v2.in = o2.out;
	v2.prev = { { o.out, "" } };
	Require(MuPatch::VerifyPatch(v2, &err), ("verify v2 con delta: " + err).c_str());

	// Secuencia que no crece y versión repetida: rechazadas.
	MuPatch::BuildOptions o3 = o2;
	o3.out = JoinPath(dir, "out3");
	o3.version = "0.97.52";
	o3.sequence = 1;
	Require(!Build(o3, &err), "rechaza secuencia que no crece");
	o3.sequence = 0;
	o3.version = "0.97.50";
	Require(!Build(o3, &err), "rechaza versión ya publicada");

	// La base no puede traer ejecutables.
	const std::string inExe = JoinPath(dir, "inexe");
	Write(JoinPath(inExe, "Content/a.txt"), std::string("a"));
	Write(JoinPath(inExe, "main.exe"), FakeExe("0.97.50"));
	MuPatch::BuildOptions ob = o;
	ob.in = inExe;
	ob.include.clear();
	ob.out = JoinPath(dir, "outexe");
	Require(!Build(ob, &err), "la base rechaza ejecutables");

	// Overlay: toma la base del ejecutable.
	MuPatch::BuildOptions ov;
	ov.channel = "miserver/overlay";
	ov.version = "1.0.0";
	ov.in = inExe;
	ov.out = JoinPath(dir, "outov");
	ov.keyPath = key;
	Require(Build(ov, &err), ("build overlay: " + err).c_str());
	Manifest mo;
	Require(ReadWholeFile(JoinPath(ov.out, "manifest.json"), text) && ParseManifest(text, mo, &err), "leer overlay");
	Require(mo.requiresBase == "0.97.50", "overlay: requires.base sale de MU_RELEASE");
	bool exeTagged = false;
	for (const auto& f : mo.files)
		if (f.path == "main.exe") exeTagged = f.platform == "windows-x64";
	Require(exeTagged && mo.platforms.size() == 1, "overlay: el exe lleva su plataforma");

	// Overlay sin ejecutable, con .toml o sin MU_RELEASE ni --base: rechazado.
	const std::string inNoExe = JoinPath(dir, "innoexe");
	Write(JoinPath(inNoExe, "Content/a.txt"), std::string("a"));
	MuPatch::BuildOptions on = ov;
	on.in = inNoExe;
	on.out = JoinPath(dir, "outnoexe");
	Require(!Build(on, &err), "overlay sin ejecutable");
	const std::string inToml = JoinPath(dir, "intoml");
	Write(JoinPath(inToml, "main.exe"), FakeExe("0.97.50"));
	Write(JoinPath(inToml, "Content/item/Item.toml"), std::string("x=1"));
	on.in = inToml;
	on.out = JoinPath(dir, "outtoml");
	Require(!Build(on, &err), "overlay con .toml");
	const std::string inOld = JoinPath(dir, "inold");
	Write(JoinPath(inOld, "main.exe"), FakeExe(nullptr));
	on.in = inOld;
	on.out = JoinPath(dir, "outold");
	Require(!Build(on, &err), "overlay con exe sin MU_RELEASE y sin --base");
	on.base = "0.97.50";
	on.out = JoinPath(dir, "outold2");
	Require(Build(on, &err), "overlay con exe viejo y --base");

	// Carpeta de salida con archivos: no se mezcla.
	MuPatch::BuildOptions again = o;
	Require(!Build(again, &err), "rechaza salida no vacía");
	k.Wipe();
}

// ── Instalación (índice, plan por capas, descargas) ───────────────────────

// Transporte de archivos que anota cada pedido y puede fallar a propósito.
class TestTransport : public FileTransport
{
public:
	struct Call
	{
		std::string url;
		std::uint64_t offset;
		std::uint64_t length;
	};

	// Los próximos cutCalls pedidos se cortan después de cutAfter bytes.
	int cutCalls = 0;
	std::uint64_t cutAfter = 0;
	// Los próximos corruptCalls pedidos llegan con el primer byte cambiado.
	int corruptCalls = 0;

	bool Fetch(const std::string& url, std::uint64_t offset, std::uint64_t length, const DataSink& sink,
	           std::string* error) override
	{
		bool cut = false, corrupt = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			calls.push_back(Call{ url, offset, length });
			if (cutCalls > 0) { --cutCalls; cut = true; }
			if (corruptCalls > 0) { --corruptCalls; corrupt = true; }
		}
		std::uint64_t sent = 0;
		bool first = true;
		const bool ok = FileTransport::Fetch(url, offset, length, [&](const std::uint8_t* data, std::size_t size) {
			if (cut && sent + size > cutAfter)
			{
				const std::size_t keep = static_cast<std::size_t>(cutAfter - sent);
				if (keep) sink(data, keep);
				sent += keep;
				return false;
			}
			sent += size;
			if (corrupt && first)
			{
				std::vector<std::uint8_t> copy(data, data + size);
				copy[0] ^= 0xFF;
				first = false;
				return sink(copy.data(), copy.size());
			}
			first = false;
			return sink(data, size);
		}, error);
		if (cut && error) *error = "corte de prueba";
		return ok && !cut;
	}

	std::vector<Call> calls;

private:
	std::mutex m_Mutex;
};

std::string FileUrl(const std::string& path)
{
	return "file://" + std::string(path.empty() || path[0] == '/' ? "" : "/") + path;
}

bool LoadManifest(const std::string& dir, Manifest& m)
{
	std::string text, err;
	return ReadWholeFile(JoinPath(dir, "manifest.json"), text) && ParseManifest(text, m, &err);
}

bool SameFile(const std::string& path, const std::vector<std::uint8_t>& data)
{
	std::vector<std::uint8_t> got;
	return ReadWholeFile(path, got) && got == data;
}

const PlanFile* FindPlanned(const Plan& p, const std::string& path)
{
	for (const PlanFile& f : p.files)
		if (f.entry->path == path) return &f;
	return nullptr;
}

void TestHdVariantAndIndex(const std::string& dir)
{
	Require(HdVariant("Content/a/casa.ozj") == "Content/a/casa-hd.ozj", "HdVariant");
	Require(HdVariant("Content/a/casa-hd.ozj").empty(), "HdVariant de un -hd");
	Require(HdVariant("Content/a.b/sinext").empty() && HdVariant("Content/.oculto").empty(), "HdVariant sin extensión");
	Require(FileUrlToPath("file:///C:/mu/a.zip") == "C:/mu/a.zip" && FileUrlToPath("file:///tmp/a.zip") == "/tmp/a.zip" &&
	            FileUrlToPath("C:/a.zip").empty(), "FileUrlToPath");

	InstallIndex idx;
	std::string err;
	Require(LoadIndex(JoinPath(dir, "no-existe.json"), idx, &err) && idx.files.empty(), "índice que falta: vacío");
	idx.revision = 7;
	IndexEntry e;
	e.size = 5;
	e.mtime = -3;
	e.hash = HashBytes("hola", 4);
	e.channel = "mu-linux/base";
	idx.files["Content/\xc3\xb1.txt"] = e;
	const std::string path = JoinPath(dir, "index/install.json");
	Require(SaveIndex(path, idx), "guardar índice");
	InstallIndex back;
	Require(LoadIndex(path, back, &err) && back.revision == 7 && back.files.size() == 1 &&
	            back.files.begin()->second.hash == e.hash && back.files.begin()->second.mtime == -3, "releer índice");
	Write(path, std::string("{\"format\": 1, \"revision\": 1, \"savedAt\": 0, \"files\": [{\"path\": \"../x\"}]}"));
	Require(!LoadIndex(path, back, &err) && back.files.empty(), "índice roto: se rechaza");

	// Tamaño y fecha iguales a lo guardado: se confía en el índice sin rehashear
	// (salvo "Verificar archivos").  Una fecha cerca del guardado no alcanza.
	namespace fs = std::filesystem;
	const std::string root = JoinPath(dir, "index-root");
	const std::string file = JoinPath(root, "a.bin");
	const auto v1 = Random(1000, 31), v2 = Random(1000, 32);
	Write(file, v1);
	const Digest h1 = HashBytes(v1.data(), v1.size());
	InstallIndex ix;
	Require(CheckLocal(root, "a.bin", h1, ix, false, nullptr) == LocalState::Match, "CheckLocal: hashea y coincide");
	const fs::path fp = fs::u8path(file);
	const auto when = fs::last_write_time(fp);
	Write(file, v2);
	fs::last_write_time(fp, when);
	ix.savedAt = 0;
	Require(CheckLocal(root, "a.bin", h1, ix, false, nullptr) == LocalState::Differs, "índice sin guardar: rehashea");
	Write(file, v1);
	fs::last_write_time(fp, when);
	Require(CheckLocal(root, "a.bin", h1, ix, false, nullptr) == LocalState::Match, "vuelve a v1");
	Write(file, v2);
	fs::last_write_time(fp, when);
	ix.savedAt = when.time_since_epoch().count() + static_cast<std::int64_t>(
		std::chrono::duration_cast<fs::file_time_type::duration>(std::chrono::seconds(60)).count());
	Require(CheckLocal(root, "a.bin", h1, ix, false, nullptr) == LocalState::Match, "índice confiable: no rehashea");
	Require(CheckLocal(root, "a.bin", h1, ix, true, nullptr) == LocalState::Differs, "verificar archivos: rehashea igual");
	Require(CheckLocal(root, "falta.bin", h1, ix, false, nullptr) == LocalState::Missing, "CheckLocal: falta");
}

void TestRateLimiter()
{
	using namespace std::chrono;
	RateLimiter lim(1 << 20);   // 1 MB/s, ráfaga de 256 KB
	const auto t0 = steady_clock::now();
	for (int i = 0; i < 12; ++i) lim.Acquire(64 << 10, nullptr);   // 768 KB
	const double s = duration<double>(steady_clock::now() - t0).count();
	Require(s >= 0.4, "el limitador frena (768 KB a 1 MB/s tardan más de 0,4 s)");

	std::atomic<bool> cancel{ false };
	lim.SetPaused(true);
	std::thread t([&] {
		std::this_thread::sleep_for(milliseconds(150));
		cancel = true;
		lim.Wake();
	});
	Require(!lim.Acquire(1, &cancel), "pausado: la cancelación lo destraba");
	t.join();
	lim.SetPaused(false);
	lim.SetRate(0);
	Require(lim.Acquire(100 << 20, nullptr), "sin límite pasa todo");
}

void TestInstall(const std::string& dir)
{
	std::string err;
	KeyPair k;
	Require(GenerateKey(k), "clave install");
	const std::string key = JoinPath(dir, "inst.key");
	Require(SaveKeyFile(key, k), "guardar clave install");

	// Base v1, HD y overlay.
	const std::string bin = JoinPath(dir, "inst-base1");
	const auto casa = Random(200000, 21), otra = Compressible(300000, 22), big1 = Random(3 << 20, 23);
	const auto music = Compressible(200000, 24);
	Write(JoinPath(bin, "Content/a/casa.ozj"), casa);
	Write(JoinPath(bin, "Content/a/otra.ozj"), otra);
	Write(JoinPath(bin, "Content/data/big.bin"), big1);
	Write(JoinPath(bin, "Content/item/Item.toml"), std::string("x=1\n"));
	Write(JoinPath(bin, "Content/music/main.ogg"), music);
	Write(JoinPath(bin, "System/x.cfg"), std::string("a=1\n"));
	MuPatch::BuildOptions ob;
	ob.channel = "mu-linux/base";
	ob.version = "0.97.50";
	ob.in = bin;
	ob.out = JoinPath(dir, "inst-out-base1");
	ob.keyPath = key;
	ob.optional = { "Content/music/**" };
	ob.bundleSize = 1 << 20;
	Require(Build(ob, &err), ("install: base v1: " + err).c_str());

	const std::string hin = JoinPath(dir, "inst-hd");
	const auto casaHd = Random(100000, 25), otraHd = Random(100000, 26);
	Write(JoinPath(hin, "Content/a/casa-hd.ozj"), casaHd);
	Write(JoinPath(hin, "Content/a/otra-hd.ozj"), otraHd);
	MuPatch::BuildOptions oh = ob;
	oh.channel = "mu-linux/hd";
	oh.in = hin;
	oh.out = JoinPath(dir, "inst-out-hd");
	oh.optional = { "**" };
	Require(Build(oh, &err), ("install: hd: " + err).c_str());

	const std::string oin = JoinPath(dir, "inst-ov");
	const auto casaAdmin = Random(150000, 27), exe = FakeExe("0.97.50");
	Write(JoinPath(oin, "Content/a/casa.ozj"), casaAdmin);
	Write(JoinPath(oin, "main.exe"), exe);
	MuPatch::BuildOptions oo;
	oo.channel = "prueba/overlay";
	oo.version = "1.0.0";
	oo.in = oin;
	oo.out = JoinPath(dir, "inst-out-ov");
	oo.keyPath = key;
	Require(Build(oo, &err), ("install: overlay: " + err).c_str());

	Manifest base1, hd, ov;
	Require(LoadManifest(ob.out, base1) && LoadManifest(oh.out, hd) && LoadManifest(oo.out, ov), "install: leer manifiestos");

	const std::string root = JoinPath(dir, "cliente");
	Write(JoinPath(root, "Content/mods/mio.txt"), std::string("del jugador"));
	const std::string indexPath = JoinPath(root, "User/Cache/install.json");

	// Las URLs de la base son file://; las demás, rutas.  El espejo de la base
	// no existe: no tiene que hacer falta.
	std::vector<ChannelSource> sources = {
		{ &base1, { FileUrl(ob.out), JoinPath(dir, "espejo-que-no-existe") }, 0 },
		{ &hd, { oh.out }, 10 },
		{ &ov, { oo.out }, 20 },
	};
	PlanOptions po;
	po.root = root;
	po.platform = "windows-x64";

	TestTransport transport;
	InstallOptions io;
	io.root = root;
	io.downloadDir = JoinPath(root, "User/Cache/downloads");
	io.indexPath = indexPath;
	io.transport = &transport;
	io.connections = 3;
	io.backoffMs = 1;
	io.backoffMaxMs = 4;
	std::size_t progressCalls = 0;
	io.progress = [&](const InstallStatus&) { ++progressCalls; };

	auto plan = [&](Plan& p, InstallIndex& idx) {
		std::string e;
		Require(BuildPlan(sources, po, idx, p, &e), ("plan: " + e).c_str());
	};
	auto run = [&](const Plan& p, InstallIndex& idx, InstallResult& r) {
		const bool ok = RunPlan(sources, p, idx, io, r);
		for (const std::string& f : r.failures) std::printf("  falla de instalación: %s\n", f.c_str());
		return ok;
	};

	// ── Instalación desde cero ────────────────────────────────────────────
	InstallIndex idx;
	Plan p;
	plan(p, idx);
	Require(p.files.size() == 8, "plan: 6 de la base (casa del overlay) + otra-hd + main.exe");
	Require(!FindPlanned(p, "Content/a/casa-hd.ozj"), "plan: el overlay pisa casa.ozj y tapa su -hd");
	const PlanFile* pc = FindPlanned(p, "Content/a/casa.ozj");
	Require(pc && pc->channel == 2, "plan: casa.ozj sale del overlay");
	Require(FindPlanned(p, "Content/a/otra-hd.ozj") && FindPlanned(p, "main.exe"), "plan: otra-hd y el exe");
	Require(!p.requests.empty() && p.requests.front().required && !p.requests.back().required,
	        "plan: lo obligatorio primero, lo opcional al final");
	InstallResult r;
	Require(run(p, idx, r), "instalar desde cero");
	Require(r.installed == 8 && r.downloaded == p.downloadBytes, "instalar: 8 archivos y lo planeado");
	Require(progressCalls > 0, "instalar: avisa el progreso");
	Require(SameFile(JoinPath(root, "Content/a/casa.ozj"), casaAdmin) && SameFile(JoinPath(root, "main.exe"), exe) &&
	            SameFile(JoinPath(root, "Content/data/big.bin"), big1) && SameFile(JoinPath(root, "Content/a/otra-hd.ozj"), otraHd),
	        "instalar: contenidos correctos");
	Require(!FileExists(JoinPath(root, "Content/a/casa-hd.ozj")), "instalar: no baja el -hd tapado");
	std::vector<std::string> left;
	ListFiles(io.downloadDir, left, &err);
	Require(left.empty(), "instalar: no quedan temporales");
	InstallIndex saved;
	Require(LoadIndex(indexPath, saved, &err) && saved.files.size() == 8 && saved.revision >= 1, "instalar: guarda el índice");
	Require(saved.files["Content/a/casa.ozj"].channel == "prueba/overlay", "índice: canal de cada archivo");

	// ── Nada que hacer; tocar un archivo sin cambiarlo tampoco ────────────
	plan(p, idx);
	Require(p.files.empty() && p.requests.empty() && p.remove.empty() && p.upToDate == 8, "segundo plan vacío");
	Write(JoinPath(root, "Content/a/otra.ozj"), otra);
	plan(p, idx);
	Require(p.files.empty(), "misma data, otra fecha: se rehashea y no se baja");

	// ── Un archivo cambiado baja solo, con un tramo del zip ───────────────
	Write(JoinPath(root, "Content/a/otra.ozj"), std::string("roto"));
	Write(JoinPath(root, "Content/item/Item.toml"), std::string("x=2\n"));
	plan(p, idx);
	Require(p.files.size() == 1 && p.requests.size() == 1, "un archivo cambiado: un pedido (el .toml no se repara)");
	if (p.requests.size() == 1)
	{
		const RangeRequest& rq = p.requests[0];
		Require(rq.length < base1.bundles[rq.bundle].size, "un archivo: tramo del zip, no el zip entero");
	}
	transport.calls.clear();
	transport.corruptCalls = 1;   // y llega roto la primera vez
	Require(run(p, idx, r), "reparar otra.ozj");
	// Intento 1 roto, intento 2 al espejo (no existe), intento 3 bien.
	Require(transport.calls.size() == 3 && SameFile(JoinPath(root, "Content/a/otra.ozj"), otra),
	        "datos corruptos: se vuelve a pedir y queda bien");

	// ── Retomar un pedido cortado ─────────────────────────────────────────
	RemoveFile(JoinPath(root, "Content/data/big.bin"));
	plan(p, idx);
	Require(p.files.size() == 1 && p.requests.size() == 1, "falta big.bin: un pedido");
	transport.calls.clear();
	transport.cutCalls = 1;
	transport.cutAfter = 1 << 20;
	Require(run(p, idx, r), "bajar big.bin con un corte");
	// Intento 1 cortado, intento 2 al espejo (no existe), intento 3 retoma.
	Require(transport.calls.size() == 3 && transport.calls[1].url.find("espejo") != std::string::npos &&
	            transport.calls[2].offset == transport.calls[0].offset + (1 << 20) &&
	            transport.calls[2].length == transport.calls[0].length - (1 << 20),
	        "corte: reintenta con el espejo y retoma desde donde quedó");
	Require(SameFile(JoinPath(root, "Content/data/big.bin"), big1), "corte: big.bin queda bien");

	// ── Base v2: delta del archivo grande, y un archivo que se va ─────────
	const std::string bin2 = JoinPath(dir, "inst-base2");
	auto big2 = big1;
	for (std::size_t i = 0; i < 4096; ++i) big2[1000 + i * 600] ^= 0x5A;
	Write(JoinPath(bin2, "Content/a/casa.ozj"), casa);
	Write(JoinPath(bin2, "Content/a/otra.ozj"), otra);
	Write(JoinPath(bin2, "Content/data/big.bin"), big2);
	Write(JoinPath(bin2, "Content/item/Item.toml"), std::string("x=1\n"));
	Write(JoinPath(bin2, "Content/music/main.ogg"), music);
	MuPatch::BuildOptions ob2 = ob;
	ob2.version = "0.97.51";
	ob2.in = bin2;
	ob2.out = JoinPath(dir, "inst-out-base2");
	ob2.prev = { { ob.out, "" } };
	Require(Build(ob2, &err), ("install: base v2: " + err).c_str());
	Manifest base2;
	Require(LoadManifest(ob2.out, base2), "install: leer base v2");
	// Los zips que v2 reusa de v1 no tienen url: en la prueba, v1 va de espejo.
	sources[0] = { &base2, { ob2.out, ob.out }, 0 };

	plan(p, idx);
	const PlanFile* pb = FindPlanned(p, "Content/data/big.bin");
	Require(p.files.size() == 1 && pb && pb->delta, "v2: sólo big.bin, con delta");
	Require(p.downloadBytes < big2.size() / 4, "v2: el delta pesa mucho menos que el archivo");
	Require(p.remove.size() == 1 && p.remove[0] == "System/x.cfg", "v2: se borra lo que la base ya no trae");
	Require(run(p, idx, r), "instalar v2");
	Require(r.deltasApplied == 1 && r.removed == 1 && SameFile(JoinPath(root, "Content/data/big.bin"), big2) &&
	            !FileExists(JoinPath(root, "System/x.cfg")),
	        "v2: delta aplicado y archivo borrado");
	Require(FileExists(JoinPath(root, "Content/mods/mio.txt")), "v2: no toca lo que no instaló el launcher");
	std::string toml;
	Require(ReadWholeFile(JoinPath(root, "Content/item/Item.toml"), toml) && toml == "x=2\n", "v2: el .toml queda como estaba");

	// ── Un delta que ya no sirve: se baja el archivo entero ───────────────
	Write(JoinPath(root, "Content/data/big.bin"), big1);
	plan(p, idx);
	pb = FindPlanned(p, "Content/data/big.bin");
	Require(pb && pb->delta, "big.bin vuelto a v1: delta");
	Write(JoinPath(root, "Content/data/big.bin"), Random(1000, 99));   // cambia entre el plan y la descarga
	Require(run(p, idx, r), "delta que falla");
	Require(r.deltaFallbacks == 1 && r.deltasApplied == 0 && SameFile(JoinPath(root, "Content/data/big.bin"), big2),
	        "delta que falla: baja el archivo entero");

	// ── Con el juego abierto: sólo agregar lo que falta ───────────────────
	RemoveFile(JoinPath(root, "Content/music/main.ogg"));
	Write(JoinPath(root, "Content/a/otra.ozj"), std::string("roto"));
	po.gameRunning = true;
	plan(p, idx);
	po.gameRunning = false;
	Require(p.files.size() == 1 && FindPlanned(p, "Content/music/main.ogg") && p.remove.empty(),
	        "juego abierto: sólo lo que falta, nada que reemplazar ni borrar");

	// ── Sólo el launcher: el overlay no trae ninguno, no se baja nada ─────
	plan(p, idx);
	Require(!p.selfUpdate, "sin launcher en el overlay: selfUpdate apagado");
	io.selfOnly = true;
	Require(run(p, idx, r) && r.installed == 0, "selfOnly sin launcher: no instala nada");
	io.selfOnly = false;

	// ── Cancelar ──────────────────────────────────────────────────────────
	std::atomic<bool> cancel{ true };
	io.cancel = &cancel;
	Require(!run(p, idx, r) && r.cancelled && r.installed == 0, "cancelado: no instala nada");
	io.cancel = nullptr;
	Require(run(p, idx, r) && r.installed == 2, "después de cancelar se completa");
	k.Wipe();
}

void TestChannelFetch(const std::string& dir)
{
	std::string err;
	KeyPair k, other;
	Require(GenerateKey(k) && GenerateKey(other), "claves fetch");
	const std::string key = JoinPath(dir, "fetch.key");
	Require(SaveKeyFile(key, k), "guardar clave fetch");

	const std::string in = JoinPath(dir, "fetch-in");
	Write(JoinPath(in, "Content/a.txt"), std::string("a"));
	MuPatch::BuildOptions o;
	o.channel = "mu-linux/base";
	o.version = "0.97.50";
	o.in = in;
	o.out = JoinPath(dir, "fetch-v1");
	o.keyPath = key;
	o.sequence = 5;
	Require(Build(o, &err), ("fetch: build v1: " + err).c_str());
	MuPatch::BuildOptions o2 = o;
	o2.version = "0.97.51";
	o2.out = JoinPath(dir, "fetch-v2");
	o2.sequence = 6;
	Require(Build(o2, &err), ("fetch: build v2: " + err).c_str());

	FileTransport ft;
	ChannelSpec spec;
	spec.channel = "mu-linux/base";
	spec.urls = { JoinPath(dir, "fetch-no-existe"), FileUrl(o.out) };
	std::memcpy(spec.publicKey, k.publicKey, kKeySize);
	spec.version = "0.97.50";

	SequenceStore seq;
	FetchedChannel f;
	Require(FetchChannel(ft, spec, seq, f, &err) && f.manifest.version == "0.97.50" && f.urls.size() == 2 &&
	            f.urls[0] == FileUrl(o.out), ("fetch: un espejo caído y el siguiente bien: " + err).c_str());
	Require(seq.Seen("mu-linux/base") == 5, "fetch: registra la secuencia");
	ChannelSource src = SourceOf(f, 10);
	Require(src.manifest == &f.manifest && src.priority == 10 && src.baseUrls == f.urls, "SourceOf");

	ChannelSpec bad = spec;
	std::memcpy(bad.publicKey, other.publicKey, kKeySize);
	Require(!FetchChannel(ft, bad, seq, f, &err), "fetch: otra clave se rechaza");
	bad = spec;
	bad.version = "0.97.49";
	Require(!FetchChannel(ft, bad, seq, f, &err), "fetch: otra versión se rechaza");
	bad = spec;
	bad.channel = "mu-linux/hd";
	Require(!FetchChannel(ft, bad, seq, f, &err), "fetch: otro canal se rechaza");

	// La v2 sube la secuencia; después la v1 (con firma válida) ya no se acepta.
	ChannelSpec spec2 = spec;
	spec2.urls = { o2.out };
	spec2.version.clear();
	Require(FetchChannel(ft, spec2, seq, f, &err) && seq.Seen("mu-linux/base") == 6, "fetch: v2 sube la secuencia");
	Require(!FetchChannel(ft, spec, seq, f, &err), "fetch: un parche viejo con firma válida se rechaza");

	const std::string seqPath = JoinPath(dir, "fetch-cache/sequences.json");
	Require(seq.Save(seqPath), "guardar secuencias");
	SequenceStore back;
	Require(back.Load(seqPath, &err) && back.Seen("mu-linux/base") == 6 && back.Seen("otro/canal") == 0, "releer secuencias");
	Write(seqPath, std::string("{\"channels\": {\"mu-linux/base\": -1}}"));
	Require(!back.Load(seqPath, &err), "secuencias rotas: se rechazan");

	Require(ReleaseManifestName("mu-linux/base") == "mu-linux-base.manifest.json", "ReleaseManifestName");
	Require(GithubReleaseUrl("dueño/repo", "0.97.50") == "https://github.com/dueño/repo/releases/download/0.97.50", "GithubReleaseUrl");

	Manifest base, ov;
	base.version = "0.97.50";
	ov.requiresBase = "0.97.50";
	ov.requiresProtocol = 13;
	Require(CheckRequires(base, ov, 13, &err) && CheckRequires(base, ov, 0, &err), "requires: coincide");
	Require(!CheckRequires(base, ov, 12, &err), "requires: otro protocolo");
	base.version = "0.97.51";
	Require(!CheckRequires(base, ov, 13, &err), "requires: otra base");
	k.Wipe();
	other.Wipe();
}

// Un zip como el que arma cualquier herramienta: Deflate.
void WriteDeflateZip(const std::string& path, const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& files)
{
	ZipWriter w;
	Require(w.Open(path), "abrir zip deflate");
	for (const auto& [name, data] : files)
	{
		std::vector<std::uint8_t> packed;
		std::string err;
		Require(DeflateCompress(data.data(), data.size(), 6, packed, &err), "deflate");
		Require(w.Add(name, ZipMethod::Deflate, packed.data(), packed.size(), data.size(), Crc32(data.data(), data.size()), nullptr),
		        "agregar al zip deflate");
	}
	Require(w.Finish(), "cerrar zip deflate");
}

void TestDeflate(const std::string& dir)
{
	const auto data = Compressible(100000, 61);
	const std::string zip = JoinPath(dir, "deflate.zip");
	WriteDeflateZip(zip, { { "a/b.txt", data } });
	ZipReader r;
	std::string err;
	std::vector<std::uint8_t> out;
	Require(r.Open(zip, &err) && r.Entries().size() == 1 && r.Entries()[0].method == ZipMethod::Deflate, "deflate: listar");
	Require(ZipExtract(zip, r.Entries()[0], out, &err) && out == data, ("deflate: descomprimir: " + err).c_str());
	std::vector<std::uint8_t> packed;
	Require(DeflateCompress(data.data(), data.size(), 6, packed, &err), "deflate 2");
	packed[packed.size() / 2] ^= 0xFF;
	Require(!ZipDecode(ZipMethod::Deflate, packed.data(), packed.size(), data.size(), out, &err), "deflate: datos rotos se rechazan");
}

void TestPublish(const std::string& dir)
{
	std::uint64_t a = 0, b = 0;
	Require(MuPatch::ParsePatchFolder("19112026-1", &a) && MuPatch::ParsePatchFolder("01122026-1", &b) && a < b,
	        "carpetas: el orden es por fecha, no alfabético");
	Require(MuPatch::ParsePatchFolder("19112026-2", &b) && a < b, "carpetas: y por número del día");
	Require(!MuPatch::ParsePatchFolder("parche-nuevo", nullptr) && !MuPatch::ParsePatchFolder("32112026-1", nullptr) &&
	            !MuPatch::ParsePatchFolder("19112026-0", nullptr) && !MuPatch::ParsePatchFolder("19112026", nullptr),
	        "carpetas: formatos inválidos");

	std::string err;
	KeyPair k;
	Require(GenerateKey(k), "clave publish");
	const std::string key = JoinPath(dir, "ServerIdentity.key");
	Require(SaveKeyFile(key, k), "guardar clave publish");
	const std::string patches = JoinPath(dir, "Patches");
	const std::string out = JoinPath(patches, "published");

	// Parche 1: sueltos (el exe y una textura).  Parche 2 (fecha anterior en el
	// texto pero posterior en el calendario): zip Deflate que pisa la textura y
	// borra un archivo del 1.  Una carpeta con otro nombre se ignora.
	const auto exe = FakeExe("0.97.50"), tex1 = Random(30000, 62), tex2 = Random(30000, 63), extra = Random(2000, 64);
	Write(JoinPath(patches, "19112026-1/main.exe"), exe);
	Write(JoinPath(patches, "19112026-1/Content/item/casa.ozj"), tex1);
	Write(JoinPath(patches, "19112026-1/Content/item/viejo.ozj"), extra);
	MakeDirs(JoinPath(patches, "01122026-1"));
	WriteDeflateZip(JoinPath(patches, "01122026-1/update.zip"), { { "Content/item/casa.ozj", tex2 } });
	Write(JoinPath(patches, "01122026-1/deleted.txt"), std::string("# comentario\r\nContent\\item\\viejo.ozj\r\n"));
	Write(JoinPath(patches, "borrador/main.exe"), FakeExe("0.97.49"));

	MuPatch::PublishOptions o;
	o.patches = patches;
	o.channel = "miserver/overlay";
	o.keyPath = key;
	o.level = 3;
	o.threads = 2;
	bool published = false;
	Require(MuPatch::PublishPatches(o, &published, &err) && published, ("publish 1: " + err).c_str());

	MuPatch::VerifyOptions v;
	std::memcpy(v.publicKey, k.publicKey, kKeySize);
	v.in = out;
	Require(MuPatch::VerifyPatch(v, &err), ("publish 1: verify: " + err).c_str());
	Manifest m1;
	std::string text;
	Require(ReadWholeFile(JoinPath(out, "manifest.json"), text) && ParseManifest(text, m1, &err), "publish 1: manifiesto");
	Require(m1.sequence == 1 && m1.version == "1.0.1" && m1.requiresBase == "0.97.50", "publish 1: secuencia, versión y base del exe");
	Require(m1.files.size() == 2, "publish 1: el exe y la textura (viejo.ozj borrado)");
	for (const FileEntry& f : m1.files)
		if (f.path == "Content/item/casa.ozj") Require(f.hash == HashBytes(tex2.data(), tex2.size()), "publish 1: gana el parche más nuevo");
	Require(m1.deleted.size() == 1 && m1.deleted[0] == "Content/item/viejo.ozj", "publish 1: deleted.txt");
	Require(FileExists(JoinPath(out, "history/1/manifest.json")) && !FileExists(JoinPath(out, ".staging")), "publish 1: historial y sin temporales");

	// Sin cambios: no hay versión nueva.
	Require(MuPatch::PublishPatches(o, &published, &err) && !published, "publish sin cambios");

	// Un parche más: sólo lo nuevo en zips nuevos; lo demás se reusa sin copiar.
	Write(JoinPath(patches, "02122026-1/Content/item/nuevo.ozj"), Random(4000, 65));
	Require(MuPatch::PublishPatches(o, &published, &err) && published, ("publish 2: " + err).c_str());
	Manifest m2;
	Require(ReadWholeFile(JoinPath(out, "manifest.json"), text) && ParseManifest(text, m2, &err) && m2.sequence == 2, "publish 2: secuencia 2");
	std::size_t reused = 0;
	for (const FileEntry& f : m2.files)
		if (m2.bundles[f.bundle].name.find("-1.0.1-") != std::string::npos) ++reused;
	Require(reused == 2 && m2.files.size() == 3, "publish 2: los 2 archivos anteriores vienen de los zips de la 1");
	v.in = out;
	Require(MuPatch::VerifyPatch(v, &err), ("publish 2: verify: " + err).c_str());

	// El admin saca la textura nueva: el zip de la 1 deja de usarse cuando ya
	// no está en ninguna de las últimas versiones.
	Write(JoinPath(patches, "03122026-1/deleted.txt"), std::string("Content/item/casa.ozj\nContent/item/nuevo.ozj\n"));
	Require(MuPatch::PublishPatches(o, &published, &err) && published, ("publish 3: " + err).c_str());
	for (int i = 4; i <= 6; ++i)
	{
		Write(JoinPath(patches, "0" + std::to_string(i) + "122026-1/Content/x" + std::to_string(i) + ".txt"), std::string("x"));
		Require(MuPatch::PublishPatches(o, &published, &err) && published, ("publish 4..6: " + err).c_str());
	}
	std::vector<std::string> left;
	ListFiles(out, left, &err);
	bool oldZip = false;
	for (const std::string& f : left) oldZip |= f.find("-1.0.1-") != std::string::npos && f.find('/') == std::string::npos;
	Require(m2.sequence == 2, "publish: guardas");
	Manifest m6;
	Require(ReadWholeFile(JoinPath(out, "manifest.json"), text) && ParseManifest(text, m6, &err) && m6.sequence == 6, "publish 6");
	bool exeFromV1 = false;
	for (const FileEntry& f : m6.files)
		if (f.path == "main.exe") exeFromV1 = m6.bundles[f.bundle].name.find("-1.0.1-") != std::string::npos;
	Require(exeFromV1 && oldZip, "publish: un zip se conserva mientras alguna versión lo use");
	bool v2Zip = false;
	for (const std::string& f : left) v2Zip |= f.find("-1.0.2-") != std::string::npos && f.find('/') == std::string::npos;
	Require(!v2Zip, "publish: el zip que sólo usaba la 2 se borra cuando sale de las últimas 3");
	Require(!FileExists(JoinPath(out, "history/1")) && FileExists(JoinPath(out, "history/6/manifest.json")),
	        "publish: se guardan sólo las últimas 3 versiones");
	v.in = out;
	Require(MuPatch::VerifyPatch(v, &err), ("publish 6: verify: " + err).c_str());

	// Un zip con una ruta peligrosa se rechaza.
	const std::string bad = JoinPath(dir, "PatchesMalos");
	MakeDirs(JoinPath(bad, "19112026-1"));
	WriteDeflateZip(JoinPath(bad, "19112026-1/update.zip"), { { "../fuera.txt", extra } });
	MuPatch::PublishOptions ob = o;
	ob.patches = bad;
	Require(!MuPatch::PublishPatches(ob, &published, &err), "publish: rechaza rutas fuera de la carpeta");

	// Snapshot: cambia si cambia un archivo e ignora published/.
	const std::string s1 = MuPatch::SnapshotPatches(patches, out);
	Write(JoinPath(out, "otra-cosa.txt"), std::string("y"));
	Require(MuPatch::SnapshotPatches(patches, out) == s1, "snapshot: ignora published/");
	Write(JoinPath(patches, "06122026-1/Content/x6.txt"), std::string("xx"));
	Require(MuPatch::SnapshotPatches(patches, out) != s1, "snapshot: ve un cambio");
	k.Wipe();
}

void TestAnnounce(const std::string& dir)
{
	std::string host;
	std::uint16_t port = 0;
	Require(ParseHostPort("mu.ejemplo.com:44405", &host, &port) && host == "mu.ejemplo.com" && port == 44405, "host:puerto");
	Require(!ParseHostPort("mu.ejemplo.com", nullptr, nullptr) && !ParseHostPort("x:0", nullptr, nullptr) &&
	            !ParseHostPort("x:70000", nullptr, nullptr) && !ParseHostPort("a b:1", nullptr, nullptr) &&
	            !ParseHostPort(":1", nullptr, nullptr),
	        "host:puerto inválidos");

	Announce a;
	a.sequence = 4;
	a.connectServers = { "mu.ejemplo.com:44405", "10.0.0.2:44405" };
	a.patchChannel = "miserver/overlay";
	a.patchUrls = { "https://mu.ejemplo.com/updates" };
	a.baseSource = "dueno/repo";
	a.baseTag = "0.97.50";
	a.news = "Mantenimiento el lunes.\nVuelve \"pronto\".";
	a.web = "https://mu.ejemplo.com";
	Announce b;
	std::string err;
	Require(ParseAnnounce(DumpAnnounce(a), b, &err) && b.sequence == 4 && SameAnnounceContent(a, b), "launcher.json: ida y vuelta");
	Require(ParseAnnounce("{\"format\":1,\"sequence\":1,\"nuevo\":true}", b, &err) && b.connectServers.empty(),
	        "launcher.json: un campo desconocido se ignora");
	Require(!ParseAnnounce("{\"format\":2,\"sequence\":1}", b, &err), "launcher.json: formato desconocido");
	Require(!ParseAnnounce("{\"format\":1}", b, &err), "launcher.json: sin secuencia");
	Require(!ParseAnnounce("{\"format\":1,\"sequence\":1,\"connectServers\":[\"sin-puerto\"]}", b, &err),
	        "launcher.json: dirección inválida");
	Require(!ParseAnnounce("{\"format\":1,\"sequence\":1,\"patches\":{\"urls\":[\"ftp://x\"]}}", b, &err),
	        "launcher.json: url inválida");
	Require(!ParseAnnounce("{\"format\":1,\"sequence\":1,\"news\":\"a\\u0007b\"}", b, &err), "launcher.json: control en news");
	Require(!ParseAnnounce(std::string(kAnnounceMaxSize + 1, ' '), b, &err), "launcher.json: tope de tamaño");
	Require(ParseAnnounce(DumpAnnounce(a), b, &err) && b.web == "https://mu.ejemplo.com", "launcher.json: sitio del server");
	Require(!ParseAnnounce("{\"format\":1,\"sequence\":1,\"web\":\"javascript:alert(1)\"}", b, &err),
	        "launcher.json: un sitio que no es http no vale");

	// Publicar: secuencia 1, sin cambios no reescribe, un cambio sube la
	// secuencia, y con otra clave (rotación) la secuencia sigue.
	KeyPair k1, k2;
	Require(GenerateKey(k1) && GenerateKey(k2), "claves launcher.json");
	const std::string key = JoinPath(dir, "announce.key");
	Require(SaveKeyFile(key, k1), "guardar clave launcher.json");
	MuPatch::AnnounceOptions o;
	o.out = JoinPath(dir, "announce");
	o.keyPath = key;
	o.content = a;
	bool wrote = false;
	Require(MuPatch::PublishAnnounce(o, &wrote, &err) && wrote, "launcher.json: publicar");
	auto load = [&](Announce& m, const KeyPair& k) {
		std::string text, sigText;
		std::uint8_t sig[kSignatureSize];
		return ReadWholeFile(JoinPath(o.out, "launcher.json"), text) && ReadWholeFile(JoinPath(o.out, "launcher.json.sig"), sigText) &&
		       ParseSignatureFile(sigText, sig) && Verify(kLauncherContext, text.data(), text.size(), sig, k.publicKey) &&
		       !Verify(kPatchContext, text.data(), text.size(), sig, k.publicKey) && ParseAnnounce(text, m, nullptr);
	};
	Announce got;
	Require(load(got, k1) && got.sequence == 1 && SameAnnounceContent(got, a), "launcher.json: firmado con MuLauncher1");
	Require(MuPatch::PublishAnnounce(o, &wrote, &err) && !wrote, "launcher.json: sin cambios no se reescribe");
	o.content.news = "Otra noticia";
	Require(MuPatch::PublishAnnounce(o, &wrote, &err) && wrote && load(got, k1) && got.sequence == 2, "launcher.json: un cambio sube la secuencia");
	Require(SaveKeyFile(key, k2), "rotar clave launcher.json");
	Require(MuPatch::PublishAnnounce(o, &wrote, &err) && wrote && load(got, k2) && got.sequence == 3,
	        "launcher.json: con la clave nueva se vuelve a firmar y la secuencia sigue");
	o.content.connectServers = { "sin-puerto" };
	Require(!MuPatch::PublishAnnounce(o, &wrote, &err) && load(got, k2) && got.sequence == 3,
	        "launcher.json: un valor inválido no publica");
	k1.Wipe();
	k2.Wipe();
}

} // namespace

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / ("mu_update_test_" + std::to_string(std::random_device{}()));
	fs::create_directories(dir);
	const std::string d = dir.generic_string();

	TestJson();
	TestPaths();
	TestZipAndDelta(d);
	TestSigning(d);
	TestManifestValidation();
	TestEndToEnd(d);
	TestHdVariantAndIndex(d);
	TestRateLimiter();
	TestInstall(d);
	TestChannelFetch(d);
	TestDeflate(d);
	TestPublish(d);
	TestAnnounce(d);

	std::error_code ec;
	fs::remove_all(dir, ec);
	if (s_Failures)
	{
		std::printf("%d fallas\n", s_Failures);
		return 1;
	}
	std::printf("update: todo bien\n");
	return 0;
}
