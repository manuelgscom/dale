// dale_risk: reads commands from stdin (one per line) and writes, for each one,
// "level<TAB>rule_ids<TAB>command". The ids are those of the final level.
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "risk/risk.hpp"

int main() {
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        dale::risk::Assessment a = dale::risk::classify(line);
        std::vector<std::string> ids;
        for (const auto& f : a.findings) {
            if (f.level == a.level && std::find(ids.begin(), ids.end(), f.rule_id) == ids.end()) ids.push_back(f.rule_id);
        }
        std::string joined;
        for (const auto& id : ids) {
            if (!joined.empty()) joined += ',';
            joined += id;
        }
        std::cout << dale::risk::to_string(a.level) << '\t' << joined << '\t' << line << '\n';
    }
    return 0;
}
