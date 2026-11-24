#include "stdafx.h"
#include "ConnectServerProtocol.h"
#include "ClientManager.h"
#include "ClientContent.h"
#include "SecureLink.h"
#include "ServerList.h"
#include "SocketManager.h"
#include "Util.h"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>

void ConnectServerProtocolCore(int index, BYTE head, BYTE* lpMsg, int size)
{
	ConsoleProtocolLog(CON_PROTO_TCP_RECV, lpMsg, size);

	gClientManager[index].m_PacketTime = GetTickCount();

	switch (head)
	{
		case 0xF4:
		{
			switch (lpMsg[3])
			{
				case 0x02:
				{
					CCCustomServerListSend(index);

					CCServerListRecv((PMSG_SERVER_LIST_RECV*)lpMsg, index);

					break;
				}

				case 0x03:
				{
					CCServerInfoRecv((PMSG_SERVER_INFO_RECV*)lpMsg, index);

					break;
				}

				case 0x10:
				{
					CCContentManifestRecv((PMSG_CONTENT_MANIFEST_RECV*)lpMsg, size, index);

					break;
				}

				case 0x12:
				{
					CCContentFileRecv((PMSG_CONTENT_FILE_RECV*)lpMsg, size, index);

					break;
				}

				case 0x14:
				{
					CCLauncherConfigRecv((PMSG_LAUNCHER_CONFIG_RECV*)lpMsg, size, index);

					break;
				}

				case 0x16:
				{
					CCClientPolicyRecv((PMSG_CLIENT_POLICY_RECV*)lpMsg, size, index);

					break;
				}
			}

			break;
		}
	}
}

void CCServerInitSend(int index, int result)
{
	PMSG_SERVER_INIT_SEND pMsg;

	pMsg.header.set(0x00, sizeof(pMsg));

	pMsg.result = result;

	gSocketManager.DataSend(index, (BYTE*)&pMsg, pMsg.header.size);
}

void CCCustomServerListSend(int index)
{
	BYTE send[2048];

	PMSG_CUSTOM_SERVER_LIST_SEND pMsg;

	pMsg.header.set(0xF4, 0x04, 0);

	int size = sizeof(pMsg);

	int count = gServerList.GenerateCustomServerList(send, &size);

	pMsg.count[0] = SET_NUMBERHB(count);

	pMsg.count[1] = SET_NUMBERLB(count);

	pMsg.header.size[0] = SET_NUMBERHB(size);

	pMsg.header.size[1] = SET_NUMBERLB(size);

	memcpy(send, &pMsg, sizeof(pMsg));

	gSocketManager.DataSend(index, send, size);
}

void CCServerListRecv(PMSG_SERVER_LIST_RECV* lpMsg, int index)
{
	BYTE send[2048];

	PMSG_SERVER_LIST_SEND pMsg;

	pMsg.header.set(0xF4, 0x02, 0);

	int size = sizeof(pMsg);

	int count = gServerList.GenerateServerList(send, &size);

	pMsg.count = count;

	pMsg.header.size[0] = SET_NUMBERHB(size);

	pMsg.header.size[1] = SET_NUMBERLB(size);

	memcpy(send, &pMsg, sizeof(pMsg));

	gSocketManager.DataSend(index, send, size);
}

void CCServerInfoRecv(PMSG_SERVER_INFO_RECV* lpMsg, int index)
{
	if (gServerList.CheckJoinServerState() == 0)
	{
		return;
	}

	SERVER_LIST_INFO* lpServerListInfo = gServerList.GetServerListInfo(lpMsg->ServerCode);

	if (lpServerListInfo == 0)
	{
		return;
	}

	if (lpServerListInfo->ServerShow == 0 || lpServerListInfo->ServerState == 0)
	{
		return;
	}

	PMSG_SERVER_INFO_SEND pMsg;

	pMsg.header.set(0xF4, 0x03, sizeof(pMsg));

	memcpy(pMsg.ServerAddress, lpServerListInfo->ServerAddress, sizeof(pMsg.ServerAddress));

	pMsg.ServerPort = lpServerListInfo->ServerPort;

	gSocketManager.DataSend(index, (BYTE*)&pMsg, pMsg.header.size);
}

