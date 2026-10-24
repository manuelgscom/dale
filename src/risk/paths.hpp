#pragma once
// Path normalization and the C, C*, F and D sets of the rule catalog.

#include <string>
#include <string_view>

namespace dale::risk {

// Removes double and trailing slashes, resolves "." and ".." lexically, maps ~, ~user,
// $HOME and ${HOME…} to "~" (~root to /root) and $PWD, ${PWD…} and ~+ to ".". tilde = false if
// the leading ~ was quoted (it is a file named "~").
std::string normalize_path(std::string_view text, bool tilde = true);

bool in_C(std::string_view norm);      // critical paths
bool in_Cstar(std::string_view norm);  // C without the current-directory forms
bool in_F(std::string_view norm);      // critical files
bool in_D(std::string_view norm);      // devices

}  // namespace dale::risk
