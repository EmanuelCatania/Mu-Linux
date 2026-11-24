#pragma once

#include "ProtocolDefines.h"

//**********************************************//
//********** Client -> ConnectServer ***********//
//**********************************************//

struct PMSG_SERVER_LIST_RECV
{
	PSBMSG_HEAD header; // C1:F4:02
};

struct PMSG_SERVER_INFO_RECV
{
	PSBMSG_HEAD header; // C1:F4:03
	BYTE ServerCode;
};

// Contenido del cliente (ClientContent): sólo por el canal cifrado.  Sin relleno
// entre campos, igual en el cliente (net/Protocol/ConnectServerProtocol.h).
#pragma pack(push, 1)

struct PMSG_CONTENT_MANIFEST_RECV
{
	PSBMSG_HEAD header; // C1:F4:10
	DWORD serial;       // 0 = la versión actual
	WORD first;         // primera entrada pedida
};

struct PMSG_CONTENT_FILE_RECV
{
	PSBMSG_HEAD header; // C1:F4:12
	DWORD serial;
	WORD index;         // entrada del manifiesto
	DWORD offset;
};

// Configuración del launcher (Data/Patches/published/launcher.json, la arma y la
// firma mu-patch).  El launcher la pide de a pedazos desde `offset`.
struct PMSG_LAUNCHER_CONFIG_RECV
{
	PSBMSG_HEAD header; // C1:F4:14
	DWORD offset;
};

// Qué le pide el server al cliente de escritorio (por ahora, que lo abra el
// launcher).  Lo pide el cliente en la intro, junto con el manifiesto.
struct PMSG_CLIENT_POLICY_RECV
{
	PSBMSG_HEAD header; // C1:F4:16
};

#pragma pack(pop)

//**********************************************//
//********** ConnectServer -> Client ***********//
//**********************************************//

struct PMSG_SERVER_INIT_SEND
{
	PBMSG_HEAD header; // C1:00
	BYTE result;
};

struct PMSG_CUSTOM_SERVER_LIST_SEND
{
	PSWMSG_HEAD header; // C1:F4:01
	BYTE count[2];
};

struct PMSG_CUSTOM_SERVER_LIST
{
	WORD ServerCode;
	char ServerName[32];
};

struct PMSG_SERVER_LIST_SEND
{
	PSWMSG_HEAD header; // C1:F4:02
	BYTE count;
};

struct PMSG_SERVER_LIST
{
	WORD ServerCode;
	BYTE UserTotal;
};

struct PMSG_SERVER_INFO_SEND
{
	PSBMSG_HEAD header; // C1:F4:03
	char ServerAddress[16];
	WORD ServerPort;
};

#pragma pack(push, 1)

// Resultado de F4:11 y F4:13.
enum eContentResult
{
	CONTENT_OK = 0,
	CONTENT_NOT_SECURE = 1,   // la conexión no tiene canal cifrado: no se reparte nada
	CONTENT_CHANGED = 2,      // el manifiesto cambió: hay que volver a pedirlo
	CONTENT_BAD_REQUEST = 3,  // índice u offset fuera de rango
	CONTENT_NOT_FOUND = 4,    // (F4:15) el server no publica launcher.json
};

// Seguido de `count` entradas: BYTE largo de la ruta, la ruta (sin el cero
// final, relativa a Content/ y con '/'), DWORD tamaño, BYTE sha256[32].
struct PMSG_CONTENT_MANIFEST_SEND
{
	PSWMSG_HEAD header; // C2:F4:11
	BYTE result;
	DWORD serial;
	WORD total;         // entradas del manifiesto entero
	WORD first;
	BYTE count;
};

// Seguido de `length` bytes del archivo desde `offset`.
struct PMSG_CONTENT_FILE_SEND
{
	PSWMSG_HEAD header; // C2:F4:13
	BYTE result;
	DWORD serial;
	WORD index;
	DWORD offset;
	DWORD size;         // tamaño del archivo entero
	WORD length;
};

// Bits de PMSG_CLIENT_POLICY_SEND::flags.
enum eClientPolicy
{
	// El cliente de escritorio abierto sin el launcher abre el launcher y se
	// cierra (ConnectServer.ini [Launcher] RequireLauncher).  No es seguridad:
	// es para que nadie juegue sin actualizar.  La versión web no lo aplica.
	CLIENT_POLICY_REQUIRE_LAUNCHER = 0x01,
};

struct PMSG_CLIENT_POLICY_SEND
{
	PSBMSG_HEAD header; // C1:F4:17
	BYTE result;        // CONTENT_OK o CONTENT_NOT_SECURE (entonces flags = 0)
	BYTE flags;
};

// Seguido de `length` bytes desde `offset` de: la firma (64 bytes, contexto
// MuLauncher1) y después launcher.json tal cual.  El ConnectServer no lo
// interpreta: el launcher verifica la firma con la identidad del server.
struct PMSG_LAUNCHER_CONFIG_SEND
{
	PSWMSG_HEAD header; // C2:F4:15
	BYTE result;
	DWORD size;         // firma + archivo
	DWORD offset;
	WORD length;
};

#pragma pack(pop)

// Los mismos tamaños que en el cliente (net/Protocol/ConnectServerProtocol.h).
static_assert(sizeof(PMSG_CONTENT_MANIFEST_RECV) == 10, "F4:10");
static_assert(sizeof(PMSG_CONTENT_MANIFEST_SEND) == 15, "F4:11");
static_assert(sizeof(PMSG_CONTENT_FILE_RECV) == 14, "F4:12");
static_assert(sizeof(PMSG_CONTENT_FILE_SEND) == 22, "F4:13");
static_assert(sizeof(PMSG_LAUNCHER_CONFIG_RECV) == 8, "F4:14");
static_assert(sizeof(PMSG_LAUNCHER_CONFIG_SEND) == 16, "F4:15");
static_assert(sizeof(PMSG_CLIENT_POLICY_RECV) == 4, "F4:16");
static_assert(sizeof(PMSG_CLIENT_POLICY_SEND) == 6, "F4:17");

// Bytes de archivo por paquete: entra con el encabezado en MAX_PACKET_SIZE.
#define CONTENT_FILE_CHUNK 2000

// Tope de launcher.json (el mismo que Update::kAnnounceMaxSize del launcher).
#define LAUNCHER_CONFIG_MAX_SIZE (16 * 1024)

//**********************************************//
//**********************************************//
//**********************************************//

void ConnectServerProtocolCore(int index, BYTE head, BYTE* lpMsg, int size);

void CCServerInfoRecv(PMSG_SERVER_INFO_RECV* lpMsg, int index);

void CCServerListRecv(PMSG_SERVER_LIST_RECV* lpMsg, int index);

void CCCustomServerListSend(int index);

void CCServerInitSend(int index, int result);

void CCContentManifestRecv(PMSG_CONTENT_MANIFEST_RECV* lpMsg, int size, int index);

void CCContentFileRecv(PMSG_CONTENT_FILE_RECV* lpMsg, int size, int index);

void CCClientPolicyRecv(PMSG_CLIENT_POLICY_RECV* lpMsg, int size, int index);

void CCLauncherConfigRecv(PMSG_LAUNCHER_CONFIG_RECV* lpMsg, int size, int index);