void CCContentManifestRecv(PMSG_CONTENT_MANIFEST_RECV* lpMsg, int size, int index)
{
	if (size < (int)sizeof(PMSG_CONTENT_MANIFEST_RECV))
	{
		return;
	}

	BYTE send[MAX_PACKET_SIZE];

	PMSG_CONTENT_MANIFEST_SEND pMsg;

	memset(&pMsg, 0, sizeof(pMsg));

	int written = 0, total = 0;

	if (gSecureLink.IsSecure(index) == false)
	{
		pMsg.result = CONTENT_NOT_SECURE;
	}
	else
	{
		const int count = gClientContent.WriteEntries(lpMsg->serial, lpMsg->first, &send[sizeof(pMsg)], MAX_PACKET_SIZE - (int)sizeof(pMsg), &written, &total);

		if (count < 0)
		{
			pMsg.result = CONTENT_CHANGED;

			written = 0;
		}
		else
		{
			pMsg.result = CONTENT_OK;

			pMsg.count = (BYTE)count;
		}

		pMsg.serial = gClientContent.CurrentSerial();

		pMsg.total = (WORD)total;

		pMsg.first = lpMsg->first;
	}

	const int length = (int)sizeof(pMsg) + written;

	pMsg.header.set(0xF4, 0x11, (WORD)length);

	memcpy(send, &pMsg, sizeof(pMsg));

	gSocketManager.DataSend(index, send, length);
}

void CCContentFileRecv(PMSG_CONTENT_FILE_RECV* lpMsg, int size, int index)
{
	if (size < (int)sizeof(PMSG_CONTENT_FILE_RECV))
	{
		return;
	}

	BYTE send[sizeof(PMSG_CONTENT_FILE_SEND) + CONTENT_FILE_CHUNK];

	PMSG_CONTENT_FILE_SEND pMsg;

	memset(&pMsg, 0, sizeof(pMsg));

	pMsg.serial = lpMsg->serial;

	pMsg.index = lpMsg->index;

	pMsg.offset = lpMsg->offset;

	int length = 0;

	if (gSecureLink.IsSecure(index) == false)
	{
		pMsg.result = CONTENT_NOT_SECURE;
	}
	else if (lpMsg->serial != gClientContent.CurrentSerial())
	{
		pMsg.result = CONTENT_CHANGED;
	}
	else if (gClientContent.ReadFile(lpMsg->serial, lpMsg->index, lpMsg->offset, &send[sizeof(pMsg)], CONTENT_FILE_CHUNK, &length, &pMsg.size) == false)
	{
		// Puede haber cambiado entre las dos consultas: el cliente vuelve a pedir
		// el manifiesto en cualquiera de los dos casos.
		pMsg.result = (BYTE)((lpMsg->serial != gClientContent.CurrentSerial()) ? CONTENT_CHANGED : CONTENT_BAD_REQUEST);

		length = 0;
	}
	else
	{
		pMsg.result = CONTENT_OK;
	}

	pMsg.length = (WORD)length;

	const int total = (int)sizeof(pMsg) + length;

	pMsg.header.set(0xF4, 0x13, (WORD)total);

	memcpy(send, &pMsg, sizeof(pMsg));

	gSocketManager.DataSend(index, send, total);
}

