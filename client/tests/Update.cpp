// Núcleo del actualizador (src/update) y mu-patch (tools/mu-patch): JSON, zips,
// deltas, firma, validación del manifiesto y un canal armado y verificado de
// punta a punta, con una segunda versión que reusa zips y lleva deltas.

#include "PatchBuilder.h"

#include "update/Delta.h"
#include "update/FileUtil.h"
#include "update/Hash.h"
#include "update/Json.h"
#include "update/Manifest.h"
#include "update/Signing.h"
#include "update/Zip.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
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
