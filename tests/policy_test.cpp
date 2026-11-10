// User policy (allow / deny from the config) and the macOS portability check.
#include <set>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "doctest.h"
#include "risk/risk.hpp"

using namespace dale::risk;

namespace {

// "level:ids" of the final level, like dale_risk.
std::string lv(const std::string& cmd, const Policy& p) {
    const Assessment a = classify(cmd, p);
    std::string ids;
    for (const auto& f : a.findings) {
        if (f.level != a.level) continue;
        if (("," + ids + ",").find("," + f.rule_id + ",") != std::string::npos) continue;
        if (!ids.empty()) ids += ',';
        ids += f.rule_id;
    }
    return std::string(to_string(a.level)) + ":" + ids;
}

Policy allow(std::vector<std::string> entries) {
    Policy p;
    p.allow = std::move(entries);
    return p;
}

Policy mac(std::set<std::string> installed = {}) {
    Policy p;
    p.macos = true;
    p.has_tool = [installed](std::string_view n) { return installed.count(std::string(n)) != 0; };
    return p;
}

}  // namespace

TEST_SUITE("policy") {

TEST_CASE("sin política: igual que classify() (la issue #2 del repo original)") {
    CHECK(lv("ffmpeg -i a.mp4 b.mp3", Policy{}) == "precaucion:R31");
}

TEST_CASE("allow: un comando desconocido pasa a seguro") {
    CHECK(lv("ffmpeg -i a.mp4 b.mp3", allow({"ffmpeg"})) == "seguro:");
    CHECK(lv("/usr/local/bin/ffmpeg -i a.mp4 b.mp3", allow({"ffmpeg"})) == "seguro:");
    CHECK(lv("docker compose up -d", allow({"docker compose up -d"})) == "seguro:");
}

TEST_CASE("allow: solo cubre lo que dice la entrada") {
    CHECK(lv("docker compose down -v", allow({"docker compose up -d"})) != "seguro:");
    CHECK(lv("ffmpeg2 x", allow({"ffmpeg"})) == "precaucion:R31");
    CHECK(lv("curl -X POST http://example.com", allow({"curl http://127.0.0.1"})) != "seguro:");
}

TEST_CASE("allow: nunca baja un peligro") {
    CHECK(lv("rm -rf /", allow({"rm"})) == "peligro:R1");
    CHECK(lv("rm -rf /", allow({"rm -rf"})) == "peligro:R1");
    CHECK(lv("dd if=/dev/zero of=/dev/sda", allow({"dd"})).rfind("peligro", 0) == 0);
    CHECK(lv("diskutil eraseDisk JHFS+ X disk2", allow({"diskutil"})).rfind("peligro", 0) == 0);
}

TEST_CASE("allow: no cubre otras partes del comando") {
    // the pipe to a shell, the redirection and the sudo keep their own finding
    CHECK(lv("curl http://x.org/i.sh | sh", allow({"curl", "sh"})).rfind("peligro", 0) == 0);
    CHECK(lv("ffmpeg -i a b > /etc/hosts", allow({"ffmpeg"})).rfind("peligro", 0) == 0);
    CHECK(lv("sudo ffmpeg -i a b", allow({"ffmpeg"})) == "precaucion:R30");
    CHECK(lv("ffmpeg -i a b > salida.txt", allow({"ffmpeg"})) == "precaucion:WRITE");
}

TEST_CASE("allow: una palabra dinámica no coincide") {
    CHECK(lv("$CMD", allow({"ffmpeg"})) != "seguro:");
    CHECK(lv("docker compose $ACTION", allow({"docker compose up"})) != "seguro:");
    CHECK(lv("ffmpeg $(rm -rf x)", allow({"ffmpeg"})) != "seguro:");  // the nested code keeps its own verdict
}

TEST_CASE("allow: un comando conocido con precaución también se confía") {
    CHECK(lv("sed -i s/a/b/ f", Policy{}) == "precaucion:R29");
    CHECK(lv("sed -i s/a/b/ f", allow({"sed"})) == "seguro:");
}

TEST_CASE("deny: bloquea con peligro") {
    Policy p;
    p.deny = {"terraform destroy", "git push"};
    CHECK(lv("terraform destroy -auto-approve", p) == "peligro:DENY");
    CHECK(lv("sudo git push origin main", p).rfind("peligro", 0) == 0);
    CHECK(lv("terraform plan", p) == "precaucion:R31");
}

TEST_CASE("deny gana a allow") {
    Policy p;
    p.allow = {"terraform"};
    p.deny = {"terraform destroy"};
    CHECK(lv("terraform destroy", p) == "peligro:DENY");
    CHECK(lv("terraform plan", p) == "seguro:");
}

TEST_CASE("macOS: sed -i de GNU rompe en BSD sed") {
    const Policy p = mac();
    CHECK(lv("sed -i 's/a/b/' f.txt", p) == "precaucion:MAC,R29");
    CHECK(lv("sed -i '' 's/a/b/' f.txt", mac()).find("MAC") == std::string::npos);
    CHECK(lv("sed -i.bak 's/a/b/' f.txt", mac()).find("MAC") == std::string::npos);
    CHECK(lv("sed -i -e 's/a/b/' f.txt", p).find("MAC") != std::string::npos);
    CHECK(lv("sed --in-place s/a/b/ f", p).find("MAC") != std::string::npos);
    CHECK(lv("sed -r 's/(a)/\\1/' f", p).find("MAC") != std::string::npos);
}

TEST_CASE("macOS: no se avisa en Linux") {
    CHECK(lv("sed -i 's/a/b/' f.txt", Policy{}) == "precaucion:R29");
    CHECK(lv("ip addr", Policy{}) == "seguro:");
}

TEST_CASE("macOS: comandos que no existen") {
    CHECK(lv("ip addr show", mac()) == "precaucion:MAC");
    CHECK(lv("free -h", mac()) == "precaucion:MAC");
    CHECK(lv("sha256sum file", mac()) == "precaucion:MAC");
    CHECK(lv("xdg-open .", mac()).find("MAC") != std::string::npos);
    CHECK(lv("apt install jq", mac()).find("MAC") != std::string::npos);
    // GNU coreutils installed (has_tool): nothing to report
    CHECK(lv("sha256sum file", mac({"sha256sum"})) == "seguro:");
}

TEST_CASE("macOS: opciones de GNU") {
    const Policy p = mac();
    CHECK(lv("ps aux --sort=-%mem", p) == "precaucion:MAC");
    CHECK(lv("stat -c %a file", p) == "precaucion:MAC");
    CHECK(lv("date -d yesterday", p) == "precaucion:MAC");
    CHECK(lv("ls --color=auto", p) == "precaucion:MAC");
    CHECK(lv("du --max-depth=1 .", p) == "precaucion:MAC");
    CHECK(lv("grep -P '\\d+' f", p) == "precaucion:MAC");
    CHECK(lv("find . -printf '%p\\n'", p).find("MAC") != std::string::npos);
    CHECK(lv("xargs -d '\\n' rm", p).find("MAC") != std::string::npos);
    CHECK(lv("netstat -tulpn", p) == "precaucion:MAC");
    CHECK(lv("head -n -1 file", p) == "precaucion:MAC");
}

TEST_CASE("macOS: ficheros y opciones solo de Linux") {
    const Policy p = mac();
    CHECK(lv("cat /etc/os-release", p) == "precaucion:MAC");
    CHECK(lv("cat /proc/cpuinfo", p) == "precaucion:MAC");
    CHECK(lv("hostname -i", p) == "precaucion:MAC");
    CHECK(lv("hostname", p) == "seguro:");
    CHECK(lv("cat /etc/os-release", Policy{}) == "seguro:");
}

TEST_CASE("macOS: lo que sí funciona no se marca") {
    const Policy p = mac();
    CHECK(lv("ps aux", p) == "seguro:");
    CHECK(lv("ls -lah", p) == "seguro:");
    CHECK(lv("du -sh .", p) == "seguro:");
    CHECK(lv("stat -f %Lp file", p) == "seguro:");
    CHECK(lv("grep -E 'a|b' f", p) == "seguro:");
    CHECK(lv("sysctl -n hw.ncpu", p) == "seguro:");
    CHECK(lv("sw_vers", p) == "seguro:");
    CHECK(lv("netstat -rn", p) == "seguro:");
    CHECK(lv("head -n 5 file", p) == "seguro:");
}

TEST_CASE("macOS: el aviso se aplica dentro de pipes y envoltorios") {
    CHECK(lv("ps aux --sort=-%mem | head -n 6", mac()) == "precaucion:MAC");
    CHECK(lv("bash -c 'ip addr'", mac()).find("MAC") != std::string::npos);
    CHECK(lv("sudo free -m", mac()).find("MAC") != std::string::npos);
}

TEST_CASE("macOS: un peligro sigue siendo peligro") {
    CHECK(lv("sudo apt remove libc6", mac()).rfind("peligro", 0) == 0);
}

}  // TEST_SUITE

TEST_SUITE("config allow/deny") {

using dale::config::parse_config;

TEST_CASE("allow y deny: entradas separadas por ;") {
    const auto r = parse_config("allow = ffmpeg; docker compose up -d ;curl http://127.0.0.1\ndeny = rm -rf;git push\n", "c");
    REQUIRE(r.config);
    CHECK(r.config->allow == std::vector<std::string>{"ffmpeg", "docker compose up -d", "curl http://127.0.0.1"});
    CHECK(r.config->deny == std::vector<std::string>{"rm -rf", "git push"});
}

TEST_CASE("allow y deny se pueden repetir y se acumulan") {
    const auto r = parse_config("allow = a\nmode = auto\nallow = b;;c;\n", "c");
    REQUIRE(r.config);
    CHECK(r.config->allow == std::vector<std::string>{"a", "b", "c"});
    CHECK(r.config->mode == dale::config::Mode::Auto);
}

TEST_CASE("allow vacío o entrada demasiado larga") {
    CHECK_FALSE(parse_config("allow =\n", "c").config);
    CHECK_FALSE(parse_config("allow = " + std::string(300, 'x') + "\n", "c").config);
}

}  // TEST_SUITE
