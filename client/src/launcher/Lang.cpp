#include "Lang.h"

#include <atomic>
#include <cstddef>
#include <unordered_map>

namespace Launcher {

namespace {

struct Entry
{
	const char* es;
	const char* en;
	const char* pt;
};

// Castellano, inglés y portugués.  Los textos que son iguales en los tres
// (nombres propios, números) no hace falta ponerlos.
const Entry kTable[] = {
	// Botones y pantalla principal.
	{ "Jugar", "Play", "Jogar" },
	{ "Opciones", "Options", "Opções" },
	{ "Cerrar", "Close", "Fechar" },
	{ "Pausar", "Pause", "Pausar" },
	{ "Seguir", "Resume", "Continuar" },
	{ "Guardar", "Save", "Salvar" },
	{ "Volver", "Back", "Voltar" },
	{ "Verificar archivos", "Verify files", "Verificar arquivos" },
	{ "Ir a la web", "Open website", "Abrir o site" },
	{ "Diseño: kayito", "Design: kayito", "Design: kayito" },
	{ "Opciones del juego", "Game options", "Opções do jogo" },
	{ "(ninguno)", "(none)", "(nenhum)" },
	{ "Vuelve a revisar todos los archivos instalados.", "Checks every installed file again.",
	  "Verifica de novo todos os arquivos instalados." },
	{ "El server no tiene sitio configurado.", "The server has no website configured.",
	  "O servidor não tem site configurado." },
	{ "En pausa. {0}", "Paused. {0}", "Em pausa. {0}" },
	{ "{0} (jugando: más despacio)", "{0} (playing: slower)", "{0} (jogando: mais devagar)" },
	{ "No se pudo abrir {0}: {1}", "Could not start {0}: {1}", "Não foi possível abrir {0}: {1}" },
	{ "No se pudo abrir {0} (error {1}).", "Could not start {0} (error {1}).", "Não foi possível abrir {0} (erro {1})." },
	{ "No está {0}.", "{0} is missing.", "{0} não foi encontrado." },
	{ "No se pudo abrir el navegador: {0}", "Could not open the browser: {0}", "Não foi possível abrir o navegador: {0}" },
	{ "No se pudo leer Config.ini.", "Could not read Config.ini.", "Não foi possível ler o Config.ini." },
	{ "El juego está abierto: sus opciones se ven pero se cambian al cerrarlo.",
	  "The game is running: its options can be viewed but only changed after closing it.",
	  "O jogo está aberto: as opções dele podem ser vistas, mas só mudam depois de fechá-lo." },
	{ "No se pudo guardar (¿el archivo es de sólo lectura?).", "Could not save (is the file read-only?).",
	  "Não foi possível salvar (o arquivo é somente leitura?)." },
	{ "Guardado. El juego lo usa la próxima vez que arranque.", "Saved. The game uses it the next time it starts.",
	  "Salvo. O jogo usa na próxima vez que iniciar." },
	{ "La versión nueva del launcher no arrancó; se volvió a la anterior.",
	  "The new launcher version did not start; the previous one was restored.",
	  "A nova versão do launcher não iniciou; a anterior foi restaurada." },
	{ "La versión nueva del launcher no arrancó y no se pudo volver a la anterior.",
	  "The new launcher version did not start and the previous one could not be restored.",
	  "A nova versão do launcher não iniciou e não foi possível restaurar a anterior." },

	// Pestañas y opciones.
	{ "General", "General", "Geral" },
	{ "Pantalla", "Display", "Tela" },
	{ "Gráficos", "Graphics", "Gráficos" },
	{ "Rendimiento", "Performance", "Desempenho" },
	{ "Sonido", "Sound", "Som" },
	{ "Texto", "Text", "Texto" },
	{ "Minimapa", "Minimap", "Minimapa" },
	{ "Descargas", "Downloads", "Downloads" },
	{ "Acerca de", "About", "Sobre" },
	{ "Mu Linux launcher, versión {0}", "Mu Linux launcher, version {0}", "Mu Linux launcher, versão {0}" },
	{ "Diseño de los temas: kayito (NicoMuratona), sobre los launchers originales de Webzen. Se usan con su permiso.",
	  "Theme design: kayito (NicoMuratona), based on the original Webzen launchers. Used with his permission.",
	  "Design dos temas: kayito (NicoMuratona), sobre os launchers originais da Webzen. Usados com a permissão dele." },
	{ "El launcher y el cliente son de Mu Linux, de código abierto (licencia MIT).",
	  "The launcher and the client are Mu Linux, open source (MIT license).",
	  "O launcher e o cliente são do Mu Linux, de código aberto (licença MIT)." },
	{ "Repositorio de kayito", "kayito's repository", "Repositório do kayito" },
	{ "Crear accesos directos", "Create shortcuts", "Criar atalhos" },
	{ "Accesos directos creados.", "Shortcuts created.", "Atalhos criados." },
	{ "No se pudieron crear los accesos directos: {0}", "Could not create the shortcuts: {0}",
	  "Não foi possível criar os atalhos: {0}" },
	{ "¿Crear un acceso directo al launcher?", "Create a shortcut to the launcher?", "Criar um atalho para o launcher?" },
	{ "En el escritorio y en el menú de inicio.", "On the desktop and in the Start menu.", "Na área de trabalho e no menu Iniciar." },
	{ "En el escritorio y en el menú de aplicaciones.", "On the desktop and in the applications menu.",
	  "Na área de trabalho e no menu de aplicativos." },
	{ "Windows no dejó crearlos", "Windows did not allow it", "o Windows não permitiu" },
	{ "falta la variable HOME", "the HOME variable is missing", "falta a variável HOME" },
	{ "no se pudo escribir en {0}", "could not write to {0}", "não foi possível gravar em {0}" },
	{ "Usuario guardado", "Saved username", "Usuário salvo" },
	{ "Idioma", "Language", "Idioma" },
	{ "Original (0.97k)", "Original (0.97k)", "Original (0.97k)" },
	{ "Tamaño del launcher", "Launcher size", "Tamanho do launcher" },
	{ "Automático", "Automatic", "Automático" },
	{ "No", "No", "Não" },
	{ "Sí", "Yes", "Sim" },
	{ "Modo ventana", "Windowed mode", "Modo janela" },
	{ "Sin bordes", "Borderless", "Sem bordas" },
	{ "Resolución", "Resolution", "Resolução" },
	{ "Sincronización vertical", "Vertical sync", "Sincronização vertical" },
	{ "Límite de FPS", "FPS limit", "Limite de FPS" },
	{ "Sin límite", "Unlimited", "Sem limite" },
	{ "Dibujo desacoplado", "Decoupled rendering", "Renderização desacoplada" },
	{ "Calidad de texturas", "Texture quality", "Qualidade das texturas" },
	{ "Mipmaps + bicúbico", "Mipmaps + bicubic", "Mipmaps + bicúbico" },
	{ "Texturas HD", "HD textures", "Texturas HD" },
	{ "Nitidez", "Sharpness", "Nitidez" },
	{ "Apagada", "Off", "Desligada" },
	{ "Apagado", "Off", "Desligado" },
	{ "Apagadas", "Off", "Desligadas" },
	{ "Baja", "Low", "Baixa" },
	{ "Media", "Medium", "Média" },
	{ "Alta", "High", "Alta" },
	{ "Sombras", "Shadows", "Sombras" },
	{ "Suaves", "Soft", "Suaves" },
	{ "Fuertes", "Strong", "Fortes" },
	{ "Suave", "Soft", "Suave" },
	{ "Fuerte", "Strong", "Forte" },
	{ "Niebla por profundidad", "Depth fog", "Neblina por profundidade" },
	{ "Distancia de dibujado", "Draw distance", "Distância de visão" },
	{ "x1,5", "x1.5", "x1,5" },
	{ "Luces dinámicas", "Dynamic lights", "Luzes dinâmicas" },
	{ "Conservadoras", "Conservative", "Conservadoras" },
	{ "Contrastadas", "High contrast", "Contrastadas" },
	{ "Cielo", "Sky", "Céu" },
	{ "Bajo el agua", "Underwater", "Debaixo d'água" },
	{ "Sin ondulación", "No ripple", "Sem ondulação" },
	{ "Completo", "Full", "Completo" },
	{ "Agua mejorada", "Improved water", "Água melhorada" },
	{ "Monturas como pet en zona segura", "Mounts as pets in safe zones", "Montarias como pet na zona segura" },
	{ "Quitar sombras", "Remove shadows", "Remover sombras" },
	{ "Quitar objetos", "Remove objects", "Remover objetos" },
	{ "Quitar piso", "Remove floor", "Remover chão" },
	{ "Quitar skills", "Remove skills", "Remover skills" },
	{ "Quitar efectos fijos", "Remove static effects", "Remover efeitos fixos" },
	{ "Quitar efectos móviles", "Remove moving effects", "Remover efeitos móveis" },
	{ "Quitar alas", "Remove wings", "Remover asas" },
	{ "Quitar barras de vida", "Remove health bars", "Remover barras de vida" },
	{ "Quitar interfaz", "Remove interface", "Remover interface" },
	{ "Quitar clima", "Remove weather", "Remover clima" },
	{ "Quitar brillo", "Remove glow", "Remover brilho" },
	{ "Efectos de sonido", "Sound effects", "Efeitos sonoros" },
	{ "Volumen de efectos", "Effects volume", "Volume dos efeitos" },
	{ "Música", "Music", "Música" },
	{ "Volumen de la música", "Music volume", "Volume da música" },
	{ "Sombra del texto", "Text shadow", "Sombra do texto" },
	{ "Contorno del texto", "Text outline", "Contorno do texto" },
	{ "Emoji en color", "Color emoji", "Emoji colorido" },
	{ "Opacidad", "Opacity", "Opacidade" },
	{ "Bajar la música", "Download music", "Baixar a música" },
	{ "Bajar texturas HD", "Download HD textures", "Baixar texturas HD" },
	{ "Conexiones", "Connections", "Conexões" },
	{ "Límite de velocidad", "Speed limit", "Limite de velocidade" },
	{ "Límite jugando", "Limit while playing", "Limite jogando" },

	// Actualización.
	{ "Configuración de prueba: {0}", "Test configuration: {0}", "Configuração de teste: {0}" },
	{ "No se pudo buscar actualizaciones ({0}). Podés jugar con lo instalado.",
	  "Could not check for updates ({0}). You can play with what is installed.",
	  "Não foi possível procurar atualizações ({0}). Você pode jogar com o que está instalado." },
	{ "Buscando la configuración del server...", "Getting the server configuration...", "Buscando a configuração do servidor..." },
	{ "Cancelado.", "Cancelled.", "Cancelado." },
	{ "cancelado", "cancelled", "cancelado" },
	{ "La clave de la base no es válida (64 dígitos hex).", "The base key is not valid (64 hex digits).",
	  "A chave da base não é válida (64 dígitos hex)." },
	{ "Falta la clave o el canal de los parches del server.", "The server patch key or channel is missing.",
	  "Falta a chave ou o canal dos patches do servidor." },
	{ "No hay nada configurado para actualizar.", "Nothing is configured to update.", "Não há nada configurado para atualizar." },
	{ "No hay de dónde bajar el cliente.", "There is nowhere to download the client from.",
	  "Não há de onde baixar o cliente." },
	{ "Verificando los archivos...", "Verifying files...", "Verificando os arquivos..." },
	{ "Buscando actualizaciones...", "Checking for updates...", "Procurando atualizações..." },
	{ "Hay una versión nueva del launcher que esta no sabe instalar: bajala del sitio del server.",
	  "There is a new launcher version this one cannot install: download it from the server website.",
	  "Há uma nova versão do launcher que esta não sabe instalar: baixe-a do site do servidor." },
	{ "No se pudo bajar {0}: {1}", "Could not download {0}: {1}", "Não foi possível baixar {0}: {1}" },
	{ "La versión nueva del launcher no arranca en este equipo; se sigue con la actual.",
	  "The new launcher version does not start on this computer; keeping the current one.",
	  "A nova versão do launcher não inicia neste computador; continua a atual." },
	{ "Actualizando el launcher...", "Updating the launcher...", "Atualizando o launcher..." },
	{ "No se pudo actualizar el launcher: {0}", "Could not update the launcher: {0}", "Não foi possível atualizar o launcher: {0}" },
	{ "No se pudo escribir en {0}", "Could not write to {0}", "Não foi possível gravar em {0}" },
	{ "Launcher actualizado. Reiniciando...", "Launcher updated. Restarting...", "Launcher atualizado. Reiniciando..." },
	{ "El cliente está al día.", "The client is up to date.", "O cliente está atualizado." },
	{ "Bajando {0} archivos ({1})...", "Downloading {0} files ({1})...", "Baixando {0} arquivos ({1})..." },
	{ "Bajando {0} de {1} archivos ({2} de {3})...", "Downloading {0} of {1} files ({2} of {3})...",
	  "Baixando {0} de {1} arquivos ({2} de {3})..." },
	{ "Ya podés jugar. Bajando {0} archivos ({1})...", "You can play now. Downloading {0} files ({1})...",
	  "Você já pode jogar. Baixando {0} arquivos ({1})..." },
	{ "Ya podés jugar. Bajando {0} de {1} archivos ({2} de {3})...",
	  "You can play now. Downloading {0} of {1} files ({2} of {3})...",
	  "Você já pode jogar. Baixando {0} de {1} arquivos ({2} de {3})..." },
	{ "{0} archivos se instalan cuando cierres el juego.", "{0} files will be installed when you close the game.",
	  "{0} arquivos serão instalados quando você fechar o jogo." },
	{ "Faltan {0} archivos: se instalan la próxima vez.", "{0} files are missing: they will be installed next time.",
	  "Faltam {0} arquivos: serão instalados na próxima vez." },
	{ "Instalando lo que faltaba...", "Installing what was missing...", "Instalando o que faltava..." },
	{ "No se pudieron instalar {0} archivos: {1}", "Could not install {0} files: {1}", "Não foi possível instalar {0} arquivos: {1}" },
	{ "Actualizado: {0} archivos.", "Updated: {0} files.", "Atualizado: {0} arquivos." },

	// Conexión con el ConnectServer y anuncio del server.
	{ "no se encuentra {0}", "{0} not found", "{0} não encontrado" },
	{ "no responde {0}", "{0} does not respond", "{0} não responde" },
	{ "no hay números aleatorios", "no random numbers available", "não há números aleatórios" },
	{ "{0} no contestó a tiempo", "{0} did not answer in time", "{0} não respondeu a tempo" },
	{ "se cortó la conexión con {0}", "the connection to {0} was lost", "a conexão com {0} caiu" },
	{ "{0} cerró la conexión", "{0} closed the connection", "{0} fechou a conexão" },
	{ "{0} no presentó la identidad del server", "{0} did not present the server identity",
	  "{0} não apresentou a identidade do servidor" },
	{ "{0} no usa el canal cifrado", "{0} does not use the encrypted channel", "{0} não usa o canal criptografado" },
	{ "el canal con {0} falló", "the channel with {0} failed", "o canal com {0} falhou" },
	{ "respuesta inválida de {0}", "invalid answer from {0}", "resposta inválida de {0}" },
	{ "{0} no publica launcher.json", "{0} does not publish launcher.json", "{0} não publica launcher.json" },
	{ "{0} rechazó el pedido ({1})", "{0} rejected the request ({1})", "{0} recusou o pedido ({1})" },
	{ "launcher.json cambió mientras se bajaba de {0}", "launcher.json changed while downloading it from {0}",
	  "launcher.json mudou enquanto era baixado de {0}" },
	{ "respuesta vacía", "empty answer", "resposta vazia" },
	{ "la firma no es del server", "the signature is not the server's", "a assinatura não é do servidor" },
	{ "{0}: configuración más vieja que la guardada", "{0}: configuration older than the saved one",
	  "{0}: configuração mais antiga que a salva" },
	{ "{0}: firma ilegible", "{0}: unreadable signature", "{0}: assinatura ilegível" },

	// launcher.toml.
	{ "línea {0} no se entiende", "line {0} is not understood", "a linha {0} não é entendida" },
	{ "clave o valor no válido en la línea {0} ({1})", "invalid key or value on line {0} ({1})",
	  "chave ou valor inválido na linha {0} ({1})" },
	{ "origen de la base: {0}", "base source: {0}", "origem da base: {0}" },
	{ "tag de la base: {0}", "base tag: {0}", "tag da base: {0}" },
	{ "clave de la base cambiada", "base key changed", "chave da base alterada" },
};

std::atomic<int> g_Language{ static_cast<int>(Language::Spanish) };

const std::unordered_map<std::string, const Entry*>& Index()
{
	static const std::unordered_map<std::string, const Entry*> index = [] {
		std::unordered_map<std::string, const Entry*> m;
		for (const Entry& e : kTable) m.emplace(e.es, &e);
		return m;
	}();
	return index;
}

const char* Lookup(const std::string& es)
{
	const Language l = CurrentLanguage();
	if (l == Language::Spanish) return nullptr;
	const auto it = Index().find(es);
	if (it == Index().end()) return nullptr;
	return l == Language::English ? it->second->en : it->second->pt;
}

} // namespace

Language LanguageFromSetting(const std::string& langSelection)
{
	if (langSelection == "Eng") return Language::English;
	if (langSelection == "Por") return Language::Portuguese;
	return Language::Spanish;
}

void SetLanguage(Language language)
{
	g_Language.store(static_cast<int>(language));
}

Language CurrentLanguage()
{
	return static_cast<Language>(g_Language.load());
}

const char* Tr(const char* es)
{
	const char* t = Lookup(es);
	return t ? t : es;
}

std::string Tr(const std::string& es)
{
	const char* t = Lookup(es);
	return t ? std::string(t) : es;
}

std::string Fmt(const char* es, std::initializer_list<std::string> args)
{
	const std::string pattern = Tr(es);
	std::string out;
	out.reserve(pattern.size() + 32);
	for (std::size_t i = 0; i < pattern.size(); ++i)
	{
		const std::size_t n = static_cast<std::size_t>(pattern[i + 1 < pattern.size() ? i + 1 : i] - '0');
		if (pattern[i] == '{' && i + 2 < pattern.size() && n < args.size() && pattern[i + 2] == '}')
		{
			out += *(args.begin() + n);
			i += 2;
		}
		else
			out += pattern[i];
	}
	return out;
}

bool HasTranslation(const std::string& es)
{
	return Index().count(es) != 0;
}

} // namespace Launcher
