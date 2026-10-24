#include "context/context.hpp"

namespace dale::context {

std::string to_line(const SystemContext& ctx) {
    std::string tools;
    for (const auto& t : ctx.tools) {
        if (!tools.empty()) tools += ',';
        tools += t;
    }
    return "os=" + ctx.os + " distro=" + ctx.distro + " pm=" + ctx.pm + " tools=" + (tools.empty() ? std::string("none") : tools);
}

}  // namespace dale::context
