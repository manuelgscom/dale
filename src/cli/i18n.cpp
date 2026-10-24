#include "cli/i18n.hpp"

#include <cstdlib>

namespace dale::cli {

namespace {

struct Entry {
    Msg msg;
    const char* es;
    const char* en;
};

// One row per message, in the same order as the enum.
constexpr Entry kTable[] = {
    {Msg::Help,
     "Uso: dale [opciones] <petición…>\n"
     "\n"
     "Convierte una petición en lenguaje natural en un comando bash, lo explica,\n"
     "evalúa su riesgo y solo lo ejecuta si lo confirmas.\n"
     "\n"
     "Opciones:\n"
     "  -h, --help     muestra esta ayuda\n"
     "  --version      muestra la versión\n"
     "  --model RUTA   usa este fichero GGUF en lugar del modelo descargado\n"
     "\n"
     "Respuestas: s = ejecutar, n = no, e = editar, c = copiar al portapapeles.\n"
     "Configuración: ~/.config/dale/config, con «mode = suggest|confirm|auto»\n"
     "y «lang = auto|es|en».\n"
     "En tmux, copiar al portapapeles necesita «set-clipboard on».\n"
     "\n"
     "Opciones internas para evaluación:\n"
     "  --json           escribe el resultado en una línea JSON; no pregunta ni ejecuta\n"
     "  --print-prompt   escribe el prompt exacto del modelo, sin cargarlo\n"
     "  DALE_CONTEXT_FILE=RUTA   toma la línea de contexto de la primera línea del fichero\n",
     "Usage: dale [options] <request…>\n"
     "\n"
     "Turns a natural language request into a bash command, explains it,\n"
     "assesses its risk and only runs it if you confirm.\n"
     "\n"
     "Options:\n"
     "  -h, --help     show this help\n"
     "  --version      show the version\n"
     "  --model PATH   use this GGUF file instead of the downloaded model\n"
     "\n"
     "Answers: y = run, n = no, e = edit, c = copy to clipboard.\n"
     "Configuration: ~/.config/dale/config, with \"mode = suggest|confirm|auto\"\n"
     "and \"lang = auto|es|en\".\n"
     "In tmux, copying to the clipboard needs \"set-clipboard on\".\n"
     "\n"
     "Internal options for evaluation:\n"
     "  --json           write the result as one JSON line; never asks or runs\n"
     "  --print-prompt   write the exact model prompt, without loading it\n"
     "  DALE_CONTEXT_FILE=PATH   take the context line from the first line of the file\n"},
    {Msg::Version, "dale {} (modelo {}, {}, sha256 {})\n", "dale {} (model {}, {}, sha256 {})\n"},
    {Msg::UnknownFlag, "dale: opción desconocida: ", "dale: unknown option: "},
    {Msg::MissingValue, "dale: falta el valor de la opción: ", "dale: missing value for option: "},
    {Msg::RequestTooLong, "dale: petición demasiado larga (máximo {} bytes)",
     "dale: request too long (at most {} bytes)"},
    {Msg::ContextUnreadable, "dale: no se puede leer DALE_CONTEXT_FILE: ", "dale: cannot read DALE_CONTEXT_FILE: "},
    {Msg::ContextInvalid,
     "dale: la primera línea de DALE_CONTEXT_FILE no tiene el formato «os=… distro=… pm=… tools=…»: ",
     "dale: the first line of DALE_CONTEXT_FILE is not \"os=… distro=… pm=… tools=…\": "},
    {Msg::EngineError, "dale: error del motor: ", "dale: engine error: "},
    {Msg::InvalidResponse, "dale: no he podido generar una respuesta válida",
     "dale: I could not generate a valid response"},
    {Msg::Incomplete, " (respuesta incompleta)", " (incomplete response)"},
    {Msg::Timeout, "dale: ha tardado demasiado; prueba a reformular la petición",
     "dale: it took too long; try rephrasing the request"},
    {Msg::LoadFailed, "dale: no se ha podido cargar el modelo: ", "dale: could not load the model: "},
    {Msg::ModelMissing, "dale: no existe el fichero del modelo: ", "dale: model file not found: "},
    {Msg::NoModelsDir, "dale: no sé dónde guardar el modelo: define HOME o XDG_DATA_HOME, o usa «--model RUTA»",
     "dale: don't know where to store the model: set HOME or XDG_DATA_HOME, or use \"--model PATH\""},
    {Msg::DownloadOffer,
     "dale necesita descargar el modelo (solo la primera vez):\n"
     "  fichero: {}\n"
     "  tamaño:  {} MB\n"
     "  origen:  {}\n",
     "dale needs to download the model (only the first time):\n"
     "  file:   {}\n"
     "  size:   {} MB\n"
     "  source: {}\n"},
    {Msg::DownloadPrompt, "¿Descargarlo ahora? [S/n] ", "Download it now? [Y/n] "},
    {Msg::DownloadDeclined,
     "dale: no se ha descargado el modelo. Puedes descargarlo tú y usar «dale --model RUTA …».",
     "dale: the model was not downloaded. You can download it yourself and use \"dale --model PATH …\"."},
    {Msg::DownloadNoTty,
     "dale: falta el modelo {} ({} MB). Ejecuta dale en un terminal (sin --json) para descargarlo,\n"
     "o descárgalo de {} y usa «dale --model RUTA …».",
     "dale: the model {} ({} MB) is missing. Run dale in a terminal (without --json) to download it,\n"
     "or download it from {} and use \"dale --model PATH …\"."},
    {Msg::DownloadDone, "Modelo descargado y verificado: ", "Model downloaded and verified: "},
    {Msg::Refuse, "Esto no se resuelve con un comando: ", "This can't be solved with a command: "},
    {Msg::Unsure, "No estoy seguro: ", "I'm not sure: "},
    {Msg::LevelSafe, "seguro", "safe"},
    {Msg::LevelCaution, "precaución", "caution"},
    {Msg::DangerBanner, "███ PELIGRO ███", "███ DANGER ███"},
    {Msg::Irreversible, "Esta acción puede ser irreversible.", "This action may be irreversible."},
    {Msg::PromptSafe, "¿Ejecutar? [S/n/e(ditar)/c(opiar)] ", "Run it? [Y/n/e(dit)/c(opy)] "},
    {Msg::PromptCaution, "¿Ejecutar? [s/N/e(ditar)/c(opiar)] ", "Run it? [y/N/e(dit)/c(opy)] "},
    {Msg::PromptDanger, "Para ejecutarlo escribe «ejecutar». e = editar, c = copiar, Intro = cancelar: ",
     "To run it, type \"run\". e = edit, c = copy, Enter = cancel: "},
    {Msg::DangerWord, "ejecutar", "run"},
    {Msg::MoreReason, "y 1 motivo más", "and 1 more reason"},
    {Msg::MoreReasons, "y {} motivos más", "and {} more reasons"},
    {Msg::Cancelled, "Cancelado", "Cancelled"},
    {Msg::Copied, "Copiado al portapapeles", "Copied to clipboard"},
    {Msg::CopyFailed, "No se ha podido copiar al portapapeles", "Could not copy to the clipboard"},
    {Msg::EditedExplanation, "(comando editado)", "(edited command)"},
};

constexpr bool table_in_order() {
    constexpr std::size_t n = sizeof(kTable) / sizeof(kTable[0]);
    if (n != static_cast<std::size_t>(Msg::Count_)) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (static_cast<std::size_t>(kTable[i].msg) != i) return false;
    return true;
}
static_assert(table_in_order(), "la tabla i18n no tiene todos los textos en el orden del enum");

}  // namespace

std::string_view tr(UiLang lang, Msg msg) {
    const Entry& e = kTable[static_cast<std::size_t>(msg)];
    return lang == UiLang::Es ? e.es : e.en;
}

std::string fill(std::string_view pattern, std::string_view value) {
    std::string out(pattern);
    const auto pos = out.find("{}");
    if (pos != std::string::npos) out.replace(pos, 2, value);
    return out;
}

std::string fill_all(std::string_view pattern, std::initializer_list<std::string_view> values) {
    std::string out;
    auto next = values.begin();
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{' && i + 1 < pattern.size() && pattern[i + 1] == '}') {
            if (next != values.end()) out += *next++;
            ++i;
        } else {
            out += pattern[i];
        }
    }
    return out;
}

UiLang ui_lang(config::Lang lang, const char* lc_all, const char* lc_messages, const char* lang_env) {
    if (lang == config::Lang::Es) return UiLang::Es;
    if (lang == config::Lang::En) return UiLang::En;
    for (const char* v : {lc_all, lc_messages, lang_env}) {
        if (v == nullptr || *v == '\0') continue;
        return std::string_view(v).substr(0, 2) == "es" ? UiLang::Es : UiLang::En;
    }
    return UiLang::En;
}

UiLang ui_lang_from_env(config::Lang lang) {
    return ui_lang(lang, std::getenv("LC_ALL"), std::getenv("LC_MESSAGES"), std::getenv("LANG"));
}

}  // namespace dale::cli
