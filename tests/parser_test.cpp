#include "doctest.h"

#include "infer/parser.hpp"

#include <string>

using dale::infer::parse_response;
using dale::infer::ParseResult;
using dale::infer::Suggestion;

namespace {

// Checks the error and returns the detail.
std::string err(std::string_view raw, std::string_view code) {
    const ParseResult r = parse_response(raw);
    CHECK_FALSE(r.suggestion.has_value());
    CHECK(r.error == code);
    CHECK_FALSE(r.detail.empty());
    return r.detail;
}

Suggestion ok(std::string_view raw) {
    const ParseResult r = parse_response(raw);
    REQUIRE_MESSAGE(r.suggestion.has_value(), r.error << ": " << r.detail);
    CHECK(r.error.empty());
    return *r.suggestion;
}

}  // namespace

TEST_SUITE("infer") {

TEST_CASE("parser: válido de cada tipo") {
    Suggestion s = ok(R"({"type":"command","command":"ls -la","explanation":"Lista los ficheros."})");
    CHECK(s.kind == Suggestion::Kind::Command);
    CHECK(s.command == "ls -la");
    CHECK(s.explanation == "Lista los ficheros.");

    s = ok(R"({"type":"refuse","command":"","explanation":"Not a shell task."})");
    CHECK(s.kind == Suggestion::Kind::Refuse);
    CHECK(s.command.empty());

    s = ok(R"({"type":"unsure","command":"","explanation":"¿Qué ficheros?"})");
    CHECK(s.kind == Suggestion::Kind::Unsure);
    CHECK(s.explanation == "¿Qué ficheros?");
}

TEST_CASE("parser: blancos ASCII alrededor, orden de claves libre y recorte") {
    const Suggestion s =
        ok(" \n\t{ \"explanation\" : \"x \", \"command\":\"  df -h \", \"type\":\"command\" }\r\n ");
    CHECK(s.command == "df -h");
    CHECK(s.explanation == "x");
}

TEST_CASE("parser: escapes y UTF-8") {
    Suggestion s = ok(R"({"type":"command","command":"echo \"a\\b\" | tr a b","explanation":"España, ñ y á."})");
    CHECK(s.command == "echo \"a\\b\" | tr a b");
    CHECK(s.explanation == "España, ñ y á.");
    s = ok(R"({"type":"command","command":"echo 😀","explanation":"emoji \/ ok"})");
    CHECK(s.command == "echo \xF0\x9F\x98\x80");
    CHECK(s.explanation == "emoji / ok");
}

TEST_CASE("parser: claves extra, que faltan y repetidas") {
    CHECK(err(R"({"type":"command","command":"ls","explanation":"x","risk":"low"})", "invalid_schema") ==
          "unexpected key: risk");
    CHECK(err(R"({"type":"command","command":"ls"})", "invalid_schema") == "missing key: explanation");
    CHECK(err(R"({"type":"command","command":"ls","explanation":"x","type":"refuse"})", "invalid_schema") ==
          "duplicate key: type");
    CHECK(err(R"({"type":"command","command":"rm -rf /","command":"ls","explanation":"x"})", "invalid_schema") ==
          "duplicate key: command");
    // A nested key with the same name is not a duplicate: it is a non-string value.
    CHECK(err(R"({"type":{"type":"x"},"command":"ls","explanation":"x"})", "invalid_schema") ==
          "key is not a string: type");
    err("{}", "invalid_schema");
}

TEST_CASE("parser: valores que no son cadenas") {
    err(R"({"type":"command","command":["ls"],"explanation":"x"})", "invalid_schema");
    err(R"({"type":"command","command":"ls","explanation":null})", "invalid_schema");
    err(R"({"type":1,"command":"ls","explanation":"x"})", "invalid_schema");
}

TEST_CASE("parser: tipo erróneo") {
    CHECK(err(R"({"type":"Command","command":"ls","explanation":"x"})", "invalid_schema") == "unknown type");
    err(R"({"type":"","command":"","explanation":"x"})", "invalid_schema");
}

