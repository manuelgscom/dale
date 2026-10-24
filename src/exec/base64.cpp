#include "exec/base64.hpp"

namespace dale::exec {

std::string base64_encode(std::string_view data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const unsigned n = static_cast<unsigned>(static_cast<unsigned char>(data[i])) << 16 |
                           static_cast<unsigned>(static_cast<unsigned char>(data[i + 1])) << 8 |
                           static_cast<unsigned>(static_cast<unsigned char>(data[i + 2]));
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += kAlphabet[n & 63];
    }
    const size_t rest = data.size() - i;
    if (rest > 0) {
        unsigned n = static_cast<unsigned>(static_cast<unsigned char>(data[i])) << 16;
        if (rest == 2) n |= static_cast<unsigned>(static_cast<unsigned char>(data[i + 1])) << 8;
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += rest == 2 ? kAlphabet[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

}  // namespace dale::exec
