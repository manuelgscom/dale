#include "cli/report.hpp"

#include <cmath>
#include <nlohmann/json.hpp>

#include "infer/prompt.hpp"
#include "risk/risk.hpp"

namespace dale::cli {

namespace {

using Json = nlohmann::ordered_json;

const char* kind_name(infer::Suggestion::Kind kind) {
    switch (kind) {
        case infer::Suggestion::Kind::Command: return "command";
        case infer::Suggestion::Kind::Refuse: return "refuse";
        case infer::Suggestion::Kind::Unsure: return "unsure";
    }
    return "unsure";
}

long long ms(double v) { return std::llround(v); }

}  // namespace

std::string json_report(const ReportInput& in, const infer::EngineResult& result, UiLang lang) {
    Json j;
    j["contract_version"] = infer::kContractVersion;
    j["dale_version"] = DALE_VERSION;
    j["model"] = in.model;
    j["threads"] = in.threads;
    j["request"] = in.request;
    j["context"] = in.context;
    j["raw"] = result.raw;

    Json response = nullptr;
    Json risk = nullptr;
    if (result.suggestion) {
        const infer::Suggestion& s = *result.suggestion;
        response = {{"type", kind_name(s.kind)}, {"command", s.command}, {"explanation", s.explanation}};
        if (s.kind == infer::Suggestion::Kind::Command) {
            const risk::Assessment a = risk::classify(s.command);
            Json rules = Json::array();
            Json reasons = Json::array();
            // All reasons, in parallel: rules[i] is the id of reasons[i].
            for (const auto& f : a.findings) {
                rules.push_back(f.rule_id);
                std::string why = f.fragment.empty() ? "" : f.fragment + ": ";
                why += lang == UiLang::Es ? f.why_es : f.why_en;
                reasons.push_back(why);
            }
            risk = {{"level", std::string(risk::to_string(a.level))}, {"rules", rules}, {"reasons", reasons}};
        }
    }
    j["response"] = response;
    j["error"] = result.suggestion ? Json(nullptr) : Json(result.error);
    j["error_detail"] = result.suggestion || result.error_detail.empty() ? Json(nullptr) : Json(result.error_detail);
    j["risk"] = risk;

    const infer::Timings& t = result.timings;
    j["timings_ms"] = {{"load", ms(t.load_ms)},
                       {"prefill", ms(t.prefill_ms)},
                       {"generate", ms(t.generate_ms)},
                       {"total", ms(t.total_ms)}};
    j["tokens"] = {{"prompt", t.prompt_tokens}, {"generated", t.gen_tokens}};
    return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

}  // namespace dale::cli
