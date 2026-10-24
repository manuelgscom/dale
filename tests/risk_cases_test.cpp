// Risk cases from tests/risk/*.txt: one command per line, exact level expected.
#include <fstream>
#include <string>

#include "doctest.h"
#include "risk/risk.hpp"

using dale::risk::Risk;

namespace {

void check_file(const char* name, Risk expected) {
    std::string path = std::string(DALE_RISK_CASES_DIR) + "/" + name;
    std::ifstream f(path);
    const std::string missing = "no existe " + path;
    REQUIRE_MESSAGE(f.good(), missing);
    std::string line;
    int lineno = 0, cases = 0;
    while (std::getline(f, line)) {
        ++lineno;
        std::size_t b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos || line[b] == '#') continue;
        ++cases;
        dale::risk::Assessment a = dale::risk::classify(line);
        if (a.level == expected) continue;
        std::string reasons;
        for (const auto& fd : a.findings) {
            if (fd.level != a.level) continue;
            reasons += "\n    " + fd.rule_id + " [" + fd.fragment + "]: " + fd.why_es;
        }
        const std::string msg = std::string(name) + ":" + std::to_string(lineno) + ": " +
                                dale::risk::escape_control(line) + "\n  esperado " +
                                std::string(dale::risk::to_string(expected)) + ", obtenido " +
                                std::string(dale::risk::to_string(a.level)) + reasons;
        FAIL_CHECK(msg);
    }
    const std::string count = std::string(name) + " tiene " + std::to_string(cases) + " casos (mínimo 150)";
    CHECK_MESSAGE(cases >= 150, count);
}

}  // namespace

TEST_SUITE("risk_cases") {

TEST_CASE("casos seguro.txt") { check_file("seguro.txt", Risk::Seguro); }
TEST_CASE("casos precaucion.txt") { check_file("precaucion.txt", Risk::Precaucion); }
TEST_CASE("casos peligro.txt") { check_file("peligro.txt", Risk::Peligro); }

}  // TEST_SUITE
