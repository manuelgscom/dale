#include <string>
#include <vector>

#include "doctest.h"
#include "risk/lexer.hpp"
#include "risk/paths.hpp"
#include "risk/risk.hpp"

using namespace dale::risk;

namespace {

std::vector<std::string> texts(const SimpleCommand& c) {
    std::vector<std::string> out;
    for (const auto& w : c.words) out.push_back(w.text);
    return out;
}

using V = std::vector<std::string>;

// "level:ids" with the ids of the final level, without duplicates and in order (like dale_risk).
std::string lv(const std::string& cmd) {
    Assessment a = classify(cmd);
    std::string ids;
    for (const auto& f : a.findings) {
        if (f.level != a.level) continue;
        if (("," + ids + ",").find("," + f.rule_id + ",") != std::string::npos) continue;
        if (!ids.empty()) ids += ',';
        ids += f.rule_id;
    }
    return std::string(to_string(a.level)) + ":" + ids;
}

std::string level(const std::string& cmd) { return std::string(to_string(classify(cmd).level)); }

}  // namespace

TEST_SUITE("risk") {

// ---------------------------------------------------------------- lexer

TEST_CASE("lexer: palabras simples y blancos") {
    Script s = parse("  ls   -la\t/tmp  ");
    REQUIRE(s.commands.size() == 1);
    CHECK(texts(s.commands[0]) == V{"ls", "-la", "/tmp"});
    CHECK(s.errors.empty());
}

TEST_CASE("lexer: comillas simples y dobles") {
    Script s = parse("echo 'a $b \"c\"' \"x $y `z` \\\" \\n\"");
    REQUIRE(s.commands.size() == 1);
    const auto& w = s.commands[0].words;
    REQUIRE(w.size() == 3);
    CHECK(w[1].text == "a $b \"c\"");
    CHECK_FALSE(w[1].dynamic);
    CHECK(w[1].quoted);
    CHECK(w[2].text == "x $y `z` \" \\n");
    CHECK(w[2].dynamic);
    REQUIRE(w[2].subs.size() == 1);
    CHECK(w[2].subs[0] == "z");
}

TEST_CASE("lexer: $'...' con escapes ANSI-C") {
    Script s = parse("$'\\x72\\x6d' $'a\\tb\\n' $'\\101\\u00e9' $'it\\'s'");
    REQUIRE(s.commands.size() == 1);
    const auto& w = s.commands[0].words;
    REQUIRE(w.size() == 4);
    CHECK(w[0].text == "rm");
    CHECK_FALSE(w[0].dynamic);
    CHECK(w[1].text == "a\tb\n");
    CHECK(w[2].text == "A\xC3\xA9");
    CHECK(w[3].text == "it's");
}

TEST_CASE("lexer: concatenaciones r''m y \"r\"m") {
    Script s = parse("r''m \"r\"m 'r'\"m\" r\\m");
    REQUIRE(s.commands.size() == 1);
    CHECK(texts(s.commands[0]) == V{"rm", "rm", "rm", "rm"});
}

TEST_CASE("lexer: escapes con barra") {
    Script s = parse("\\rm a\\ b c\\\nd");
    REQUIRE(s.commands.size() == 1);
    CHECK(texts(s.commands[0]) == V{"rm", "a b", "cd"});
    CHECK(s.commands[0].words[0].quoted);
}

TEST_CASE("lexer: comentarios") {
    Script s = parse("ls # rm -rf /\necho a#b");
    REQUIRE(s.commands.size() == 2);
    CHECK(texts(s.commands[0]) == V{"ls"});
    CHECK(texts(s.commands[1]) == V{"echo", "a#b"});
    CHECK(parse("# solo comentario").commands.empty());
}

TEST_CASE("lexer: separadores") {
    Script s = parse("a; b\nc & d && e || f | g |& h");
    REQUIRE(s.commands.size() == 8);
    CHECK(s.commands[2].background);
    CHECK_FALSE(s.commands[3].background);
    CHECK(s.commands[6].in_pipe);
    CHECK(s.commands[7].upstream == std::vector<std::size_t>{5, 6});
    CHECK(s.commands[6].upstream == std::vector<std::size_t>{5});
    CHECK(s.commands[5].upstream.empty());
    CHECK(s.errors.empty());
}

TEST_CASE("lexer: agrupaciones se aplanan") {
    Script s = parse("(cd /tmp && ls) ; { echo a; echo b; }");
    REQUIRE(s.commands.size() == 4);
    CHECK(texts(s.commands[0]) == V{"cd", "/tmp"});
    CHECK(texts(s.commands[3]) == V{"echo", "b"});
    CHECK(s.errors.empty());

    Script p = parse("curl x | { cat; bash; }");
    REQUIRE(p.commands.size() == 3);
    CHECK(p.commands[2].upstream == std::vector<std::size_t>{0});

    CHECK_FALSE(parse("(ls").errors.empty());
    CHECK_FALSE(parse("ls)").errors.empty());
    CHECK_FALSE(parse("{ ls }").errors.empty());
}

TEST_CASE("lexer: redirecciones") {
    Script s = parse("cmd >a >>b >|c <d <>e &>f &>>g 2>h 2>&1 >&2 <&0 <<<'hola' 3<i");
    REQUIRE(s.commands.size() == 1);
    const auto& r = s.commands[0].redirs;
    REQUIRE(r.size() == 13);
    V ops;
    for (const auto& x : r) ops.push_back(x.op);
    CHECK(ops == V{">", ">>", ">|", "<", "<>", "&>", "&>>", ">", ">&", ">&", "<&", "<<<", "<"});
    CHECK(r[7].fd == 2);
    CHECK(r[8].fd == 2);
    CHECK(r[8].target.text == "1");
    CHECK(r[11].target.text == "hola");
    CHECK(r[12].fd == 3);
    CHECK(texts(s.commands[0]) == V{"cmd"});
    CHECK_FALSE(parse("ls >").errors.empty());
}

TEST_CASE("lexer: heredocs") {
    Script s = parse("cat <<EOF\nlinea $(date)\nEOF\necho fin");
    REQUIRE(s.commands.size() == 2);
    REQUIRE(s.commands[0].redirs.size() == 1);
    CHECK(s.commands[0].redirs[0].body == "linea $(date)\n");
    CHECK(s.commands[0].redirs[0].body_expands);
    REQUIRE(s.loose.size() == 1);
    CHECK(s.loose[0].subs == V{"date"});
    CHECK(texts(s.commands[1]) == V{"echo", "fin"});

    Script q = parse("bash <<'FIN'\nrm -rf /\nFIN");
    REQUIRE(q.commands.size() == 1);
    CHECK(q.commands[0].redirs[0].body == "rm -rf /\n");
    CHECK_FALSE(q.commands[0].redirs[0].body_expands);
    CHECK(q.loose.empty());
    CHECK(q.errors.empty());

    Script t = parse("cat <<-\"X\"\n\tuno\n\tdos\n\tX\n");
    CHECK(t.commands[0].redirs[0].body == "uno\ndos\n");
    CHECK(t.errors.empty());

    Script two = parse("cat <<A <<B\na\nA\nb\nB\nls");
    REQUIRE(two.commands.size() == 2);
    CHECK(two.commands[0].redirs[0].body == "a\n");
    CHECK(two.commands[0].redirs[1].body == "b\n");

    CHECK_FALSE(parse("cat <<EOF\nsin cierre").errors.empty());
    CHECK_FALSE(parse("cat <<EOF").errors.empty());
}

TEST_CASE("lexer: expansiones marcan la palabra como dinámica") {
    Script s = parse("echo $HOME ${X:-$(id)} $(ls -l) `pwd` $((1+2)) <(sort a) >(cat) $1 plano");
    REQUIRE(s.commands.size() == 1);
    const auto& w = s.commands[0].words;
    REQUIRE(w.size() == 10);
    for (std::size_t i = 1; i < 9; ++i) CHECK(w[i].dynamic);
    CHECK_FALSE(w[9].dynamic);
    CHECK(w[1].text == "$HOME");
    CHECK(w[2].subs == V{"id"});
    CHECK(w[3].subs == V{"ls -l"});
    CHECK(w[4].subs == V{"pwd"});
    CHECK(w[5].subs.empty());
    CHECK(w[6].subs == V{"sort a"});
    CHECK(w[6].proc_sub);
    CHECK(w[7].subs == V{"cat"});
    CHECK(parse("echo $(echo \"a)b\")").commands[0].words[1].subs == V{"echo \"a)b\""});
    CHECK_FALSE(parse("echo $(ls").errors.empty());
    CHECK_FALSE(parse("echo `ls").errors.empty());
}

TEST_CASE("lexer: globs") {
    Script s = parse("rm * a?b [ab]c '*' \"x*\" [ ]");
    const auto& w = s.commands[0].words;
    REQUIRE(w.size() == 8);
    CHECK(w[1].glob);
    CHECK(w[2].glob);
    CHECK(w[3].glob);
    CHECK_FALSE(w[4].glob);
    CHECK_FALSE(w[5].glob);
    CHECK_FALSE(w[6].glob);
}

TEST_CASE("lexer: asignaciones de prefijo") {
    Script s = parse("A=1 B+=x C=(1 2) env");
    REQUIRE(s.commands.size() == 1);
    CHECK(s.commands[0].assigns.size() == 3);
    CHECK(texts(s.commands[0]) == V{"env"});
    Script only = parse("X=$(id)");
    REQUIRE(only.commands.size() == 1);
    CHECK(only.commands[0].words.empty());
    CHECK(only.commands[0].assigns[0].subs == V{"id"});
    CHECK(parse("echo A=1").commands[0].words.size() == 2);
}

TEST_CASE("lexer: palabras reservadas") {
    Script s = parse("if test -f x; then rm a; elif true; then :; else ! ls; fi");
    V names;
    for (const auto& c : s.commands) names.push_back(c.words.at(0).text);
    CHECK(names == V{"test", "rm", "true", ":", "ls"});
    CHECK(s.errors.empty());

    Script w = parse("while read l; do echo $l; done < f.txt");
    REQUIRE(w.commands.size() == 3);
    CHECK(w.commands[2].words.empty());
    CHECK(w.commands[2].redirs.size() == 1);

    Script f = parse("for f in $(ls) a; do rm $f; done");
    REQUIRE(f.commands.size() == 1);
    CHECK(f.commands[0].words[0].text == "rm");
    REQUIRE(f.loose.size() == 1);
    CHECK(f.loose[0].subs == V{"ls"});

    Script t = parse("time -p sleep 1");
    CHECK(texts(t.commands[0]) == V{"sleep", "1"});

    Script c = parse("case $x in a|b) rm a;; *) ls;; esac");
    CHECK(c.has_case);
    REQUIRE(c.commands.size() == 2);
    CHECK(c.commands[0].words[0].text == "rm");
    CHECK(c.errors.empty());

    Script tt = parse("[[ $a > 3 && -f x ]] && ls");
    REQUIRE(tt.commands.size() == 2);
    CHECK(tt.commands[0].words[0].text == "[[");
    CHECK(tt.commands[0].redirs.empty());

    CHECK(parse("for ((i=0; i<3; i++)); do echo $i; done").commands.size() == 1);
}

TEST_CASE("lexer: funciones") {
    Script s = parse(":(){ :|:& };:");
    REQUIRE(s.functions.size() == 1);
    CHECK(s.functions[0].name == ":");
    CHECK(s.functions[0].end - s.functions[0].begin == 2);
    REQUIRE(s.commands.size() == 3);
    CHECK(s.commands[0].in_pipe);
    CHECK(s.commands[1].background);
    CHECK(s.errors.empty());

    Script k = parse("function f { rm x; }; f");
    REQUIRE(k.functions.size() == 1);
    CHECK(k.functions[0].name == "f");
    CHECK(k.commands.size() == 2);
}

TEST_CASE("lexer: límite de anidamiento 8") {
    std::string ok = "ls", bad = "ls";
    for (int i = 0; i < 8; ++i) ok = "( " + ok + " )";
    for (int i = 0; i < 9; ++i) bad = "( " + bad + " )";
    CHECK(parse(ok).errors.empty());
    CHECK_FALSE(parse(bad).errors.empty());

    std::string v8 = "x", v9 = "x";
    for (int i = 0; i < 8; ++i) v8 = "${" + v8 + "}";
    for (int i = 0; i < 9; ++i) v9 = "${" + v9 + "}";
    CHECK(parse("echo " + v8).errors.empty());
    CHECK_FALSE(parse("echo " + v9).errors.empty());
    std::string a9 = "1";
    for (int i = 0; i < 9; ++i) a9 = "$((" + a9 + "))";
    CHECK_FALSE(parse("echo " + a9).errors.empty());
}

// ---------------------------------------------------------------- paths

TEST_CASE("rutas: normalización") {
    CHECK(normalize_path("/") == "/");
    CHECK(normalize_path("//") == "/");
    CHECK(normalize_path("/etc//") == "/etc");
    CHECK(normalize_path("~/") == "~");
    CHECK(normalize_path("$HOME") == "~");
    CHECK(normalize_path("${HOME}/") == "~");
    CHECK(normalize_path("$HOME/.ssh/") == "~/.ssh");
    CHECK(normalize_path("$HOMEX") == "$HOMEX");
    CHECK(normalize_path("./") == ".");
    CHECK(normalize_path("./..") == "..");
    CHECK(normalize_path("../../") == "../..");
    CHECK(normalize_path("a/..") == ".");
    CHECK(normalize_path("/tmp/../etc") == "/etc");
    CHECK(normalize_path("/..") == "/");
    CHECK(normalize_path("~/..") == "/home");
    CHECK(normalize_path("./*") == "*");
    CHECK(normalize_path("//*") == "/*");
    CHECK(normalize_path("~root") == "/root");
    CHECK(normalize_path("~root/") == "/root");
    CHECK(normalize_path("~root/.ssh") == "/root/.ssh");
    CHECK(normalize_path("~bob") == "~");
    CHECK(normalize_path("~bob/.ssh/") == "~/.ssh");
    CHECK(normalize_path("~bob/proyectos") == "~/proyectos");
}

TEST_CASE("rutas: conjuntos C, C*, F y D") {
    for (const char* p : {"/", "~", ".", "..", "../..", "/etc", "/usr/local", "/var/lib", "~/.ssh", "~/.gnupg",
                          "/*", "~/*", "/home/*", "*", ".*", "../*"}) {
        CAPTURE(std::string(p));
        CHECK(in_C(p));
    }
    for (const char* p : {"/tmp", "/etc/nginx", "~/proyectos", "*.log", "./build", "/usr/local/bin"}) {
        CAPTURE(std::string(p));
        CHECK_FALSE(in_C(p));
    }
    CHECK(in_Cstar("/"));
    CHECK(in_Cstar("~"));
    CHECK(in_Cstar("/etc/*"));
    CHECK_FALSE(in_Cstar("."));
    CHECK_FALSE(in_Cstar(".."));
    CHECK_FALSE(in_Cstar("*"));
    CHECK_FALSE(in_Cstar(".*"));

    CHECK(in_F("/etc/passwd"));
    CHECK(in_F("/etc/sudoers.d/99-x"));
    CHECK(in_F("/boot/vmlinuz"));
    CHECK(in_F("~/.bashrc"));
    CHECK(in_F("~/.ssh/authorized_keys"));
    CHECK_FALSE(in_F("/boot"));
    CHECK_FALSE(in_F("/etc/passwd.bak"));

    CHECK(in_D("/dev/sda"));
    CHECK(in_D("/dev/nvme0n1p1"));
    CHECK(in_D("/dev/mapper/root"));
    CHECK(in_D("/dev/mem"));
    CHECK_FALSE(in_D("/dev/null"));
    CHECK_FALSE(in_D("/dev/tty"));
}

// ---------------------------------------------------------------- policy and limits

TEST_CASE("política: seguro solo por lista blanca") {
    CHECK(lv("ls -la /tmp") == "seguro:");
    CHECK(classify("ls").findings.empty());
    CHECK(lv("") == "seguro:");
    CHECK(lv("   # nada") == "seguro:");
    CHECK(lv("cat /etc/passwd | grep root | wc -l") == "seguro:");
    CHECK(lv("X=1") == "seguro:");
    CHECK(lv("LANG=C ls") == "seguro:");
    CHECK(lv("foo --bar") == "precaucion:R31");
    CHECK(lv("./deploy.sh") == "precaucion:R31");
    CHECK(lv("ls; foo") == "precaucion:R31");
    CHECK(lv("cd") == "seguro:");
    CHECK(lv("cd /tmp") == "seguro:");
    CHECK(lv("cd -") == "seguro:");
    CHECK(lv("cd /tmp && ls") == "seguro:");
    CHECK(lv("cd / && rm -rf *") == "peligro:R1");
    CHECK(lv(":") == "seguro:");
    CHECK(lv(": hola 'mundo'") == "seguro:");
    CHECK(lv(": ${X:=1}") == "precaucion:R31");
    CHECK(lv(": > vacio.txt") == "precaucion:WRITE");
    CHECK(lv(":(){ :|:& };:") == "peligro:R11");
}

TEST_CASE("política: redirecciones") {
    CHECK(lv("ls >/dev/null") == "seguro:");
    CHECK(lv("ls 2>/dev/null") == "seguro:");
    CHECK(lv("ls 2>&1") == "seguro:");
    CHECK(lv("ls >&2") == "seguro:");
    CHECK(lv("ls &>/dev/null") == "seguro:");
    CHECK(lv("ls < entrada.txt") == "seguro:");
    CHECK(lv("ls > lista.txt") == "precaucion:WRITE");
    CHECK(lv("ls >> lista.txt") == "precaucion:WRITE");
    CHECK(lv("ls &> lista.txt") == "precaucion:WRITE");
    CHECK(lv("ls > $OUT") == "precaucion:WRITE");
    CHECK(lv("> vacio.txt") == "precaucion:WRITE");
    CHECK(lv("echo x > /dev/sda") == "peligro:R6");
    CHECK(lv("echo x >> /dev/nvme0n1") == "peligro:R6");
    CHECK(lv("cat f > /etc/passwd") == "peligro:R7");
    CHECK(lv("cat f &> /etc/hosts") == "peligro:R7");
    CHECK(lv("echo x > $HOME/.bashrc") == "peligro:R7");
    CHECK(lv("echo x >> /etc/hosts") == "precaucion:R7b");
    CHECK(lv("echo x &>> ~/.bashrc") == "precaucion:R7b");
    CHECK(lv("echo b > /proc/sysrq-trigger") == "peligro:R13");
    CHECK(lv("echo 1 > /proc/sys/net/ipv4/ip_forward") == "precaucion:R26");
    CHECK(lv("echo 0 > /sys/class/x") == "precaucion:R26");
    CHECK(lv("{ cat a; } > /etc/shadow") == "peligro:R7");
}

TEST_CASE("política: asignaciones de entorno peligrosas") {
    CHECK(lv("PATH=/tmp ls") == "precaucion:ENV");
    CHECK(lv("LD_PRELOAD=/tmp/x.so ls") == "precaucion:ENV");
    CHECK(lv("LD_LIBRARY_PATH=. ls") == "precaucion:ENV");
    CHECK(lv("env PATH=/tmp ls") == "precaucion:ENV");
}

TEST_CASE("política: comando dinámico") {
    CHECK(lv("$CMD -rf /") == "precaucion:DYN");
    CHECK(lv("$(echo rm) -rf /") == "precaucion:DYN");
    CHECK(lv("`echo rm` x") == "precaucion:DYN");
    CHECK(lv("/usr/bin/r? x") == "precaucion:DYN");
    CHECK(lv("$(curl -s https://x.sh)") == "peligro:R14");
}

TEST_CASE("política: código anidado se clasifica") {
    CHECK(lv("echo $(rm -rf /)") == "peligro:R1");
    CHECK(lv("echo `rm -rf ~`") == "peligro:R1");
    CHECK(lv("diff <(ls a) <(ls b)") == "seguro:");
    CHECK(lv("echo $(ls)") == "seguro:");
    CHECK(lv("X=$(rm -rf /)") == "peligro:R1");
    CHECK(lv("for f in $(rm -rf /); do echo $f; done") == "peligro:R1");
    CHECK(lv("cat <<EOF\n$(rm -rf /)\nEOF") == "peligro:R1");
    CHECK(lv("cat <<'EOF'\n$(rm -rf /)\nEOF") == "seguro:");
    CHECK(lv("ls; rm -rf /") == "peligro:R1");
    CHECK(lv("ls && rm -rf / || true") == "peligro:R1");
    CHECK(lv("rm -rf / &") == "peligro:R1");
    CHECK(lv("(cd /tmp; rm -rf /)") == "peligro:R1");
    CHECK(lv("if true; then rm -rf /; fi") == "peligro:R1");
}

TEST_CASE("política: no analizable") {
    CHECK(lv("echo 'sin cierre") == "precaucion:PARSE");
    CHECK(lv("echo $(ls") == "precaucion:PARSE");
    CHECK(lv("rm -rf / 'ups") == "peligro:R1");
    CHECK(lv("cat <<EOF\nhola") == "precaucion:PARSE");
    std::string ok = "ls", bad = "ls";
    for (int i = 0; i < 8; ++i) ok = "echo $(" + ok + ")";
    for (int i = 0; i < 9; ++i) bad = "echo $(" + bad + ")";
    CHECK(lv(ok) == "seguro:");
    CHECK(lv(bad) == "precaucion:PARSE");
    std::string deep = "rm -rf /";
    for (int i = 0; i < 12; ++i) deep = "( " + deep + " )";
    CHECK(level(deep) == "precaucion");
}

TEST_CASE("política: case") {
    CHECK(lv("case $1 in a) ls;; esac") == "precaucion:CASE");
    CHECK(lv("case $1 in a) rm -rf /;; esac") == "peligro:R1");
}

TEST_CASE("límite de 8 KiB") {
    std::string big = "echo " + std::string(8192, 'a');
    CHECK(lv(big) == "precaucion:SIZE");
    std::string fits = "echo " + std::string(8192 - 5, 'a');
    CHECK(lv(fits) == "seguro:");
}

TEST_CASE("caracteres de control") {
    CHECK(lv("echo \x1b[2J") == "precaucion:CTRL");
    CHECK(lv("ls\x1b[2J") == "precaucion:CTRL,R31");
    CHECK(lv("ls \r") == "precaucion:CTRL");
    CHECK(lv("ls \x7f") == "precaucion:CTRL");
    CHECK(lv("ls\tx\nls") == "seguro:");
    CHECK(lv("rm -rf / \x01") == "peligro:R1");
    Assessment a = classify("ls\x1b");
    REQUIRE_FALSE(a.findings.empty());
    CHECK(a.findings[0].fragment == "\\x1b");
}

TEST_CASE("escape_control") {
    CHECK(escape_control("hola") == "hola");
    CHECK(escape_control("a\x1b[31mb") == "a\\x1b[31mb");
    CHECK(escape_control("a\nb\tc") == "a\nb\tc");
    CHECK(escape_control(std::string("\x00\x01\x7f", 3)) == "\\x00\\x01\\x7f");
    CHECK(escape_control("\r") == "\\x0d");
    CHECK(escape_control("ñ €") == "ñ €");
}

TEST_CASE("findings: motivos y fragmentos") {
    Assessment a = classify("sudo rm -rf ~");
    CHECK(a.level == Risk::Peligro);
    REQUIRE(a.findings.size() == 2);
    CHECK(a.findings[0].rule_id == "R30");
    CHECK(a.findings[0].fragment == "sudo");
    CHECK(a.findings[0].why_es == "se ejecuta como administrador");
    CHECK(a.findings[1].rule_id == "R1");
    CHECK(a.findings[1].fragment == "rm -rf ~");
    CHECK_FALSE(a.findings[1].why_en.empty());
    Assessment b = classify("rm -rf /; rm -rf /");
    CHECK(b.findings.size() == 1);
    Assessment c = classify("rm -rf " + std::string(200, 'x'));
    CHECK(c.findings[0].fragment.size() < 60);
}

// ---------------------------------------------------------------- lexer in classification

TEST_CASE("variantes de escritura del comando") {
    for (const char* cmd : {"rm -rf /", "\"rm\" -rf /", "r''m -rf /", "\\rm -rf /", "/bin/rm -rf /",
                            "rm / -rf", "rm -r -f /", "rm --recursive --force /", "rm -fr -- /",
                            "$'\\x72\\x6d' -rf /", "rm -rf '/'", "rm -rf \"$HOME\"", "rm -rf ${HOME}/",
                            "rm -rf ~/", "rm -rf //", "rm -Rf /usr/", "rm -rf /tmp/../etc"}) {
        CAPTURE(std::string(cmd));
        CHECK(lv(cmd) == "peligro:R1");
    }
}

TEST_CASE("heredocs a intérpretes") {
    CHECK(lv("bash <<EOF\nrm -rf /\nEOF") == "peligro:R1");
    CHECK(lv("sh <<'X'\nls\nX") == "seguro:");
    CHECK(lv("sudo bash <<EOF\nrm -rf ~\nEOF") == "peligro:R1");
    CHECK(lv("bash <<< 'rm -rf /'") == "peligro:R1");
    CHECK(lv("python3 <<EOF\nprint(1)\nEOF") == "precaucion:R14c");
    CHECK(lv("bash < script.sh") == "precaucion:R14c");
    CHECK(lv("cat <<EOF > /etc/passwd\nroot::0:0::/:/bin/sh\nEOF") == "peligro:R7");
}

// ---------------------------------------------------------------- unwrapping

TEST_CASE("envoltorio sudo") {
    CHECK(lv("sudo ls") == "precaucion:R30");
    CHECK(lv("sudo rm -rf /") == "peligro:R1");
    CHECK(lv("sudo -u root rm -rf /") == "peligro:R1");
    CHECK(lv("sudo -uroot -g wheel -- rm -rf /") == "peligro:R1");
    CHECK(lv("sudo -E -H rm -rf /") == "peligro:R1");
    CHECK(lv("sudo -i") == "precaucion:R30");
    CHECK(lv("sudo -s") == "precaucion:R30");
    CHECK(lv("sudo FOO=1 rm -rf /") == "peligro:R1");
    CHECK(lv("sudo --bogus rm -rf /") == "peligro:R1");
    CHECK(lv("sudo --bogus ls") == "precaucion:WRAP,R30");
    CHECK(lv("sudo sudo -u x rm -rf /") == "peligro:R1");
}

TEST_CASE("envoltorios de administrador: doas, pkexec, run0, su, runuser, chroot") {
    CHECK(lv("doas ls") == "precaucion:R30");
    CHECK(lv("doas -u root rm -rf /") == "peligro:R1");
    CHECK(lv("pkexec --user root rm -rf /") == "peligro:R1");
    CHECK(lv("run0 rm -rf /") == "peligro:R1");
    CHECK(lv("su -c 'rm -rf /'") == "peligro:R1");
    CHECK(lv("su - root -c 'rm -rf /'") == "peligro:R1");
    CHECK(lv("su root -c ls") == "precaucion:R30");
    CHECK(lv("su") == "precaucion:R30");
    CHECK(lv("runuser -u bob -- rm -rf ~") == "peligro:R1");
    CHECK(lv("runuser bob -c 'rm -rf ~'") == "peligro:R1");
    CHECK(lv("chroot /mnt rm -rf /") == "peligro:R1");
    CHECK(lv("chroot /mnt ls") == "precaucion:R30");
}

TEST_CASE("envoltorio env") {
    CHECK(lv("env") == "seguro:");
    CHECK(lv("env -i rm -rf ~") == "peligro:R1");
    CHECK(lv("env -u HOME A=1 rm -rf /") == "peligro:R1");
    CHECK(lv("env - ls") == "seguro:");
    CHECK(lv("env -S 'rm -rf' /") == "peligro:R1");
    CHECK(lv("env A=1 ls") == "precaucion:ENV");
    CHECK(lv("env LANG=C ls") == "seguro:");
    CHECK(lv("env -0 ls") == "precaucion:WRAP");
}

TEST_CASE("envoltorios neutros: nice, ionice, nohup, setsid, stdbuf, time, command, builtin, exec, timeout") {
    CHECK(lv("nice -n 10 rm -rf /") == "peligro:R1");
    CHECK(lv("nice -n 10 du -sh .") == "seguro:");
    CHECK(lv("nice --adjustment=5 ls") == "precaucion:WRAP");
    CHECK(lv("ionice -c 3 rm -rf /") == "peligro:R1");
    CHECK(lv("ionice -c 2 -n 7 ls") == "seguro:");
    CHECK(lv("nohup rm -rf / &") == "peligro:R1");
    CHECK(lv("nohup ls") == "seguro:");
    CHECK(lv("setsid rm -rf /") == "peligro:R1");
    CHECK(lv("stdbuf -o0 rm -rf /") == "peligro:R1");
    CHECK(lv("stdbuf -oL ls") == "seguro:");
    CHECK(lv("time rm -rf /") == "peligro:R1");
    CHECK(lv("/usr/bin/time rm -rf /") == "peligro:R1");
    CHECK(lv("command rm -rf /") == "peligro:R1");
    CHECK(lv("command -v docker") == "seguro:");
    CHECK(lv("builtin echo hola") == "seguro:");
    CHECK(lv("exec rm -rf /") == "peligro:R1");
    CHECK(lv("exec > salida.log") == "precaucion:WRITE");
    CHECK(lv("timeout 5 rm -rf /") == "peligro:R1");
    CHECK(lv("timeout -s KILL -k 3 10 rm -rf /") == "peligro:R1");
    CHECK(lv("timeout 5 ping -c 1 example.com") == "seguro:");
    CHECK(lv("timeout --preserve-status 5 ls") == "precaucion:WRAP");
    CHECK(lv("nohup") == "precaucion:R31");
}

TEST_CASE("envoltorios watch y flock") {
    CHECK(lv("watch -n 5 df -h") == "seguro:");
    CHECK(lv("watch -n 1 rm -rf /") == "peligro:R1");
    CHECK(lv("watch 'rm -rf ~'") == "peligro:R1");
    CHECK(lv("flock /tmp/l rm -rf /") == "peligro:R1");
    CHECK(lv("flock -w 5 /tmp/l ls") == "seguro:");
    CHECK(lv("flock /tmp/l -c 'rm -rf /'") == "peligro:R1");
}

TEST_CASE("envoltorio xargs") {
    CHECK(lv("xargs rm -rf") == "precaucion:R4");
    CHECK(lv("xargs -n 1 rm -rf ~") == "peligro:R1");
    CHECK(lv("echo / | xargs rm -rf") == "peligro:R1");
    CHECK(lv("echo a b | xargs -I{} rm -rf /{}") == "precaucion:R4");
    CHECK(lv("printf '/\\n' | xargs -I % rm -rf %") == "peligro:R1");
    CHECK(lv("ls | xargs grep foo") == "seguro:");
    CHECK(lv("ls | xargs") == "seguro:");
    CHECK(lv("ls | xargs sh -c 'rm -rf /'") == "peligro:R1");
    CHECK(lv("ls | xargs -0 grep x") == "seguro:");
    CHECK(lv("ls | xargs --null -r -t --verbose --no-run-if-empty grep x") == "seguro:");
    CHECK(lv("xargs -0 rm -rf") == lv("xargs rm -rf"));
    CHECK(lv("xargs -0 rm -rf") == "precaucion:R4");
    CHECK(lv("xargs -0 -n 1 rm -rf ~") == "peligro:R1");
    CHECK(lv("ls | xargs -p grep x") == "precaucion:WRAP");
    CHECK(lv("ls | xargs -I{} {} -rf /") == "precaucion:DYN");
}

TEST_CASE("find -exec, -execdir, -ok, -okdir") {
    CHECK(lv("find / -name x -exec rm -rf {} +") == "peligro:R1");
    CHECK(lv("find . -name x -exec rm -rf {} +") == "peligro:R1");
    CHECK(lv("find /tmp -name x -execdir rm -rf {} \\;") == "precaucion:R4,R31");
    CHECK(lv("find ~ -ok rm -rf {} ';'") == "peligro:R1");
    CHECK(lv("find /tmp /etc -okdir rm -r {} +") == "peligro:R1");
    CHECK(lv("find . -name '*.c' -exec grep -l main {} +") == "precaucion:R31");
    CHECK(lv("find . -exec chmod -R 777 {} \\; -delete") == "peligro:R23");
    CHECK(lv("find . -name x -exec sh -c 'rm -rf /' \\;") == "peligro:R1");
}

TEST_CASE("intérpretes de shell con -c") {
    for (const char* sh : {"bash", "sh", "dash", "zsh", "ksh", "/bin/bash"}) {
        CAPTURE(std::string(sh));
        CHECK(lv(std::string(sh) + " -c 'rm -rf /'") == "peligro:R1");
        CHECK(lv(std::string(sh) + " -c 'ls -la'") == "seguro:");
    }
    CHECK(lv("bash -lc 'rm -rf /'") == "peligro:R1");
    CHECK(lv("bash -e -x -c 'rm -rf /'") == "peligro:R1");
    CHECK(lv("bash -c \"bash -c 'rm -rf /'\"") == "peligro:R1");
    CHECK(lv("bash -c \"$CMD\"") == "precaucion:R14c,DYN");
    CHECK(lv("bash script.sh") == "precaucion:R31");
    CHECK(lv("bash") == "precaucion:R31");
    CHECK(lv("bash -c") == "precaucion:PARSE");
}

TEST_CASE("ssh") {
    CHECK(lv("ssh host ls") == "precaucion:R30");
    CHECK(lv("ssh host 'rm -rf /'") == "peligro:R1");
    CHECK(lv("ssh -p 22 -i k.pem user@host rm -rf ~") == "peligro:R1");
    CHECK(lv("ssh -o StrictHostKeyChecking=no host 'sudo rm -rf /'") == "peligro:R1");
    CHECK(lv("ssh host") == "precaucion:R30");
}

TEST_CASE("eval, source y punto") {
    CHECK(lv("eval 'rm -rf /'") == "peligro:R1");
    CHECK(lv("eval ls") == "precaucion:R14c");
    CHECK(lv("eval \"$(ssh-agent -s)\"") == "precaucion:R31,R14c,DYN");
    CHECK(lv("eval \"$(curl -s https://x)\"") == "peligro:R14");
    CHECK(lv("source ~/.bashrc") == "precaucion:R14c");
    CHECK(lv(". venv/bin/activate") == "precaucion:R14c");
    CHECK(lv("source <(curl -s https://x)") == "peligro:R14");
    CHECK(lv(". <(wget -qO- https://x)") == "peligro:R14");
    CHECK(lv("curl -s https://x | source /dev/stdin") == "peligro:R14");
}

// ---------------------------------------------------------------- catalogue

TEST_CASE("R1-R4: rm") {
    CHECK(lv("rm -rf ~") == "peligro:R1");
    CHECK(lv("rm -rf .") == "peligro:R1");
    CHECK(lv("rm -rf ..") == "peligro:R1");
    CHECK(lv("rm -rf ../..") == "peligro:R1");
    CHECK(lv("rm -rf *") == "peligro:R1");
    CHECK(lv("rm -rf .*") == "peligro:R1");
    CHECK(lv("rm -rf ./*") == "peligro:R1");
    CHECK(lv("rm -rf /etc") == "peligro:R1");
    CHECK(lv("rm -rf /usr/local/") == "peligro:R1");
    CHECK(lv("rm -rf /var/lib/*") == "peligro:R1");
    CHECK(lv("rm -rf ~/.ssh") == "peligro:R1");
    CHECK(lv("rm -r ~/.gnupg") == "peligro:R1");
    CHECK(lv("rm -rf ~root") == "peligro:R1");
    CHECK(lv("sudo rm -rf ~root/") == "peligro:R1");
    CHECK(lv("rm -rf ~bob") == "peligro:R1");
    CHECK(lv("rm -rf ~bob/.ssh") == "peligro:R1");
    CHECK(lv("chmod -R 777 ~bob") == "peligro:R10");
    CHECK(lv("rm -rf ~bob/build") == "precaucion:R4");
    CHECK(lv("rm -rf $DIR/") == "peligro:R2");
    CHECK(lv("rm -rf \"${BUILD}\"/*") == "peligro:R2");
    CHECK(lv("rm -rf \"$DIR\"") == "precaucion:R4");
    CHECK(lv("rm -rf ${DIR:?}/") == "peligro:R2");
    CHECK(lv("rm --no-preserve-root x") == "peligro:R3");
    CHECK(lv("rm x.txt") == "precaucion:R4");
    CHECK(lv("rm -rf build/") == "precaucion:R4");
    CHECK(lv("rm -rf *.o") == "precaucion:R4");
    CHECK(lv("rm -f /etc/passwd") == "peligro:R7c");
}

TEST_CASE("R5, R5b: dd") {
    CHECK(lv("dd if=/dev/zero of=/dev/sda bs=1M") == "peligro:R5");
    CHECK(lv("dd if=x.iso of=/dev/mmcblk0") == "peligro:R5");
    CHECK(lv("dd if=/dev/zero of=archivo bs=1M count=1") == "precaucion:R5b");
    CHECK(lv("dd if=x of=/etc/shadow") == "peligro:R7");
}

TEST_CASE("R6, R7, R7b: destinos D y F") {
    CHECK(lv("cp disco.img /dev/sdb") == "peligro:R6");
    CHECK(lv("mv x /dev/sda1") == "peligro:R6");
    CHECK(lv("echo x | tee /dev/sda") == "peligro:R6");
    CHECK(lv("cp passwd.new /etc/passwd") == "peligro:R7");
    CHECK(lv("install -m 644 x /etc/sudoers") == "peligro:R7");
    CHECK(lv("ln -sf /tmp/x /etc/hosts") == "peligro:R7");
    CHECK(lv("cp -t /etc/sudoers.d x") == "peligro:R7");
    CHECK(lv("echo x | tee /etc/hosts") == "peligro:R7");
    CHECK(lv("echo x | sudo tee -a /etc/hosts") == "precaucion:R30,R7b");
    CHECK(lv("echo x | tee salida.txt") == "precaucion:WRITE");
    CHECK(lv("echo b | sudo tee /proc/sysrq-trigger") == "peligro:R13");
    CHECK(lv("echo 1 | tee /proc/sys/vm/drop_caches") == "precaucion:R26");
}

TEST_CASE("R8: destrucción de datos") {
    for (const char* c : {"mkfs.ext4 /dev/sdb1", "mkfs -t ext4 /dev/sdb", "mke2fs /dev/sdb1", "mkswap /dev/sdb2",
                          "wipefs -a /dev/sdb", "blkdiscard /dev/nvme0n1", "shred -u secreto.txt",
                          "cryptsetup luksFormat /dev/sdb", "cryptsetup erase /dev/sdb",
                          "cryptsetup luksKillSlot /dev/sdb 0", "pvremove /dev/sdb", "vgremove vg0",
                          "lvremove /dev/vg0/lv"}) {
        CAPTURE(std::string(c));
        CHECK(lv(c) == "peligro:R8");
    }
    CHECK(lv("cryptsetup open /dev/sdb x") == "precaucion:R31");
}

TEST_CASE("R9: particiones") {
    CHECK(lv("fdisk /dev/sda") == "peligro:R9");
    CHECK(lv("parted /dev/sda mklabel gpt") == "peligro:R9");
    CHECK(lv("sgdisk -Z /dev/sda") == "peligro:R9");
    CHECK(lv("fdisk -l") == "seguro:");
    CHECK(lv("parted -l") == "seguro:");
    CHECK(lv("parted /dev/sda print") == "seguro:");
    CHECK(lv("sfdisk -l /dev/sda") == "precaucion:R31");
    CHECK(lv("gdisk -l /dev/sda") == "precaucion:R31");
}

TEST_CASE("R10, R10b: permisos") {
    CHECK(lv("chmod -R 777 /") == "peligro:R10");
    CHECK(lv("chown -R bob ~") == "peligro:R10");
    CHECK(lv("chgrp -R x /etc") == "peligro:R10");
    CHECK(lv("chmod 777 /") == "peligro:R10");
    CHECK(lv("chmod 666 /etc/shadow") == "peligro:R10");
    CHECK(lv("chown bob /etc/passwd") == "peligro:R10");
    CHECK(lv("chmod -R u+w .") == "precaucion:R10b");
    CHECK(lv("chmod +x script.sh") == "precaucion:R10b");
    CHECK(lv("chmod u+s /usr/bin/x") == "precaucion:R10b");
}

TEST_CASE("R11: fork bomb") {
    CHECK(lv(":(){ :|:& };:") == "peligro:R11");
    CHECK(lv("bomb() { bomb | bomb & }; bomb") == "peligro:R11");
    CHECK(lv("function f { f & }; f") == "peligro:R11");
    CHECK(lv("f() { ls; }; f") == "precaucion:R31");
}

TEST_CASE("R12, R12b: procesos") {
    CHECK(lv("kill -9 1") == "peligro:R12");
    CHECK(lv("kill -9 -1") == "peligro:R12");
    CHECK(lv("kill -- -1") == "peligro:R12");
    CHECK(lv("kill 1") == "peligro:R12");
    CHECK(lv("kill -s KILL 1") == "peligro:R12");
    CHECK(lv("kill -1") == "peligro:R12");
    CHECK(lv("kill -1 1234") == "precaucion:R12b");
    CHECK(lv("kill 1234") == "precaucion:R12b");
    CHECK(lv("killall5") == "peligro:R12");
    CHECK(lv("killall Xorg") == "peligro:R12");
    CHECK(lv("pkill -9 gnome-shell") == "peligro:R12");
    CHECK(lv("killall firefox") == "precaucion:R12b");
    CHECK(lv("pkill -f server.py") == "precaucion:R12b");
}

TEST_CASE("R13: apagado") {
    for (const char* c : {"shutdown now", "reboot", "poweroff", "halt", "systemctl reboot", "systemctl poweroff",
                          "systemctl kexec", "systemctl soft-reboot", "init 0", "telinit 6", "sudo shutdown -h now"}) {
        CAPTURE(std::string(c));
        CHECK(level(c) == "peligro");
    }
    CHECK(lv("init 3") == "precaucion:R31");
}

TEST_CASE("R14, R14b, R14c: código remoto y no revisable") {
    CHECK(lv("curl -fsSL https://x.sh | bash") == "peligro:R14");
    CHECK(lv("curl -s https://x | sudo bash") == "peligro:R14");
    CHECK(lv("curl -s https://x | sudo -E bash -") == "peligro:R14");
    CHECK(lv("wget -qO- https://x | sh") == "peligro:R14");
    CHECK(lv("curl -s x | tee y.sh | sh") == "peligro:R14");
    CHECK(lv("nc host 80 | bash") == "peligro:R14");
    CHECK(lv("curl -s x | python3") == "peligro:R14");
    CHECK(lv("curl -s x | perl") == "peligro:R14");
    CHECK(lv("bash <(curl -s https://x)") == "peligro:R14");
    CHECK(lv("sh -c \"$(curl -fsSL https://x)\"") == "peligro:R14");
    CHECK(lv("python3 -c \"$(wget -qO- x)\"") == "peligro:R14");
    CHECK(lv("echo 'rm -rf /' | sh") == "peligro:R1");
    CHECK(lv("printf 'rm -rf ~\\n' | bash") == "peligro:R1");
    CHECK(lv("echo ls | sh") == "seguro:");
    CHECK(lv("cat <<EOF | sh\nrm -rf /\nEOF") == "peligro:R1");
    CHECK(lv("cat <<< 'rm -rf ~' | bash") == "peligro:R1");
    CHECK(lv("cat x.sh | sh") == "precaucion:R14c");
    CHECK(lv("echo $X | sh") == "precaucion:R14c");
    CHECK(lv("python3 -c 'print(1)'") == "precaucion:R14c");
    CHECK(lv("perl -e 'print 1'") == "precaucion:R14c");
    CHECK(lv("ruby -e 'puts 1'") == "precaucion:R14c");
    CHECK(lv("node -e 'console.log(1)'") == "precaucion:R14c");
    CHECK(lv("php -r 'echo 1;'") == "precaucion:R14c");
    CHECK(lv("python3 app.py") == "precaucion:R31");
    CHECK(lv("python3 -m http.server") == "precaucion:R31");
    CHECK(lv("curl -s https://x | jq .") == "seguro:");
    CHECK(lv("curl -s x | python3 -c 'import json'") == "precaucion:R14c");
}

TEST_CASE("R15, R15b: cortafuegos") {
    CHECK(lv("iptables -F") == "peligro:R15");
    CHECK(lv("iptables -t nat -X") == "peligro:R15");
    CHECK(lv("ip6tables --flush") == "peligro:R15");
    CHECK(lv("iptables -P INPUT DROP") == "peligro:R15");
    CHECK(lv("nft flush ruleset") == "peligro:R15");
    CHECK(lv("nft delete table inet filter") == "peligro:R15");
    CHECK(lv("ufw reset") == "peligro:R15");
    CHECK(lv("ufw --force reset") == "peligro:R15");
    CHECK(lv("iptables -A INPUT -p tcp --dport 22 -j ACCEPT") == "precaucion:R15b");
    CHECK(lv("ufw allow 22") == "precaucion:R15b");
    CHECK(lv("firewall-cmd --add-port=80/tcp") == "precaucion:R15b");
    CHECK(lv("iptables -L") == "seguro:");
}

TEST_CASE("R16, R16b: servicios") {
    CHECK(lv("systemctl stop sshd") == "peligro:R16");
    CHECK(lv("systemctl disable --now ssh.service") == "peligro:R16");
    CHECK(lv("systemctl mask systemd-resolved") == "peligro:R16");
    CHECK(lv("systemctl stop NetworkManager") == "peligro:R16");
    CHECK(lv("systemctl isolate rescue.target") == "peligro:R16");
    CHECK(lv("systemctl isolate emergency") == "peligro:R16");
    CHECK(lv("service ssh stop") == "peligro:R16");
    CHECK(lv("systemctl restart nginx") == "precaucion:R16b");
    CHECK(lv("systemctl stop nginx") == "precaucion:R16b");
    CHECK(lv("service nginx restart") == "precaucion:R16b");
    CHECK(lv("systemctl status nginx") == "seguro:");
    CHECK(lv("systemctl list-units --type=service") == "seguro:");
    CHECK(lv("systemctl is-active ssh") == "seguro:");
}

TEST_CASE("R17, R17b: git") {
    for (const char* c : {"git push --force", "git push -f origin main", "git push origin +main",
                          "git reset --hard HEAD~1", "git clean -fdx", "git clean -fd", "git checkout -- .",
                          "git checkout .", "git restore .", "git filter-branch --tree-filter x", "git branch -D x"}) {
        CAPTURE(std::string(c));
        CHECK(lv(c) == "peligro:R17");
    }
    CHECK(lv("git push --force-with-lease") == "precaucion:R17b");
    CHECK(lv("git commit -m 'x'") == "precaucion:R17b");
    CHECK(lv("git restore --staged .") == "precaucion:R17b");
    CHECK(lv("git clean -n") == "precaucion:R17b");
    CHECK(lv("git -c core.pager=x log") == "precaucion:R17b");
    CHECK(lv("git log --output=x.txt") == "precaucion:R17b");
    CHECK(lv("git branch -m nuevo") == "precaucion:R17b");
    CHECK(lv("git branch nueva") == "precaucion:R17b");
    for (const char* c : {"git status", "git log --oneline", "git diff HEAD", "git show HEAD", "git branch -a",
                          "git branch --merged", "git remote -v", "git blame x.c", "git tag", "git tag -l 'v*'",
                          "git stash list", "git config --get user.name", "git -C repo status"}) {
        CAPTURE(std::string(c));
        CHECK(lv(c) == "seguro:");
    }
}

TEST_CASE("R18, R18b: contenedores") {
    CHECK(lv("docker system prune -a") == "peligro:R18");
    CHECK(lv("podman system prune --volumes") == "peligro:R18");
    CHECK(lv("docker volume prune") == "peligro:R18");
    CHECK(lv("docker volume rm datos") == "peligro:R18");
    CHECK(lv("docker rm -f $(docker ps -aq)") == "peligro:R18");
    CHECK(lv("docker system prune") == "precaucion:R18b");
    CHECK(lv("docker rm -f web") == "precaucion:R18b");
    CHECK(lv("docker run --privileged -v /:/host img") == "precaucion:R18b");
    CHECK(lv("docker ps -a") == "seguro:");
    CHECK(lv("docker stats --no-stream") == "seguro:");
    CHECK(lv("docker stats") == "seguro:");
}

TEST_CASE("R19, R20, R20b: crontab y cuentas") {
    CHECK(lv("crontab -r") == "peligro:R19");
    CHECK(lv("crontab -l") == "seguro:");
    CHECK(lv("userdel -r bob") == "peligro:R20");
    CHECK(lv("deluser bob") == "peligro:R20");
    CHECK(lv("groupdel g") == "peligro:R20");
    CHECK(lv("passwd -d bob") == "peligro:R20");
    CHECK(lv("passwd root") == "peligro:R20");
    CHECK(lv("passwd -l root") == "peligro:R20");
    CHECK(lv("sudo passwd") == "peligro:R20");
    CHECK(lv("su -c passwd") == "peligro:R20");
    CHECK(lv("passwd") == "precaucion:R20b");
    CHECK(lv("sudo passwd bob") == "precaucion:R30,R20b");
    CHECK(lv("useradd bob") == "precaucion:R20b");
    CHECK(lv("visudo") == "precaucion:R20b");
}

TEST_CASE("R21, R21b, R22: mover y truncar") {
    CHECK(lv("mv /etc /tmp/etc") == "peligro:R21");
    CHECK(lv("mv ~ /tmp") == "peligro:R21");
    CHECK(lv("mv /etc/passwd /tmp/") == "peligro:R21");
    CHECK(lv("mv datos.txt /dev/null") == "peligro:R21");
    CHECK(lv("mv a.txt b.txt") == "precaucion:R21b");
    CHECK(lv("mv * ../") == "precaucion:R21b");
    for (const char* c : {"cp a b", "ln -s a b", "install x /usr/local/bin/", "mkdir -p x/y", "touch x", "rmdir x",
                          "unlink x", "truncate -s 0 log.txt"}) {
        CAPTURE(std::string(c));
        CHECK(lv(c) == "precaucion:R21b");
    }
    CHECK(lv("truncate -s 0 /etc/passwd") == "peligro:R22");
    CHECK(lv("truncate -s 0 /dev/sda") == "peligro:R22");
}

TEST_CASE("R23, R23b: find -delete") {
    CHECK(lv("find / -name '*.tmp' -delete") == "peligro:R23");
    CHECK(lv("find ~ -name x -delete") == "peligro:R23");
    CHECK(lv("find /tmp -delete") == "peligro:R23");
    CHECK(lv("find ./tmp -empty -delete") == "peligro:R23");
    CHECK(lv("find . -name '*.pyc' -delete") == "precaucion:R23b");
    CHECK(lv("find /tmp -type f -mtime +7 -delete") == "precaucion:R23b");
    CHECK(lv("find . -name '*.log'") == "seguro:");
    CHECK(lv("find . -fprint lista.txt") == "precaucion:WRITE");
}

TEST_CASE("R24, R24b: rsync y scp") {
    CHECK(lv("rsync -a --delete src/ dst/") == "peligro:R24");
    CHECK(lv("rsync -av --delete-after a/ b/") == "peligro:R24");
    CHECK(lv("rsync -a --del a/ b/") == "peligro:R24");
    CHECK(lv("rsync -av a/ host:b/") == "precaucion:R24b");
    CHECK(lv("scp x host:") == "precaucion:R24b");
}

TEST_CASE("R25, R25b, R25c: paquetes") {
    for (const char* c : {"apt remove systemd", "apt-get purge -y libc6", "apt autoremove sudo",
                          "dnf remove kernel-core", "yum erase glibc", "pacman -R linux", "pacman -Rns grub",
                          "zypper rm bash", "apk del coreutils", "snap remove core", "flatpak uninstall x",
                          "dpkg -r dpkg", "rpm -e rpm", "apt remove linux-image-6.8.0-31-generic",
                          "apt remove openssh-server:amd64", "pacman -R linux-zen"}) {
        CAPTURE(std::string(c));
        CHECK(level(c) == (std::string(c) == "snap remove core" || std::string(c) == "flatpak uninstall x"
                               ? "precaucion"
                               : "peligro"));
    }
    CHECK(lv("pacman -Rdd glibc") == "peligro:R25,R25b");
    CHECK(lv("pacman -Rdd firefox") == "peligro:R25b");
    CHECK(lv("rpm -e --nodeps x") == "peligro:R25b");
    CHECK(lv("dpkg --force-all -i x.deb") == "peligro:R25b");
    for (const char* c : {"apt install htop", "apt-get update", "apt upgrade", "dnf install x", "pacman -Syu",
                          "pacman -S vim", "zypper in x", "apk add x", "snap install x", "flatpak install x",
                          "pip install requests", "pipx install x", "npm install -g x", "cargo install x",
                          "dpkg -i x.deb", "rpm -i x.rpm", "apt remove firefox"}) {
        CAPTURE(std::string(c));
        CHECK(lv(c) == "precaucion:R25c");
    }
    for (const char* c : {"apt list --installed", "apt show vim", "apt-cache policy x", "dpkg -l", "dpkg -S /bin/ls",
                          "dnf info x", "rpm -qa", "rpm -qi bash", "pacman -Qi bash", "pacman -Ss vim",
                          "zypper search x", "flatpak list", "snap list"}) {
        CAPTURE(std::string(c));
        CHECK(lv(c) == "seguro:");
    }
}

TEST_CASE("R26: estado del sistema") {
    for (const char* c : {"mount /dev/sdb1 /mnt", "umount /mnt", "swapon -a", "swapoff -a", "modprobe x", "rmmod x",
                          "insmod x.ko", "chattr +i x", "setfacl -m u:x:r f", "sysctl -w vm.swappiness=10",
                          "sysctl vm.swappiness=10", "sysctl -p"}) {
        CAPTURE(std::string(c));
        CHECK(lv(c) == "precaucion:R26");
    }
    CHECK(lv("sysctl -a") == "precaucion:R31");
}

TEST_CASE("R27, R27b: kubectl") {
    CHECK(lv("kubectl delete pods --all") == "peligro:R27");
    CHECK(lv("kubectl delete ns prod") == "peligro:R27");
    CHECK(lv("kubectl delete namespace/prod") == "peligro:R27");
    CHECK(lv("kubectl delete pod x -A") == "peligro:R27");
    CHECK(lv("kubectl delete pod x -n web") == "precaucion:R27b");
    CHECK(lv("kubectl apply -f x.yaml") == "precaucion:R27b");
    CHECK(lv("kubectl get pods -n web") == "seguro:");
    CHECK(lv("kubectl logs -f x") == "seguro:");
}

TEST_CASE("R28: curl y wget") {
    CHECK(lv("curl -s https://x") == "seguro:");
    CHECK(lv("curl -sSL https://x -o -") == "seguro:");
    CHECK(lv("wget -qO- https://x") == "seguro:");
    CHECK(lv("wget -O - https://x") == "seguro:");
    CHECK(lv("curl -o x.tgz https://x") == "precaucion:WRITE,R28");
    CHECK(lv("curl -sSLo x.tgz https://x") == "precaucion:WRITE,R28");
    CHECK(lv("curl -O https://x/a.tgz") == "precaucion:R28");
    CHECK(lv("curl -d 'a=1' https://x") == "precaucion:R28");
    CHECK(lv("curl --data-binary @f https://x") == "precaucion:R28");
    CHECK(lv("curl -F f=@x https://x") == "precaucion:R28");
    CHECK(lv("curl -X POST https://x") == "precaucion:R28");
    CHECK(lv("curl -X GET https://x") == "seguro:");
    CHECK(lv("wget https://x/a.tgz") == "precaucion:R28");
    CHECK(lv("curl -K cfg http://a") == "precaucion:R28");
    CHECK(lv("curl --config cfg http://a") == "precaucion:R28");
    CHECK(lv("curl -sK cfg http://a") == "precaucion:R28");
    CHECK(lv("wget -e robots=off http://a") == "precaucion:R28");
    CHECK(lv("wget --execute robots=off -O- http://a") == "precaucion:R28");
    CHECK(lv("wget --config=x -O- http://a") == "precaucion:R28");
    CHECK(lv("wget -i urls.txt -O-") == "precaucion:R31");
    CHECK(lv("wget -O /etc/passwd https://x") == "peligro:R7");
    CHECK(lv("curl -so /dev/sda https://x") == "peligro:R6");
    CHECK(lv("wget --post-data 'a=1' -O- https://x") == "precaucion:R28");
}

TEST_CASE("R29: sed, perl, yq y awk") {
    CHECK(lv("sed -i 's/a/b/' f") == "precaucion:R29");
    CHECK(lv("sed -i.bak 's/a/b/' f") == "precaucion:R29");
    CHECK(lv("sed --in-place 's/a/b/' f") == "precaucion:R29");
    CHECK(lv("sed -n '1,5p' f") == "seguro:");
    CHECK(lv("sed 's/a/b/g' f") == "seguro:");
    CHECK(lv("sed 's/a/b/w salida' f") == "precaucion:R29");
    CHECK(lv("sed 's/x/date/e' f") == "precaucion:R29");
    CHECK(lv("sed '/x/w copia.txt' f") == "precaucion:R29");
    CHECK(lv("sed -e '1d' -e 'w out' f") == "precaucion:R29");
    CHECK(lv("sed 'e ls' f") == "precaucion:R29");
    CHECK(lv("sed 's/w/e/' f") == "seguro:");
    CHECK(lv("sed -f prog.sed f") == "precaucion:R29");
    CHECK(lv("perl -pi -e 's/a/b/' f") == "precaucion:R29,R14c");
    CHECK(lv("yq -i '.a=1' f.yaml") == "precaucion:R29");
    CHECK(lv("yq '.a' f.yaml") == "seguro:");
    CHECK(lv("awk '{print $1}' f") == "seguro:");
    CHECK(lv("awk -F: '{print $1}' /etc/passwd") == "seguro:");
    CHECK(lv("awk '{system(\"rm \" $1)}' f") == "precaucion:R29");
    CHECK(lv("awk '{print > \"out\"}' f") == "precaucion:R29");
    CHECK(lv("awk '$3 > 100' f") == "seguro:");
    CHECK(lv("awk -f prog.awk f") == "precaucion:R14c");
}

TEST_CASE("R30: envoltorios de administrador") {
    CHECK(lv("sudo cat /etc/shadow") == "precaucion:R30");
    CHECK(lv("doas -s") == "precaucion:R30");
    Assessment a = classify("ssh host uptime");
    REQUIRE(a.findings.size() == 1);
    CHECK(a.findings[0].why_es == "se ejecuta en otra máquina");
}

TEST_CASE("R31: desconocido o script local") {
    CHECK(lv("mi_programa") == "precaucion:R31");
    CHECK(lv("./x.sh") == "precaucion:R31");
    CHECK(lv("pushd /tmp") == "precaucion:R31");
    CHECK(lv("popd") == "precaucion:R31");
    CHECK(lv("export A=1") == "precaucion:R31");
    CHECK(lv("alias ll='ls -l'") == "precaucion:R31");
    Assessment a = classify("./x.sh");
    CHECK(a.findings[0].why_es == "ejecuta un programa o script local");
}

TEST_CASE("lista blanca: condiciones de lectura") {
    for (const char* c : {"ls", "cat a", "head -n 5 a", "grep -r x .", "rg foo", "fd x", "tree", "sort a", "uniq a",
                          "xxd a", "less a", "man ls", "ss -tlnp", "jq . a.json", "find . -type f", "tar -tzf a.tgz",
                          "tar tvf a.tar", "unzip -l a.zip", "7z l a.7z", "date", "hostname", "hostnamectl",
                          "hostnamectl status", "dmesg", "journalctl -u ssh", "loginctl list-sessions",
                          "smartctl -a /dev/sda", "ip addr", "ip -br addr show", "ip route", "nmcli device status",
                          "ps aux", "ping -c 1 x", "echo hola", "printf '%s' x", "test -f x", "[ -f x ]",
                          "[[ -f x ]]", "true", "magick identify a.png", "exiftool a.jpg", "ffprobe a.mp4",
                          "printenv", "which ls", "type ls", "du -sh .", "df -h"}) {
        CAPTURE(std::string(c));
        CHECK(lv(c) == "seguro:");
    }
    for (const char* c : {"sort -o out a", "sort --compress-program=gzip a", "uniq a b", "tree -o out", "xxd a b",
                          "xxd -r a", "less -o log a", "more --log-file=x a", "man -P cat ls", "ss -K dst 1.2.3.4",
                          "rg --pre ./x foo", "fd -x rm", "fd x -X ls", "tar -xzf a.tgz", "tar xvf a.tar",
                          "unzip a.zip", "date -s '2020-01-01'", "hostname nuevo", "hostname -b x",
                          "hostnamectl set-hostname x", "dmesg -c", "journalctl --vacuum-time=1d",
                          "journalctl --rotate", "loginctl lock-session", "smartctl -t long /dev/sda",
                          "ip addr add 1.2.3.4/24 dev eth0", "ip link set eth0 up", "ip route add default via 1.1.1.1",
                          "nmcli connection up x", "nmcli radio wifi on", "exiftool -all= a.jpg",
                          "exiftool -overwrite_original -x a.jpg"}) {
        CAPTURE(std::string(c));
        CHECK(level(c) == "precaucion");
    }
    CHECK(lv("fd -e log -x rm -rf") == "peligro:R1");
}

// ---------------------------------------------------------------- adversarial review, round 2

TEST_CASE("revisión adversaria 2: niveles") {
    struct Case {
        const char* cmd;
        const char* level;
    };
    const Case cases[] = {
        {"rm --rec --force /", "peligro"},
        {"rm --recur -f ~", "peligro"},
        {"git reset --har", "peligro"},
        {"git push --forc", "peligro"},
        {"date --se '2020-01-01'", "precaucion"},
        {"sed --in-pl 's/a/b/' f", "precaucion"},
        {"journalctl --rot", "precaucion"},
        {"dmesg --cle", "precaucion"},
        {"wget --output-doc=x http://a", "precaucion"},
        {"tar --to-comm='sh -c x' -xf a.tar", "precaucion"},
        {"tar --checkpoint-act=exec='rm x' -cf a.tar d", "precaucion"},
        {"tar --checkpoint-action=exec='rm -rf /' -cf a.tar d", "peligro"},
        {"sudo --us root ls", "precaucion"},
        {"LESSOPEN='|x %s' less f", "precaucion"},
        {"PAGER=x git log", "precaucion"},
        {"GIT_SSH_COMMAND=x git fetch", "precaucion"},
        {"BASH_ENV=x bash -c ls", "precaucion"},
        {"env MANPAGER=x man ls", "precaucion"},
        {"LANG=C LC_ALL=C TZ=UTC ls", "seguro"},
        {"TERM=xterm COLUMNS=80 ls", "seguro"},
        {"A=1", "seguro"},
        {"curl -sSL https://x", "seguro"},
        {"curl -sS -H 'A: b' -A ua --max-time 5 https://x", "seguro"},
        {"curl -X GET https://x", "seguro"},
        {"curl -o - https://x", "seguro"},
        {"curl --libcurl x.c https://x", "precaucion"},
        {"curl -K cfg https://x", "precaucion"},
        {"curl --unix-socket /x http://a", "precaucion"},
        {"curl -w '%output{x}' https://x", "precaucion"},
        {"wget -qO- https://x", "seguro"},
        {"wget -q -O - --timeout 5 https://x", "seguro"},
        {"wget -O- -e robots=off https://x", "precaucion"},
        {"wget -O- --no-verbose https://x", "precaucion"},
        {"tar -tzvf a.tgz", "seguro"},
        {"tar tvf a.tar", "seguro"},
        {"tar -tf a.tar --to-command=sh", "precaucion"},
        {"tar -t --use-compress-program=x -f a.tar", "precaucion"},
        {"ip a", "seguro"},
        {"ip -br addr show", "seguro"},
        {"ip route get 1.1.1.1", "seguro"},
        {"ip -s link", "seguro"},
        {"ip a s", "precaucion"},
        {"ip addr add 1.2.3.4/24 dev eth0", "precaucion"},
        {"nmcli", "seguro"},
        {"nmcli device status", "seguro"},
        {"nmcli connection show", "seguro"},
        {"nmcli device wifi list", "seguro"},
        {"nmcli general hostname", "seguro"},
        {"nmcli general hostname nuevo", "precaucion"},
        {"nmcli radio wifi on", "precaucion"},
        {"nmcli device connect wlan0", "precaucion"},
        {"gawk -i inplace '{print}' f", "precaucion"},
        {"awk -i lib.awk '{print}' f", "precaucion"},
        {"less '+!rm x' f", "precaucion"},
        {"more '+|x' f", "precaucion"},
        {"man -H ls", "precaucion"},
        {"git grep -O foo", "precaucion"},
        {"git grep --open-files-in-pager=vim foo", "precaucion"},
        {"bat --pager 'sh -c x' f", "precaucion"},
        {"ssh -o ProxyCommand='rm -rf /' host", "peligro"},
        {"scp -o 'ProxyCommand=rm -rf ~' a host:", "peligro"},
        {"rsync -e 'rm -rf /' a host:b", "peligro"},
        {"trap 'rm -rf /' EXIT", "peligro"},
        {"git rebase -x 'rm -rf /' main", "peligro"},
        {"git rebase --exec='rm -rf ~' main", "peligro"},
        {"git bisect run rm -rf /", "peligro"},
        {"git submodule foreach 'rm -rf /'", "peligro"},
        {"git difftool -x 'rm -rf /'", "peligro"},
        {"tar -cf a.tar -I 'rm -rf /' d", "peligro"},
        {"man -P 'rm -rf /' ls", "peligro"},
        {"env --split-string='rm -rf /'", "peligro"},
        {"env --split-str='rm -rf /'", "peligro"},
        {"flock /tmp/l --command 'rm -rf /'", "peligro"},
        {"script -c 'rm -rf /' /dev/null", "peligro"},
        {"ssh host -- rm -rf /", "peligro"},
        {"rm -rf ${HOME:?}", "peligro"},
        {"rm -rf ${HOME%/}/", "peligro"},
        {"rm -rf $PWD", "peligro"},
        {"rm -rf \"$PWD\"/*", "peligro"},
        {"rm -rf ~+", "peligro"},
        {"rm -rf $(pwd)/", "peligro"},
        {"rm -rf `pwd`/", "peligro"},
        {"rm -rf ${arr[0]}/", "peligro"},
        {"rm -rf /**", "peligro"},
        {"rm -rf /[a-z]*", "peligro"},
        {"rm -rf /?*", "peligro"},
        {"rm -rf ~/.*", "peligro"},
        {"rm -rf /etc/*", "peligro"},
        {"rm -rf '~'", "precaucion"},
        {"rm -rf \"~\"", "precaucion"},
        {"rm -rf \\~", "precaucion"},
        {"rm -rf $'/\\0tmp'", "peligro"},
        {"rm -rf /{etc,usr}", "peligro"},
        {"rm -rf {a,/}", "peligro"},
        {"rm -rf ~{,}", "peligro"},
        {"rm -rf {1..3}", "precaucion"},
        {"rm -rf /usr/sbin", "peligro"},
        {"rm -rf /home/bob", "peligro"},
        {"rm -rf /home/bob/build", "precaucion"},
        {"echo x > /etc/ld.so.preload", "peligro"},
        {"cp x /etc/ssh/sshd_config", "peligro"},
        {"echo x > ~/.zshrc", "peligro"},
        {"echo x > /root/.bashrc", "peligro"},
        {"echo x > /dev/root", "peligro"},
        {"cat x > /dev/nbd0", "peligro"},
        {"cp x /dev/zram0", "peligro"},
        {"dd if=x of=/dev/vg0/root", "peligro"},
        {"echo x > /dev/shm/x", "precaucion"},
        {"echo x > /dev/tty", "seguro"},
        {"cp passwd /etc", "peligro"},
        {"cp passwd /etc/", "peligro"},
        {"cp -t /etc passwd", "peligro"},
        {"cp --target-directory=/etc shadow", "peligro"},
        {"mv .bashrc ~", "peligro"},
        {"install x /etc/sudoers.d", "peligro"},
        {"sort -o /etc/passwd x", "peligro"},
        {"find . -fprint /etc/passwd", "peligro"},
        {"tar -cf /dev/sda d", "peligro"},
        {"curl -o /etc/passwd http://a", "peligro"},
        {"curl --stderr /etc/shadow http://a", "peligro"},
        {"wget -O /etc/hosts http://a", "peligro"},
        {"ddrescue /dev/sda /dev/sdb", "peligro"},
        {"dcfldd if=x of=/dev/sda", "peligro"},
        {"rsync -a x /etc/passwd", "peligro"},
        {"rm /etc/passwd", "peligro"},
        {"unlink /etc/shadow", "peligro"},
        {"busybox rm -rf /", "peligro"},
        {"busybox sh -c 'rm -rf /'", "peligro"},
        {"taskset -c 0 rm -rf /", "peligro"},
        {"taskset 0x1 rm -rf /", "peligro"},
        {"taskset -p 1234", "precaucion"},
        {"chrt -f 10 rm -rf /", "peligro"},
        {"strace -f -o /dev/null rm -rf /", "peligro"},
        {"unshare -r rm -rf /", "peligro"},
        {"systemd-run --user rm -rf ~", "peligro"},
        {"systemd-run --user ls", "precaucion"},
        {"echo 'rm -rf /' | at now", "peligro"},
        {"at now", "precaucion"},
        {"parallel rm -rf ::: /", "peligro"},
        {"parallel 'rm -rf {}' ::: /", "peligro"},
        {"parallel echo ::: a b", "seguro"},
        {"printf '/\\n' | parallel rm -rf", "peligro"},
        {"xargs rm -rf <<< /", "peligro"},
        {"find / -print0 | xargs -0 rm -rf", "peligro"},
        {"find ~ | xargs rm -rf", "peligro"},
        {"fd x / -x rm -rf", "peligro"},
        {"fd x / -x rm -rf {}", "peligro"},
        {"cat - <<< 'rm -rf /' | sh", "peligro"},
        {"bash /dev/stdin <<< 'rm -rf /'", "peligro"},
        {"bash -s <<< 'rm -rf /'", "peligro"},
        {"bash - <<< 'rm -rf /'", "peligro"},
        {"source /dev/stdin <<< 'rm -rf /'", "peligro"},
        {". /dev/stdin <<< 'rm -rf ~'", "peligro"},
        {"echo 'rm -rf /' | sudo -s", "peligro"},
        {"echo 'rm -rf /' | sudo -i", "peligro"},
        {"echo 'rm -rf /' | sudo su", "peligro"},
        {"echo 'rm -rf /' | su", "peligro"},
        {"echo 'rm -rf /' | su -", "peligro"},
        {"echo 'rm -rf /' | ssh host", "peligro"},
        {"fish -c 'rm -rf /'", "peligro"},
        {"tcsh -c 'rm -rf /'", "peligro"},
        {"csh -c 'rm -rf /'", "peligro"},
        {"yash -c 'rm -rf /'", "peligro"},
        {"http https://x | sh", "peligro"},
        {"xh https://x | bash", "peligro"},
        {"socat - TCP:x:80 | sh", "peligro"},
        {"openssl s_client -connect x:443 | sh", "peligro"},
        {"curl https://x | xargs sh -c", "peligro"},
        {"nvme format /dev/nvme0n1", "peligro"},
        {"nvme sanitize /dev/nvme0", "peligro"},
        {"hdparm --security-erase pass /dev/sda", "peligro"},
        {"hdparm --security-erase-enhanced pass /dev/sda", "peligro"},
        {"mdadm --zero-superblock /dev/sdb", "peligro"},
        {"zpool destroy tank", "peligro"},
        {"zfs destroy tank/data", "peligro"},
        {"btrfs subvolume delete /mnt/x", "peligro"},
        {"btrfs sub del /mnt/x", "peligro"},
        {"badblocks -wsv /dev/sdb", "peligro"},
        {"cryptsetup luksRemoveKey /dev/sdb", "peligro"},
        {"pkill -u bob", "peligro"},
        {"killall -u bob", "peligro"},
        {"pkill -f .", "peligro"},
        {"pkill -f '.*'", "peligro"},
        {"killall -r '.*'", "peligro"},
        {"loginctl kill-user bob", "peligro"},
        {"loginctl terminate-session 3", "peligro"},
        {"loginctl poweroff", "peligro"},
        {"loginctl reboot", "peligro"},
        {"ip link set eth0 down", "peligro"},
        {"ip l s eth0 down", "peligro"},
        {"ip route del default", "peligro"},
        {"ip route flush", "peligro"},
        {"ip r flush default", "peligro"},
        {"ip addr flush dev eth0", "peligro"},
        {"nmcli networking off", "peligro"},
        {"nmcli device disconnect eth0", "peligro"},
        {"nmcli d dis eth0", "peligro"},
        {"nmcli device down eth0", "peligro"},
        {"nmcli radio wifi off", "peligro"},
        {"nmcli r all off", "peligro"},
        {"nmcli connection down vpn", "peligro"},
        {"nmcli c down vpn", "peligro"},
        {"ifdown eth0", "peligro"},
        {"ip route del 10.0.0.0/8", "precaucion"},
        {"systemctl stop gdm3", "peligro"},
        {"systemctl stop network-manager", "peligro"},
        {"crontab /dev/null", "peligro"},
        {"crontab x.txt", "precaucion"},
        {"usermod -L root", "peligro"},
        {"usermod --lock root", "peligro"},
        {"usermod -L bob", "precaucion"},
        {"chage -E 0 root", "peligro"},
        {"gpasswd -d bob sudo", "peligro"},
        {"gpasswd -d bob wheel", "peligro"},
        {"gpasswd -a bob docker", "precaucion"},
        {"git checkout -f", "peligro"},
        {"git checkout --force main", "peligro"},
        {"git switch -f main", "peligro"},
        {"git switch --discard-changes main", "peligro"},
        {"git checkout :/", "peligro"},
        {"git restore :/", "peligro"},
        {"git rm -r :/", "peligro"},
        {"git rm -r .", "peligro"},
        {"git push --mirror", "peligro"},
        {"git push --delete origin x", "peligro"},
        {"git push -d origin x", "peligro"},
        {"git push origin :x", "peligro"},
        {"git push --prune origin", "peligro"},
        {"git reflog expire --expire=now --all", "peligro"},
        {"git gc --prune=now", "peligro"},
        {"git prune --expire=now", "peligro"},
        {"git prune --expire now", "peligro"},
        {"git stash clear", "peligro"},
        {"git filter-repo --path x", "peligro"},
        {"git stash drop", "precaucion"},
        {"git branch -f x", "precaucion"},
        {"git rm x.txt", "precaucion"},
        {"docker compose down -v", "peligro"},
        {"docker compose down --volumes", "peligro"},
        {"docker-compose down -v", "peligro"},
        {"podman system reset", "peligro"},
        {"docker compose down", "precaucion"},
        {"rm --help", "precaucion"},
        {"shutdown --help", "precaucion"},
        {"mkfs.ext4 --help", "precaucion"},
        {"ls --help", "seguro"},
        {"git --help", "seguro"},
        {"git --version", "seguro"},
        {"awk '$1 > 5' f", "seguro"},
        {"awk '/a|b/' f", "seguro"},
        {"awk '{ if ($1 > 5) print }' f", "seguro"},
        {"awk '{print ($1 > 5)}' f", "seguro"},
        {"awk '{print $1 > \"out\"}' f", "precaucion"},
        {"awk '{print | \"sort\"}' f", "precaucion"},
        {"awk '\"date\" | getline d' f", "precaucion"},
        {"awk 'BEGIN{system(\"ls\")}'", "precaucion"},
        {"awk '{printf \"%s\\n\", $1 >> \"x\"}' f", "precaucion"},
        {"mkfs.ext4 /dev/sdb1", "peligro"},
        {"mkfs.ext4 disco.img", "peligro"},
        {"shred -u fichero", "peligro"},
        {"find . -name x -empty -delete", "precaucion"},
        {"find ./tmp -empty -delete", "peligro"},
        {"find /tmp -newer x -delete", "precaucion"},
        {"find /tmp -user bob -delete", "precaucion"},
        {"find /tmp -maxdepth 1 -delete", "peligro"},
        {"git tag --contains HEAD", "seguro"},
        {"git branch --contains HEAD", "seguro"},
        {"git config user.name", "seguro"},
        {"git config user.name bob", "precaucion"},
        {"git remote show origin", "seguro"},
        {"git remote get-url origin", "seguro"},
        {"git version", "seguro"},
        {"git help log", "seguro"},
        {"git reflog", "seguro"},
        {"git reflog delete HEAD@{1}", "precaucion"},
        {"git stash show", "seguro"},
        {"git cat-file -p HEAD", "seguro"},
        {"git ls-remote origin", "seguro"},
        {"git ls-tree HEAD", "seguro"},
        {"git rev-list HEAD", "seguro"},
        {"git show-ref", "seguro"},
        {"git merge-base a b", "seguro"},
        {"git for-each-ref", "seguro"},
        {"git count-objects -v", "seguro"},
        {"docker image ls", "seguro"},
        {"docker container ls -a", "seguro"},
        {"docker volume ls", "seguro"},
        {"docker network ls", "seguro"},
        {"docker compose ps", "seguro"},
        {"docker stats", "seguro"},
        {"crontab -l", "seguro"},
        {"hostname -I", "seguro"},
        {"hostname --fqdn", "seguro"},
        {"hostname -F x", "precaucion"},
        {"iptables -L -n -v", "seguro"},
        {"iptables -t nat -L --line-numbers", "seguro"},
        {"ip6tables -S", "seguro"},
        {"iptables -L -F", "peligro"},
        {"nft list ruleset", "seguro"},
        {"ufw status", "seguro"},
        {"ufw status verbose", "seguro"},
        {"ufw status x", "precaucion"},
        {"shutdown -c", "precaucion"},
        {"cd /tmp", "seguro"},
    };
    for (const Case& c : cases) {
        CAPTURE(std::string(c.cmd));
        CHECK(level(c.cmd) == c.level);
    }
}

TEST_CASE("revisión adversaria 2: ids y motivos") {
    CHECK(lv("rm --rec --force /") == "peligro:R1");
    CHECK(lv("PAGER=x git log") == "precaucion:ENV");
    CHECK(lv("curl --unix-socket /x http://a") == "precaucion:R31");
    CHECK(lv("ip a s") == "precaucion:R31");
    CHECK(lv("trap 'rm -rf /' EXIT") == "peligro:R1");
    CHECK(lv("trap 'ls' EXIT") == "precaucion:R14c");
    CHECK(lv("rsync -e ssh a host:b") == "precaucion:R14c,R30,R24b");
    CHECK(lv("rm -rf '~'") == "precaucion:R4");
    CHECK(lv("rm -rf /{etc,usr}") == "peligro:R1");
    CHECK(lv("rm -rf {1..3}") == "precaucion:PARSE,R4");
    CHECK(lv("rm /etc/passwd") == "peligro:R7c");
    CHECK(lv("echo x > /dev/root") == "peligro:R6");
    CHECK(lv("cp passwd /etc") == "peligro:R7");
    CHECK(lv("sort -o /etc/passwd x") == "peligro:R7");
    CHECK(lv("systemd-run --user ls") == "precaucion:R30");
    CHECK(lv("echo 'ls' | at now") == "precaucion:R14c");
    CHECK(lv("nvme format /dev/nvme0n1") == "peligro:R8");
    CHECK(lv("pkill -u bob") == "peligro:R12");
    CHECK(lv("loginctl poweroff") == "peligro:R13");
    CHECK(lv("ifdown eth0") == "peligro:R15");
    CHECK(lv("crontab /dev/null") == "peligro:R19");
    CHECK(lv("chage -E 0 root") == "peligro:R20");
    CHECK(lv("git stash clear") == "peligro:R17");
    CHECK(lv("podman system reset") == "peligro:R18");
    CHECK(lv("rm --help") == "precaucion:R31");
    CHECK(lv("shutdown -c") == "precaucion:R31");
    CHECK(lv("mkfs.ext4 disco.img") == "peligro:R8");  // not downgraded: required by peligro.txt
    CHECK(lv("find ./tmp -empty -delete") == "peligro:R23");  // -empty is not a filter: peligro.txt
    CHECK(lv("find /tmp -newer x -delete") == "precaucion:R23b");
}

TEST_CASE("revisión adversaria 2: flujos de entrada de varias líneas") {
    CHECK(lv("xargs rm -rf <<EOF\n/\nEOF") == "peligro:R1");
    CHECK(lv("cat <<A <<B | sh\nls\nA\nrm -rf /\nB") == "peligro:R1");
    CHECK(lv("cat <<A <<B | sh\nrm -rf /\nA\nls\nB") == "seguro:");
    CHECK(lv("sudo -s <<EOF\nrm -rf /\nEOF") == "peligro:R1");
    CHECK(lv("ssh host <<EOF\nrm -rf ~\nEOF") == "peligro:R1");
    CHECK(lv("at now <<EOF\nrm -rf /\nEOF") == "peligro:R1");
    CHECK(lv("parallel rm -rf <<EOF\n/\nEOF") == "peligro:R1");
    CHECK(lv("echo 'rm -rf /' | xargs -I{} sh -c {}") == "peligro:R1");
    CHECK(lv("printf 'a b\\n' | xargs -I{} rm -rf /{}") == "precaucion:R4");
}

TEST_CASE("rutas: normalización ampliada") {
    CHECK(normalize_path("${HOME:?}") == "~");
    CHECK(normalize_path("${HOME:-/}/x") == "~/x");
    CHECK(normalize_path("$PWD") == ".");
    CHECK(normalize_path("${PWD}/*") == "*");
    CHECK(normalize_path("~+") == ".");
    CHECK(normalize_path("~", false) == "./~");
    CHECK(normalize_path("~/x", false) == "./~/x");
    CHECK(normalize_path("$HOMEDIR") == "$HOMEDIR");
    CHECK(in_C("/**"));
    CHECK(in_C("/[a-z]*"));
    CHECK(in_C("~/.*"));
    CHECK(in_C("/etc/*.conf"));
    CHECK(in_C("/home/bob"));
    CHECK_FALSE(in_C("/home/bob/x"));
    CHECK_FALSE(in_C("../*.log"));
    CHECK(in_C("/usr/libexec"));
    CHECK(in_F("/etc/ld.so.preload"));
    CHECK(in_F("/root/.ssh/authorized_keys"));
    CHECK(in_D("/dev/root"));
    CHECK(in_D("/dev/vg0/root"));
    CHECK_FALSE(in_D("/dev/null"));
    CHECK_FALSE(in_D("/dev/null/x"));
    CHECK_FALSE(in_D("/dev/pts/3"));
    CHECK_FALSE(in_D("/dev/urandom"));
}

TEST_CASE("lexer: ~ entre comillas, llaves y $'\\0'") {
    CHECK(parse("ls ~").commands[0].words[1].tilde);
    CHECK_FALSE(parse("ls '~'").commands[0].words[1].tilde);
    CHECK_FALSE(parse("ls \"~\"").commands[0].words[1].tilde);
    CHECK(parse("echo /{etc,usr}/x").commands[0].words[1].alts == V{"/etc/x", "/usr/x"});
    CHECK(parse("echo {a,{b,c}}").commands[0].words[1].alts == V{"a", "b", "c"});
    CHECK(parse("echo {}").commands[0].words[1].alts.empty());
    CHECK(parse("echo '{a,b}'").commands[0].words[1].alts.empty());
    CHECK_FALSE(parse("echo {1..5}").errors.empty());
    std::string many = "{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}";  // 128 results
    CHECK_FALSE(parse("echo " + many).errors.empty());
    CHECK(parse("echo $'/\\0tmp'").commands[0].words[1].text == "/");
}

// ---------------------------------------------------------------- adversarial review, round 3

TEST_CASE("revisión adversaria 3: edición en sitio de ficheros críticos (R7d)") {
    for (const char* c : {"sed -i d /etc/group", "sed -i.bak 's/a/b/' /etc/hosts", "sed -ni '1p' /etc/passwd",
                          "sed --in-place=.orig -e '/x/d' /etc/sudoers.d/90-x", "sed -ie 's/a/b/' /etc/passwd",
                          "sudo sed -i 's/^root:[^:]*:/root::/' /etc/shadow", "env LC_ALL=C /bin/sed -i 1d /etc/fstab",
                          "sed -i 's/x/y/' ~/.bashrc", "perl -pi -e 's/a/b/' /etc/passwd",
                          "perl -i.bak -pe 's/x/y/' /etc/hosts", "ruby -i -pe 'sub(/a/, \"b\")' /etc/group",
                          "gawk -i inplace '{print}' /etc/passwd"}) {
        CAPTURE(std::string(c));
        CHECK(lv(c) == "peligro:R7d");
    }
    CHECK(lv("sed -i 's/a/b/' notas.txt") == "precaucion:R29");
    CHECK(lv("sed -i 's/a/b/' /etc/hosts.bak") == "precaucion:R29");
    CHECK(lv("perl -pi -e 's/a/b/' notas.txt") == "precaucion:R29,R14c");
    CHECK(lv("gawk -i inplace '{print}' notas.txt") == "precaucion:R29");
    CHECK(lv("sed -n '1,5p' /etc/passwd") == "seguro:");
    CHECK(lv("sed 's/a/b/' /etc/hosts") == "seguro:");
}

TEST_CASE("revisión adversaria 3: ifconfig (R15)") {
    for (const char* c : {"ifconfig eth0 down", "sudo ifconfig wlan0 down", "/sbin/ifconfig eth0 0.0.0.0",
                          "ifconfig eth0 0", "ifconfig eth0 del 192.168.1.10", "ifconfig eth0 inet6 del fe80::1/64"}) {
        CAPTURE(std::string(c));
        CHECK(lv(c) == "peligro:R15");
    }
    for (const char* c : {"ifconfig", "ifconfig -a", "ifconfig eth0", "ifconfig -s", "ifconfig -v eth0"}) {
        CAPTURE(std::string(c));
        CHECK(lv(c) == "seguro:");
    }
    CHECK(lv("ifconfig eth0 up") == "precaucion:R31");
    CHECK(lv("ifconfig eth0 192.168.1.5 netmask 255.255.255.0") == "precaucion:R31");
    CHECK(lv("ifconfig eth0 mtu 1400") == "precaucion:R31");
    CHECK(lv("sudo ifconfig") == "precaucion:R30");
}

TEST_CASE("revisión adversaria 3: contraseña de root con chpasswd y usermod (R20)") {
    for (const char* c : {"echo 'root:1234' | chpasswd", "echo root:x | sudo chpasswd",
                          "echo \"root:$PW\" | chpasswd", "printf 'root:%s\\n' \"$PW\" | sudo chpasswd",
                          "chpasswd <<< 'root:x'", "chpasswd <<EOF\nbob:a\nroot:b\nEOF",
                          "sudo chpasswd -e <<< 'root:$6$abc'", "cat <<EOF | chpasswd\nroot:x\nEOF",
                          "usermod -p '$6$x' root", "sudo usermod --password x root"}) {
        CAPTURE(std::string(c));
        CHECK(lv(c) == "peligro:R20");
    }
    for (const char* c : {"echo 'bob:1234' | chpasswd", "chpasswd < usuarios.txt", "chpasswd",
                          "echo \"$U:1234\" | chpasswd", "echo 'rootkit:x' | chpasswd", "cat usuarios.txt | chpasswd",
                          "usermod --password x bob"}) {
        CAPTURE(std::string(c));
        CHECK(lv(c) == "precaucion:R20b");
    }
}

TEST_CASE("getfacl, timedatectl y apt-mark") {
    for (const char* c : {"getfacl f", "getfacl -R /srv", "timedatectl", "timedatectl status", "timedatectl show",
                          "timedatectl show -p Timezone", "timedatectl show-timesync", "timedatectl timesync-status",
                          "timedatectl list-timezones", "apt-mark showhold", "apt-mark showmanual",
                          "apt-mark showauto", "apt-mark showinstall", "apt-mark showremove", "apt-mark showpurge",
                          "apt-mark -o Debug::NoLocking=1 showhold"}) {
        CAPTURE(std::string(c));
        CHECK(lv(c) == "seguro:");
    }
    for (const char* c : {"timedatectl set-timezone Europe/Madrid", "timedatectl set-ntp true",
                          "timedatectl set-time '2026-01-01 10:00'", "timedatectl set-local-rtc 0",
                          "timedatectl -H srv set-ntp false", "apt-mark hold nginx", "apt-mark unhold nginx",
                          "apt-mark auto x", "apt-mark manual x", "apt-mark minimize-manual", "apt-mark"}) {
        CAPTURE(std::string(c));
        CHECK(lv(c) == "precaucion:R31");
    }
    CHECK(lv("sudo apt-mark hold nginx") == "precaucion:R30,R31");
    CHECK(lv("setfacl -m u:x:r f") == "precaucion:R26");
}

}  // TEST_SUITE
