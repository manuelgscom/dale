#pragma once
// JSON tracker used to stop generation: brace depth, inside/outside a string and escapes.
// It does not validate the JSON; the parser does that.

#include <cstddef>
#include <string_view>

namespace dale::infer {

class JsonTracker {
public:
    enum class State {
        Waiting,  // only whitespace so far
        Open,     // inside the top-level object
        Done,     // top-level object closed
        Invalid,  // first non-whitespace character is not '{'
    };

    // Consumes a chunk of text. After Done or Invalid it consumes nothing.
    // Returns how many bytes of the chunk belong to the response (up to the final '}').
    std::size_t feed(std::string_view piece) {
        std::size_t i = 0;
        for (; i < piece.size() && (state_ == State::Waiting || state_ == State::Open); ++i) {
            step(piece[i]);
        }
        return i;
    }

    State state() const { return state_; }

private:
    void step(char c) {
        if (state_ == State::Waiting) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return;
            if (c != '{') {
                state_ = State::Invalid;
                return;
            }
            state_ = State::Open;
            depth_ = 1;
            return;
        }
        if (in_string_) {
            if (escape_) {
                escape_ = false;
            } else if (c == '\\') {
                escape_ = true;
            } else if (c == '"') {
                in_string_ = false;
            }
            return;
        }
        if (c == '"') {
            in_string_ = true;
        } else if (c == '{') {
            ++depth_;
        } else if (c == '}') {
            if (--depth_ == 0) state_ = State::Done;
        }
    }

    State state_ = State::Waiting;
    int depth_ = 0;
    bool in_string_ = false;
    bool escape_ = false;
};

}  // namespace dale::infer