// launcher.json y su firma, como los dejó mu-patch.  Se leen en cada pedido:
// son unos pocos KB y el launcher los pide una vez por arranque.
static bool ReadLauncherConfig(std::vector<BYTE>& out)
{
	const std::string path = GetDataPath() + "Patches/published/launcher.json";

	std::ifstream json(path, std::ios::binary);

	std::ifstream sig(path + ".sig", std::ios::binary);

	if (!json || !sig)
	{
		return false;
	}

	const std::string text((std::istreambuf_iterator<char>(json)), std::istreambuf_iterator<char>());

	const std::string sigText((std::istreambuf_iterator<char>(sig)), std::istreambuf_iterator<char>());

	// La firma va en hex en el .sig; acá viaja en binario.
	out.assign(64, 0);

	int digits = 0;

	for (char c : sigText)
	{
		int v = -1;

		if (c >= '0' && c <= '9') v = c - '0';
		else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
		else if (c == '\r' || c == '\n' || c == ' ') continue;
		else return false;

		if (digits >= 128)
		{
			return false;
		}

		out[digits / 2] = (BYTE)((out[digits / 2] << 4) | v);

		digits++;
	}

	if (digits != 128 || text.empty() || text.size() > LAUNCHER_CONFIG_MAX_SIZE)
	{
		return false;
	}

	out.insert(out.end(), text.begin(), text.end());

	return true;
}

void CCLauncherConfigRecv(PMSG_LAUNCHER_CONFIG_RECV* lpMsg, int size, int index)
{
	if (size < (int)sizeof(PMSG_LAUNCHER_CONFIG_RECV))
	{
		return;
	}

	BYTE send[sizeof(PMSG_LAUNCHER_CONFIG_SEND) + CONTENT_FILE_CHUNK];

	PMSG_LAUNCHER_CONFIG_SEND pMsg;

	memset(&pMsg, 0, sizeof(pMsg));

	pMsg.offset = lpMsg->offset;

	int length = 0;

	std::vector<BYTE> blob;

	if (gSecureLink.IsSecure(index) == false)
	{
		pMsg.result = CONTENT_NOT_SECURE;
	}
	else if (ReadLauncherConfig(blob) == false)
	{
		pMsg.result = CONTENT_NOT_FOUND;
	}
	else if (lpMsg->offset > blob.size())
	{
		pMsg.result = CONTENT_BAD_REQUEST;

		pMsg.size = (DWORD)blob.size();
	}
	else
	{
		pMsg.result = CONTENT_OK;

		pMsg.size = (DWORD)blob.size();

		length = (int)std::min<size_t>(blob.size() - lpMsg->offset, CONTENT_FILE_CHUNK);

		memcpy(&send[sizeof(pMsg)], blob.data() + lpMsg->offset, length);
	}

	pMsg.length = (WORD)length;

	const int total = (int)sizeof(pMsg) + length;

	pMsg.header.set(0xF4, 0x15, (WORD)total);

	memcpy(send, &pMsg, sizeof(pMsg));

	gSocketManager.DataSend(index, send, total);
}

// Se lee en cada pedido, como launcher.json: cambiar el .ini no pide reiniciar.
void CCClientPolicyRecv(PMSG_CLIENT_POLICY_RECV* lpMsg, int size, int index)
{
	(void)lpMsg;

	if (size < (int)sizeof(PMSG_CLIENT_POLICY_RECV))
	{
		return;
	}

	PMSG_CLIENT_POLICY_SEND pMsg;

	memset(&pMsg, 0, sizeof(pMsg));

	pMsg.header.set(0xF4, 0x17, sizeof(pMsg));

	// Sin canal cifrado no se pide nada: el cliente no podría saber si la orden
	// es del server.
	if (gSecureLink.IsSecure(index) == false)
	{
		pMsg.result = CONTENT_NOT_SECURE;
	}
	else
	{
		pMsg.result = CONTENT_OK;

#ifdef _WIN32
		const char* ini = ".\\ConnectServer.ini";
#else
		const char* ini = "./ConnectServer.ini";
#endif

		if (GetPrivateProfileInt("Launcher", "RequireLauncher", 0, ini) != 0)
		{
			pMsg.flags |= CLIENT_POLICY_REQUIRE_LAUNCHER;
		}
	}

	gSocketManager.DataSend(index, (BYTE*)&pMsg, pMsg.header.size);
}
