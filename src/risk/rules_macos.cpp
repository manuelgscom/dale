// macOS additions to the rule catalog: destructive and system-changing macOS tools, the
// read-only macOS tools (so they are not reported as "unknown"), and the portability check
// that flags GNU/Linux-only commands and options when the target system is macOS.
#include <algorithm>
#include <initializer_list>
#include <functional>
#include <optional>

#include "risk/rules.hpp"

namespace dale::risk {
namespace {

using SV = std::string_view;
using R = Risk;

bool one_of(SV s, std::initializer_list<SV> l) { return std::find(l.begin(), l.end(), s) != l.end(); }
bool always(const Cmd&) { return true; }

// Exact argument text (before option bundling: "-read" is not "-r -e -a -d").
bool arg_is(const Cmd& c, SV text) {
    return std::any_of(c.args.begin(), c.args.end(), [&](const Word& w) { return w.text == text; });
}
bool any_arg(const Cmd& c, std::initializer_list<SV> l) {
    return std::any_of(l.begin(), l.end(), [&](SV t) { return arg_is(c, t); });
}
bool arg_starts(const Cmd& c, SV prefix) {
    return std::any_of(c.args.begin(), c.args.end(), [&](const Word& w) { return w.text.substr(0, prefix.size()) == prefix; });
}

// The first non-option argument (the verb of diskutil, launchctl, tmutil…).
std::string verb(const Cmd& c) { return c.op(0); }

bool diskutil_destroys(const Cmd& c) {
    const std::string v = verb(c);
    if (one_of(v, {"eraseDisk", "eraseVolume", "secureErase", "partitionDisk", "reformat", "zeroDisk", "randomDisk",
                   "erasedisk", "erasevolume", "secureerase", "partitiondisk", "zerodisk", "randomdisk"}))
        return true;
    if (v == "apfs" || v == "ap" || v == "coreStorage" || v == "cs") {
        const std::string sub = c.op(1);
        return one_of(sub, {"deleteContainer", "deleteVolume", "eraseVolume", "deletecontainer", "deletevolume",
                            "erasevolume", "delete", "deleteLVG", "deleteLV", "resizeContainer"});
    }
    return false;
}

bool diskutil_changes(const Cmd& c) { return one_of(verb(c), {"unmount", "mount", "mountDisk", "unmountDisk", "eject"}); }

bool disables_protection(const Cmd& c) {
    if (c.name == "csrutil") return one_of(verb(c), {"disable", "clear"}) || (verb(c) == "authenticated-root" && c.op(1) == "disable");
    if (c.name == "spctl") return any_arg(c, {"--master-disable", "--global-disable", "--disable"});
    if (c.name == "fdesetup") return verb(c) == "disable";
    return false;
}

bool tmutil_deletes(const Cmd& c) {
    return one_of(verb(c), {"delete", "deletelocalsnapshots", "removedestination", "thinlocalsnapshots", "deleteInProgress"});
}

bool account_destroy(const Cmd& c) {
    if (c.name == "sysadminctl") return any_arg(c, {"-deleteUser", "-resetPasswordFor", "-secureTokenOff", "-adminUser"}) && !any_arg(c, {"-secureTokenStatus"});
    // dscl . -delete /Users/x ; dscl . -passwd /Users/x ; with or without the leading dash
    return any_arg(c, {"-delete", "delete", "-passwd", "passwd", "-change", "change"});
}

bool account_change(const Cmd& c) {
    return any_arg(c, {"-create", "create", "-append", "append", "-merge", "merge", "-createprop", "-delete"}) ||
           c.name == "dseditgroup" || c.name == "sysadminctl";
}

bool launchctl_changes(const Cmd& c) {
    return !one_of(verb(c), {"list", "print", "print-cache", "print-disabled", "blame", "version", "help",
                             "managername", "manageruid", "manageruser", "error", "plist", "examine", "dumpstate"});
}

bool defaults_changes(const Cmd& c) { return one_of(verb(c), {"write", "delete", "import", "rename"}); }

bool networksetup_changes(const Cmd& c) {
    for (const Word& w : c.args) {
        const std::string& t = w.text;
        if (t.empty() || t[0] != '-') continue;
        if (t.rfind("-list", 0) == 0 || t.rfind("-get", 0) == 0 || t.rfind("-print", 0) == 0 || t.rfind("-show", 0) == 0 ||
            t == "-version" || t == "-help" || t == "-h" || t == "-listpreferredwirelessnetworks")
            return false;
        return true;  // -set*, -add*, -remove*, -create*, -delete*, -order*, -detect*, -switch*
    }
    return false;
}

bool pmset_changes(const Cmd& c) {
    for (const Word& w : c.args) {
        if (w.text == "-g" || w.text == "-G") return false;
    }
    return true;
}

bool nvram_changes(const Cmd& c) {
    if (c.has("-c") || c.has("-d") || c.has("-r") || c.has("-s")) return true;
    return std::any_of(c.operands.begin(), c.operands.end(), [](const Word& w) { return w.text.find('=') != std::string::npos; });
}

bool xattr_changes(const Cmd& c) { return c.has("-d") || c.has("-c") || c.has("-w"); }

bool hdiutil_changes(const Cmd& c) {
    return !one_of(verb(c), {"info", "imageinfo", "verify", "checksum", "isencrypted", "pmap", "plugins", "version"});
}

bool softwareupdate_changes(const Cmd& c) {
    return !(c.has("-l") || c.has("--list") || c.has("-h") || c.has("--history") || c.has("--help") ||
             c.has("--list-full-installers"));
}

bool security_changes(const Cmd& c) {
    const std::string v = verb(c);
    return !one_of(v, {"list-keychains", "default-keychain", "find-identity", "show-keychain-info", "help", "verify-cert",
                       "error", "login-keychain", "list"}) ||
           (v == "default-keychain" && c.has("-s"));
}

bool security_reads_secrets(const Cmd& c) {
    const std::string v = verb(c);
    return one_of(v, {"find-generic-password", "find-internet-password", "dump-keychain", "export", "find-certificate"});
}

}  // namespace

const std::vector<Rule>& macos_rules() {
    static const std::vector<Rule> t = {
        {"R8", {"srm", "newfs_apfs", "newfs_hfs", "newfs_msdos", "newfs_exfat", "newfs_udf", "newfs_ntfs"}, R::Peligro, always,
         false, "destruye datos de forma irreversible", "irreversibly destroys data"},
        {"R8", {"diskutil"}, R::Peligro, diskutil_destroys, false, "borra o reformatea un disco o volumen",
         "erases or reformats a disk or volume"},
        {"R8", {"asr"}, R::Peligro, [](const Cmd& c) { return c.has("--erase") || arg_is(c, "restore"); }, false,
         "borra el destino y lo sobrescribe con una imagen", "erases the target and overwrites it with an image"},
        {"M1", {"csrutil", "spctl", "fdesetup"}, R::Peligro, disables_protection, false,
         "desactiva una protección de seguridad de macOS", "turns off a macOS security protection"},
        {"M2", {"tmutil"}, R::Peligro, tmutil_deletes, false, "borra copias de seguridad de Time Machine",
         "deletes Time Machine backups"},
        {"M3", {"dscl", "sysadminctl"}, R::Peligro, account_destroy, false,
         "borra una cuenta de usuario o cambia su contraseña", "deletes a user account or changes its password"},
        {"M3b", {"dscl", "dseditgroup", "sysadminctl"}, R::Precaucion, account_change, false,
         "cambia cuentas de usuario o grupos", "changes user accounts or groups"},
        {"R26", {"diskutil"}, R::Precaucion, diskutil_changes, false, "cambia el estado del sistema", "changes the system state"},
        {"R26", {"launchctl"}, R::Precaucion, launchctl_changes, false, "cambia el estado del sistema", "changes the system state"},
        {"R26", {"defaults"}, R::Precaucion, defaults_changes, false, "cambia preferencias del sistema o de una aplicación",
         "changes system or application preferences"},
        {"R26", {"networksetup"}, R::Precaucion, networksetup_changes, false, "cambia la configuración de red",
         "changes the network configuration"},
        {"R26", {"scutil"}, R::Precaucion, [](const Cmd& c) { return c.has("--set") || arg_is(c, "--set"); }, false,
         "cambia el estado del sistema", "changes the system state"},
        {"R26", {"pmset"}, R::Precaucion, pmset_changes, false, "cambia la configuración de energía", "changes the power settings"},
        {"R26", {"nvram"}, R::Precaucion, nvram_changes, false, "cambia la NVRAM del equipo", "changes the machine's NVRAM"},
        {"R26", {"mdutil"}, R::Precaucion, [](const Cmd& c) { return !c.has("-s"); }, false, "cambia la indexación de Spotlight",
         "changes Spotlight indexing"},
        {"R26", {"xattr"}, R::Precaucion, xattr_changes, false, "modifica atributos extendidos de ficheros",
         "modifies extended file attributes"},
        {"R26", {"hdiutil"}, R::Precaucion, hdiutil_changes, false, "monta, crea o modifica imágenes de disco",
         "mounts, creates or modifies disk images"},
        {"R26", {"softwareupdate"}, R::Precaucion, softwareupdate_changes, false, "instala actualizaciones del sistema",
         "installs system updates"},
        {"R26", {"security"}, R::Precaucion, security_changes, false, "modifica el llavero o los certificados",
         "modifies the keychain or certificates"},
        {"R26", {"chflags", "csrutil", "kextload", "kextunload", "dscacheutil", "pfctl", "systemsetup"}, R::Precaucion,
         [](const Cmd& c) { return c.name != "csrutil" || verb(c) != "status"; }, false, "cambia el estado del sistema",
         "changes the system state"},
        {"M4", {"security"}, R::Precaucion, security_reads_secrets, false, "puede leer contraseñas o claves del llavero",
         "may read passwords or keys from the keychain"},
        {"M5", {"osascript"}, R::Precaucion, always, false, "ejecuta código AppleScript/JavaScript", "runs AppleScript/JavaScript code"},
        {"M6", {"open"}, R::Precaucion, always, false, "abre una aplicación, un fichero o una URL", "opens an application, file or URL"},
    };
    return t;
}

// ---- read-only macOS tools -----------------------------------------------------------------

bool macos_known(std::string_view n) {
    return one_of(n, {"sw_vers", "vm_stat", "system_profiler", "pbpaste", "pbcopy", "mdfind", "mdls", "otool", "ioreg",
                      "kextstat", "arch", "getconf", "lsbom", "hostinfo", "plutil", "lipo", "scutil", "diskutil",
                      "defaults", "launchctl", "networksetup", "pmset", "log", "tmutil", "softwareupdate", "xattr",
                      "pkgutil", "codesign", "spctl", "dscl", "csrutil", "sysctl", "mdutil", "hdiutil", "nvram",
                      "security", "osascript", "open", "brew", "sips", "say", "caffeinate", "ditto", "afinfo", "last",
                      "who", "w", "users", "arp", "route", "airport", "chflags", "dseditgroup", "sysadminctl"});
}

// -1: not a macOS tool handled here; 0: not read-only; 1: read-only.
int macos_read_only(const Cmd& c) {
    const std::string& n = c.name;
    if (one_of(n, {"sw_vers", "vm_stat", "system_profiler", "pbpaste", "pbcopy", "mdfind", "mdls", "otool", "ioreg",
                   "kextstat", "arch", "getconf", "lsbom", "hostinfo", "afinfo", "last", "who", "w", "users", "arp"}))
        return 1;
    if (n == "plutil")
        return (c.has("-p") || c.has("-lint") || c.has("-type") || arg_is(c, "-lint") || arg_is(c, "-type") || arg_is(c, "-p")) &&
               !any_arg(c, {"-convert", "-replace", "-insert", "-remove", "-create", "-o"});
    if (n == "lipo") return any_arg(c, {"-info", "-archs", "-detailed_info", "-verify_arch"});
    if (n == "scutil") return arg_is(c, "--get") || arg_is(c, "--dns") || arg_is(c, "--proxy") || arg_is(c, "--nwi") || arg_is(c, "-r") || arg_is(c, "--reach");
    if (n == "diskutil") return one_of(verb(c), {"list", "info", "activity", "listFilesystems"}) || (verb(c) == "apfs" && one_of(c.op(1), {"list", "listUsers", "listCryptoUsers", "listSnapshots", "listVolumeGroups"}));
    if (n == "defaults") return one_of(verb(c), {"read", "read-type", "domains", "find", "help"});
    if (n == "launchctl") return !launchctl_changes(c);
    if (n == "networksetup") return !networksetup_changes(c);
    if (n == "pmset") return !pmset_changes(c);
    if (n == "log") return one_of(verb(c), {"show", "stream", "stats", "help"});
    if (n == "tmutil") return one_of(verb(c), {"listbackups", "latestbackup", "status", "destinationinfo", "machinedirectory", "listlocalsnapshots", "listlocalsnapshotdates", "isexcluded", "version", "calculatedrift"});
    if (n == "softwareupdate") return !softwareupdate_changes(c);
    if (n == "xattr") return !xattr_changes(c);
    if (n == "pkgutil")
        return any_arg(c, {"--pkgs", "--packages", "--files", "--pkg-info", "--pkg-info-plist", "--file-info", "--file-info-plist", "--pkg-groups", "--check-signature", "--payload-files", "--export-plist"}) &&
               !any_arg(c, {"--forget", "--unlink", "--expand", "--expand-full", "--flatten", "--flatten-full", "--bom"});
    if (n == "codesign") return (c.has("-d") || c.has("-v") || arg_is(c, "--display") || arg_is(c, "--verify")) && !(c.has("-s") || arg_is(c, "--sign") || arg_is(c, "--remove-signature"));
    if (n == "spctl") return arg_is(c, "--status") || arg_is(c, "--assess") || arg_is(c, "-a") || arg_is(c, "--list");
    if (n == "dscl") return (any_arg(c, {"-read", "read", "-list", "list", "-search", "search", "-readall", "readall", "-readpl", "-readpli"})) && !account_destroy(c) && !any_arg(c, {"-create", "create", "-append", "append", "-merge", "merge", "-createprop"});
    if (n == "csrutil") return verb(c) == "status";
    if (n == "sysctl") return !(c.has("-w") || c.has("-f") || c.has("-p")) && std::none_of(c.operands.begin(), c.operands.end(), [](const Word& w) { return w.text.find('=') != std::string::npos; });
    if (n == "mdutil") return c.has("-s");
    if (n == "hdiutil") return !hdiutil_changes(c);
    if (n == "security") return !security_changes(c) && !security_reads_secrets(c);
    if (n == "brew")
        return one_of(verb(c), {"list", "ls", "info", "search", "outdated", "deps", "uses", "leaves", "desc", "config", "doctor", "--version", "-v", "--prefix", "--cellar", "--repository", "--cache", "commands", "formulae", "casks", "missing", "log"}) ||
               c.has("--version") || c.has("--prefix") || c.has("--cellar");
    return -1;
}

// ---- portability (GNU/Linux on a macOS system) -----------------------------------------------

namespace {

struct Missing {
    SV name;
    SV hint_es, hint_en;
};

// Not present on a stock macOS. (When the user has installed them, e.g. GNU coreutils with
// Homebrew, has_tool tells and nothing is reported.)
const Missing kMissing[] = {
    {"ip", "usa ifconfig o networksetup", "use ifconfig or networksetup"},
    {"ss", "usa netstat o lsof -i", "use netstat or lsof -i"},
    {"lsblk", "usa diskutil list", "use diskutil list"},
    {"lscpu", "usa sysctl -n machdep.cpu.brand_string", "use sysctl -n machdep.cpu.brand_string"},
    {"free", "usa vm_stat o memory_pressure", "use vm_stat or memory_pressure"},
    {"lsusb", "usa system_profiler SPUSBDataType", "use system_profiler SPUSBDataType"},
    {"lspci", "usa system_profiler SPPCIDataType", "use system_profiler SPPCIDataType"},
    {"lsmod", "usa kextstat", "use kextstat"},
    {"ldd", "usa otool -L", "use otool -L"},
    {"nproc", "usa sysctl -n hw.ncpu", "use sysctl -n hw.ncpu"},
    {"md5sum", "usa md5", "use md5"},
    {"sha1sum", "usa shasum", "use shasum"},
    {"sha256sum", "usa shasum -a 256", "use shasum -a 256"},
    {"sha512sum", "usa shasum -a 512", "use shasum -a 512"},
    {"tac", "usa tail -r", "use tail -r"},
    {"shuf", "usa sort -R o gshuf", "use sort -R or gshuf"},
    {"timeout", "instala coreutils y usa gtimeout", "install coreutils and use gtimeout"},
    {"truncate", "instala coreutils (gtruncate)", "install coreutils (gtruncate)"},
    {"xclip", "usa pbcopy / pbpaste", "use pbcopy / pbpaste"},
    {"xsel", "usa pbcopy / pbpaste", "use pbcopy / pbpaste"},
    {"wl-copy", "usa pbcopy", "use pbcopy"},
    {"wl-paste", "usa pbpaste", "use pbpaste"},
    {"xdg-open", "usa open", "use open"},
    {"notify-send", "usa osascript -e 'display notification …'", "use osascript -e 'display notification …'"},
    {"systemctl", "usa launchctl o brew services", "use launchctl or brew services"},
    {"journalctl", "usa log show", "use log show"},
    {"service", "usa launchctl o brew services", "use launchctl or brew services"},
    {"hostnamectl", "usa scutil --get ComputerName o sw_vers", "use scutil --get ComputerName or sw_vers"},
    {"timedatectl", "usa systemsetup -gettimezone o date", "use systemsetup -gettimezone or date"},
    {"loginctl", "usa who o last", "use who or last"},
    {"apt", "usa brew", "use brew"},
    {"apt-get", "usa brew", "use brew"},
    {"apt-cache", "usa brew search", "use brew search"},
    {"dpkg", "usa brew list", "use brew list"},
    {"dnf", "usa brew", "use brew"},
    {"yum", "usa brew", "use brew"},
    {"rpm", "usa brew", "use brew"},
    {"pacman", "usa brew", "use brew"},
    {"zypper", "usa brew", "use brew"},
    {"snap", "usa brew", "use brew"},
    {"ufw", "usa pfctl", "use pfctl"},
    {"iptables", "usa pfctl", "use pfctl"},
    {"nmcli", "usa networksetup", "use networksetup"},
    {"lsb_release", "usa sw_vers", "use sw_vers"},
    {"useradd", "usa sysadminctl o dscl", "use sysadminctl or dscl"},
    {"userdel", "usa sysadminctl o dscl", "use sysadminctl or dscl"},
    {"watch", "instala watch con brew", "install watch with brew"},
};

// sed -i on macOS (BSD sed) takes the backup suffix as the NEXT argument: GNU-style
// "sed -i 's/a/b/' f" makes it read the script as the suffix and fails (or leaves "f-e" backups).
bool bsd_sed_inplace(const Cmd& c) {
    for (std::size_t i = 0; i < c.args.size(); ++i) {
        const std::string& t = c.args[i].text;
        if (t == "--in-place" || t.rfind("--in-place=", 0) == 0) return true;
        if (t == "-i") {
            if (i + 1 >= c.args.size()) return true;
            const std::string& next = c.args[i + 1].text;
            return !(next.empty() || next[0] == '.');
        }
    }
    return false;
}

struct Flagged {
    SV cmd;
    bool (*match)(const Cmd&);
    SV why_es, why_en;
};

const Flagged kFlagged[] = {
    {"sed", bsd_sed_inplace,
     "el sed de macOS (BSD) necesita un sufijo tras -i: usa sed -i '' 's/a/b/' fichero",
     "macOS sed (BSD) needs a suffix after -i: use sed -i '' 's/a/b/' file"},
    {"sed", [](const Cmd& c) { return c.has("-r"); },
     "el sed de macOS no admite -r: usa -E", "macOS sed does not accept -r: use -E"},
    {"ps", [](const Cmd& c) { return arg_starts(c, "--sort") || arg_is(c, "--forest") || arg_is(c, "--no-headers") || c.has("-C"); },
     "el ps de macOS (BSD) no admite --sort, --forest ni -C: usa ps aux -m o ps -Ao pid,%mem,comm -m",
     "macOS ps (BSD) has no --sort, --forest or -C: use ps aux -m or ps -Ao pid,%mem,comm -m"},
    {"stat", [](const Cmd& c) { return c.has("-c") || arg_starts(c, "--format") || arg_is(c, "--printf"); },
     "el stat de macOS no admite -c: usa stat -f", "macOS stat has no -c: use stat -f"},
    {"date", [](const Cmd& c) { return c.has("-d") || arg_starts(c, "--date") || c.has("-I") || arg_starts(c, "--iso"); },
     "el date de macOS no admite -d ni --date: usa date -v o date -j -f", "macOS date has no -d or --date: use date -v or date -j -f"},
    {"ls", [](const Cmd& c) { return arg_starts(c, "--color") || arg_starts(c, "--group-directories") || arg_starts(c, "--time-style") || arg_is(c, "--human-readable") || arg_is(c, "--almost-all") || arg_is(c, "--sort"); },
     "el ls de macOS no admite opciones largas de GNU: usa -G, -lh, -A", "macOS ls does not accept GNU long options: use -G, -lh, -A"},
    {"du", [](const Cmd& c) { return arg_starts(c, "--max-depth") || arg_is(c, "--apparent-size") || arg_starts(c, "--exclude") || arg_is(c, "--human-readable") || arg_is(c, "--summarize") || c.has("-b"); },
     "el du de macOS no admite --max-depth ni opciones largas: usa -d N, -h, -s", "macOS du has no --max-depth or long options: use -d N, -h, -s"},
    {"df", [](const Cmd& c) { return arg_starts(c, "--output") || arg_is(c, "--total") || arg_is(c, "--human-readable") || arg_starts(c, "--type"); },
     "el df de macOS no admite --output, --total ni -t TIPO: usa df -h", "macOS df has no --output, --total or -t TYPE: use df -h"},
    {"grep", [](const Cmd& c) { return c.has("-P") || arg_is(c, "--perl-regexp"); },
     "el grep de macOS no admite -P: usa -E o instala ggrep", "macOS grep has no -P: use -E or install ggrep"},
    {"find", [](const Cmd& c) { return arg_is(c, "-printf") || arg_is(c, "-regextype") || arg_is(c, "-readable") || arg_is(c, "-writable"); },
     "el find de macOS no admite -printf ni -regextype: usa -exec stat -f o -print", "macOS find has no -printf or -regextype: use -exec stat -f or -print"},
    {"xargs", [](const Cmd& c) { return c.has("-d") || arg_starts(c, "--delimiter") || arg_is(c, "--no-run-if-empty") || arg_is(c, "--max-procs"); },
     "el xargs de macOS no admite -d ni opciones largas: usa -0 o -n", "macOS xargs has no -d or long options: use -0 or -n"},
    {"base64", [](const Cmd& c) { return c.has("-w") || arg_starts(c, "--wrap"); },
     "el base64 de macOS no admite -w: sin envolver ya es una línea con -b 0", "macOS base64 has no -w: use -b 0 for one line"},
    {"readlink", [](const Cmd& c) { return c.has("-m") || arg_is(c, "--canonicalize-missing"); },
     "el readlink de macOS no admite -m", "macOS readlink has no -m"},
    {"head", [](const Cmd& c) { for (const Word& w : c.args) if (w.text.size() > 2 && w.text[0] == '-' && (w.text[1] == 'n' || w.text[1] == 'c') && w.text[2] == '-') return true; for (std::size_t i = 0; i + 1 < c.args.size(); ++i) if ((c.args[i].text == "-n" || c.args[i].text == "-c") && !c.args[i + 1].text.empty() && c.args[i + 1].text[0] == '-') return true; return false; },
     "el head de macOS no admite un número negativo: usa sed '$d' o tail -r", "macOS head does not accept a negative count: use sed '$d' or tail -r"},
    {"cp", [](const Cmd& c) { return arg_is(c, "--parents") || arg_is(c, "--reflink") || arg_starts(c, "--target-directory") || c.has("-t"); },
     "el cp de macOS no admite --parents, --reflink ni -t", "macOS cp has no --parents, --reflink or -t"},
    {"mv", [](const Cmd& c) { return c.has("-t") || arg_starts(c, "--target-directory"); },
     "el mv de macOS no admite -t", "macOS mv has no -t"},
    {"netstat", [](const Cmd& c) { return c.has("-p") || c.has("-t") || c.has("-u") || c.has("-l"); },
     "el netstat de macOS no admite -tulpn: usa lsof -i -P -n | grep LISTEN", "macOS netstat has no -tulpn: use lsof -i -P -n | grep LISTEN"},
    {"top", [](const Cmd& c) { return c.has("-b") || c.has("-H"); },
     "el top de macOS no admite -b: usa top -l 1", "macOS top has no -b: use top -l 1"},
    {"tar", [](const Cmd& c) { return arg_is(c, "--wildcards") || arg_is(c, "--transform") || arg_starts(c, "--one-top-level"); },
     "el tar de macOS (bsdtar) no admite esta opción de GNU tar", "macOS tar (bsdtar) does not support this GNU tar option"},
    {"mktemp", [](const Cmd& c) { return arg_starts(c, "--suffix") || arg_starts(c, "--tmpdir"); },
     "el mktemp de macOS no admite --suffix ni --tmpdir", "macOS mktemp has no --suffix or --tmpdir"},
    {"chown", [](const Cmd& c) { return arg_starts(c, "--reference") || arg_is(c, "--from"); },
     "el chown de macOS no admite --reference", "macOS chown has no --reference"},
    {"sort", [](const Cmd& c) { return arg_is(c, "--parallel"); },
     "el sort de macOS no admite --parallel", "macOS sort has no --parallel"},
    {"cat", [](const Cmd& c) { return c.has("-A"); }, "el cat de macOS no admite -A: usa cat -e -t", "macOS cat has no -A: use cat -e -t"},
    {"hostname", [](const Cmd& c) { return c.has("-i") || c.has("-I") || c.has("-f") || arg_is(c, "--fqdn") || arg_is(c, "--all-ip-addresses"); },
     "el hostname de macOS no admite -i, -I ni -f: usa ipconfig getifaddr en0", "macOS hostname has no -i, -I or -f: use ipconfig getifaddr en0"},
    {"rm", [](const Cmd& c) { return arg_is(c, "--preserve-root") || arg_is(c, "--one-file-system") || arg_is(c, "--no-preserve-root"); },
     "el rm de macOS no admite esta opción de GNU", "macOS rm does not support this GNU option"},
};

}  // namespace

std::optional<std::pair<std::string, std::string>> macos_incompat(const Cmd& c,
                                                                   const std::function<bool(std::string_view)>& has_tool) {
    for (const Missing& m : kMissing) {
        if (c.name != m.name) continue;
        if (has_tool && has_tool(c.name)) return std::nullopt;
        return std::make_pair("no existe en macOS: " + std::string(m.hint_es), "does not exist on macOS: " + std::string(m.hint_en));
    }
    // Linux-only files: they do not exist on macOS (and read as "no such file").
    for (const std::string& p : c.paths) {
        if (p == "/etc/os-release" || p == "/usr/lib/os-release" || p == "/etc/lsb-release" || p == "/etc/debian_version" ||
            p == "/etc/redhat-release")
            return std::make_pair(std::string("no existe en macOS: usa sw_vers"), std::string("does not exist on macOS: use sw_vers"));
        if (p == "/proc" || p.rfind("/proc/", 0) == 0)
            return std::make_pair(std::string("/proc no existe en macOS: usa sysctl, vm_stat o ps"),
                                  std::string("/proc does not exist on macOS: use sysctl, vm_stat or ps"));
        if (p == "/sys" || p.rfind("/sys/", 0) == 0)
            return std::make_pair(std::string("/sys no existe en macOS: usa sysctl o system_profiler"),
                                  std::string("/sys does not exist on macOS: use sysctl or system_profiler"));
    }
    for (const Flagged& f : kFlagged) {
        if (c.name == f.cmd && f.match(c)) return std::make_pair(std::string(f.why_es), std::string(f.why_en));
    }
    return std::nullopt;
}

}  // namespace dale::risk