TEST_CASE("parser: texto antes o después") {
    err(R"(Here: {"type":"command","command":"ls","explanation":"x"})", "invalid_json");
    err(R"({"type":"command","command":"ls","explanation":"x"} ok)", "invalid_json");
    err(R"({"type":"command","command":"ls","explanation":"x"}{"type":"command"})", "invalid_json");
    err("```json\n{\"type\":\"command\",\"command\":\"ls\",\"explanation\":\"x\"}\n```", "invalid_json");
    // A Unicode space is not ASCII whitespace.
    err("\xC2\xA0{\"type\":\"command\",\"command\":\"ls\",\"explanation\":\"x\"}", "invalid_json");
}

TEST_CASE("parser: bloque <think>") {
    err("<think>\nlist files\n</think>\n\n{\"type\":\"command\",\"command\":\"ls\",\"explanation\":\"x\"}",
        "invalid_json");
}

TEST_CASE("parser: truncado, vacío y no objeto") {
    err(R"({"type":"command","command":"find / -name)", "invalid_json");
    err(R"({"type":"command","command":"ls","explanation":"x")", "invalid_json");
    err("", "invalid_json");
    err(" \n ", "invalid_json");
    err(R"(["ls"])", "invalid_json");
    err(R"("ls")", "invalid_json");
    err("{\"type\":\"command\",\"command\":\"\xFF\",\"explanation\":\"x\"}", "invalid_json");
}

TEST_CASE("parser: número fuera de rango no aborta") {
    // nlohmann throws out_of_range (not parse_error) on number overflow.
    err(R"({"type":"command","command":"ls","explanation":"x","n":1e999})", "invalid_json");
    err(R"({"type":"command","command":"ls","explanation":"x","n":-1e999})", "invalid_json");
}

TEST_CASE("parser: comando con salto de línea o control") {
    CHECK(err(R"({"type":"command","command":"ls\nrm -rf ~","explanation":"x"})", "invalid_schema") ==
          "control character in command");
    err(R"({"type":"command","command":"ls\r","explanation":"x"})", "invalid_schema");
    err(R"({"type":"command","command":"ls\tx","explanation":"x"})", "invalid_schema");
    err(R"({"type":"command","command":"echo \u001b[31m","explanation":"x"})", "invalid_schema");
    err(R"({"type":"command","command":"echo \u009b31m","explanation":"x"})", "invalid_schema");
    err(R"({"type":"command","command":"echo \u007f","explanation":"x"})", "invalid_schema");
}

TEST_CASE("parser: comando vacío o demasiado largo") {
    CHECK(err(R"({"type":"command","command":"   ","explanation":"x"})", "invalid_schema") == "empty command");
    const std::string max(1000, 'a');
    ok(R"({"type":"command","command":")" + max + R"(","explanation":"x"})");
    CHECK(err(R"({"type":"command","command":")" + max + R"(a","explanation":"x"})", "invalid_schema") ==
          "command too long");
}

TEST_CASE("parser: refuse o unsure con comando no vacío") {
    CHECK(err(R"({"type":"refuse","command":"ls","explanation":"x"})", "invalid_schema") ==
          "command must be empty for type refuse");
    err(R"({"type":"unsure","command":" ","explanation":"x"})", "invalid_schema");
}

TEST_CASE("parser: explicación vacía, larga o con control") {
    CHECK(err(R"({"type":"command","command":"ls","explanation":""})", "invalid_schema") == "empty explanation");
    err(R"({"type":"refuse","command":"","explanation":"  "})", "invalid_schema");
    const std::string max(600, 'e');
    ok(R"({"type":"command","command":"ls","explanation":")" + max + R"("})");
    err(R"({"type":"command","command":"ls","explanation":")" + max + R"(e"})", "invalid_schema");
    err(R"({"type":"command","command":"ls","explanation":"a\nb"})", "invalid_schema");
}

}  // TEST_SUITE
