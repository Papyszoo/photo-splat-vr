// The scripted-input parser and player (core/ui/input_script.h).
//
// Parsing is deliberately strict: this is a test harness and a typo should fail
// with the line number instead of silently pressing nothing. Playing is one
// pass over a pre-expanded timeline, so the same script and the same advance
// sequence always give the same frames.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/input_script.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "ui/pad_bits.h"

namespace splat {
namespace {

struct Token {
    std::string_view text;
    size_t begin = 0;
    size_t end = 0;
};

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

std::string_view trim_left(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && is_space(text[begin])) ++begin;
    return text.substr(begin);
}

std::string_view trim_right(std::string_view text) {
    size_t end = text.size();
    while (end > 0 && is_space(text[end - 1])) --end;
    return text.substr(0, end);
}

std::string_view trim(std::string_view text) { return trim_right(trim_left(text)); }

std::vector<Token> tokenize(std::string_view line) {
    std::vector<Token> tokens;
    size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() && is_space(line[index])) ++index;
        if (index == line.size()) break;
        const size_t begin = index;
        while (index < line.size() && !is_space(line[index])) ++index;
        tokens.push_back({line.substr(begin, index - begin), begin, index});
    }
    return tokens;
}

// strtod, then require the whole token to be a finite number: "1.5x" and "inf"
// are bad input, not events at time 1 or at infinity.
bool parse_number(std::string_view text, double& out) {
    if (text.empty()) return false;
    std::string owned(text);
    char* end = nullptr;
    const double value = std::strtod(owned.c_str(), &end);
    if (end != owned.c_str() + owned.size()) return false;
    if (!std::isfinite(value)) return false;
    out = value;
    return true;
}

std::string number_text(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

struct ButtonName {
    std::string_view name;
    uint32_t bit;
};

constexpr ButtonName kButtons[] = {
    {"Cross", pad::kCross},       {"Circle", pad::kCircle}, {"Square", pad::kSquare},
    {"Triangle", pad::kTriangle}, {"L1", pad::kL1},         {"R1", pad::kR1},
    {"L2", pad::kL2},             {"R2", pad::kR2},         {"L3", pad::kL3},
    {"R3", pad::kR3},             {"Options", pad::kOptions}, {"Create", pad::kCreate},
    {"Up", pad::kUp},             {"Down", pad::kDown},     {"Left", pad::kLeft},
    {"Right", pad::kRight},
};

bool button_from_name(std::string_view name, uint32_t& bit) {
    for (const ButtonName& button : kButtons) {
        if (button.name == name) {
            bit = button.bit;
            return true;
        }
    }
    return false;
}

bool valid_shot_name(std::string_view name) {
    if (name.empty() || name.size() > 48) return false;
    for (const char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// The bit's index 0..31, or -1 for a word that is not a single button bit
// (every parsed button is one, so this only guards internal mistakes).
int bit_index(uint32_t bit) {
    if (bit == 0 || (bit & (bit - 1)) != 0) return -1;
    int index = 0;
    while ((bit & (1u << index)) == 0) ++index;
    return index;
}

}  // namespace

double InputScript::duration() const {
    double end = 0.0;
    for (const ScriptEvent& event : events_) end = std::max(end, event.time + event.duration);
    return end;
}

Result<InputScript> parse_input_script(std::string_view text) {
    InputScript script;
    double last_time = 0.0;
    bool have_time = false;

    size_t line_number = 0;
    size_t position = 0;
    while (position <= text.size()) {
        size_t end = text.find('\n', position);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = trim(text.substr(position, end - position));
        ++line_number;

        // A comment is a whole line. `mark` text is free, so a '#' inside it is
        // not a comment and the parser never truncates it.
        if (!line.empty() && line.front() != '#') {
            const auto fail = [line_number](std::string message) {
                return Result<InputScript>::failure("line " + std::to_string(line_number) + ": " +
                                                    std::move(message));
            };
            const std::vector<Token> tokens = tokenize(line);
            const std::string_view verb = tokens.size() > 1 ? tokens[1].text : std::string_view();

            double time = 0.0;
            if (!parse_number(tokens[0].text, time)) {
                return fail("bad time '" + std::string(tokens[0].text) + "'");
            }
            if (time < 0.0) return fail("time must not be negative: " + number_text(time));
            if (have_time && time < last_time) {
                return fail("time goes backwards: " + number_text(time) + " after " +
                            number_text(last_time));
            }
            last_time = time;
            have_time = true;

            if (tokens.size() < 2) {
                return fail("missing command (want press, down, up, hold, stick, shot, mark or quit)");
            }

            ScriptEvent event;
            event.time = time;

            if (verb == "press" || verb == "down" || verb == "up") {
                if (tokens.size() < 3) return fail("missing button after '" + std::string(verb) + "'");
                if (tokens.size() > 3) return fail("too many arguments for '" + std::string(verb) + "'");
                if (!button_from_name(tokens[2].text, event.button)) {
                    return fail("unknown button '" + std::string(tokens[2].text) + "'");
                }
                if (verb == "press") {
                    event.kind = ScriptEvent::Kind::Press;
                    event.duration = kPressSeconds;
                } else if (verb == "down") {
                    event.kind = ScriptEvent::Kind::Down;
                } else {
                    event.kind = ScriptEvent::Kind::Up;
                }
            } else if (verb == "hold") {
                if (tokens.size() < 3) return fail("missing button after 'hold'");
                if (tokens.size() < 4) return fail("missing duration after 'hold <Button>'");
                if (tokens.size() > 4) return fail("too many arguments for 'hold'");
                if (!button_from_name(tokens[2].text, event.button)) {
                    return fail("unknown button '" + std::string(tokens[2].text) + "'");
                }
                double seconds = 0.0;
                if (!parse_number(tokens[3].text, seconds)) {
                    return fail("bad duration '" + std::string(tokens[3].text) + "'");
                }
                if (seconds < 0.0) return fail("hold duration must not be negative: " + number_text(seconds));
                event.kind = ScriptEvent::Kind::Hold;
                event.duration = seconds;
            } else if (verb == "stick") {
                if (tokens.size() < 6) {
                    return fail("missing stick values (want <lx> <ly> <rx> <ry>)");
                }
                if (tokens.size() > 6) return fail("too many arguments for 'stick'");
                float values[4] = {};
                for (int i = 0; i < 4; ++i) {
                    double value = 0.0;
                    if (!parse_number(tokens[2 + static_cast<size_t>(i)].text, value)) {
                        return fail("bad stick value '" +
                                    std::string(tokens[2 + static_cast<size_t>(i)].text) + "'");
                    }
                    if (value < -1.0 || value > 1.0) {
                        return fail("stick value out of range: " + number_text(value) + " (want -1..1)");
                    }
                    values[i] = static_cast<float>(value);
                }
                event.kind = ScriptEvent::Kind::Stick;
                event.lx = values[0];
                event.ly = values[1];
                event.rx = values[2];
                event.ry = values[3];
            } else if (verb == "shot") {
                if (tokens.size() < 3) return fail("missing name after 'shot'");
                if (tokens.size() > 3) return fail("too many arguments for 'shot'");
                if (!valid_shot_name(tokens[2].text)) {
                    return fail("bad shot name '" + std::string(tokens[2].text) +
                                "' (1-48 of A-Za-z0-9._-)");
                }
                event.kind = ScriptEvent::Kind::Shot;
                event.text = std::string(tokens[2].text);
            } else if (verb == "mark") {
                if (tokens.size() < 3) return fail("missing text after 'mark'");
                // The rest of the line as written, so the log can quote it.
                event.kind = ScriptEvent::Kind::Mark;
                event.text = std::string(trim_right(line.substr(tokens[2].begin)));
            } else if (verb == "quit") {
                if (tokens.size() > 2) return fail("too many arguments for 'quit'");
                event.kind = ScriptEvent::Kind::Quit;
            } else {
                return fail("unknown command '" + std::string(verb) +
                            "' (want press, down, up, hold, stick, shot, mark or quit)");
            }

            script.events_.push_back(std::move(event));
        }

        if (end == text.size()) break;
        position = end + 1;
    }

    return Result<InputScript>::success(std::move(script));
}

ScriptPlayer::ScriptPlayer(InputScript script) : script_(std::move(script)) {
    const auto down_step = [](double time, uint32_t button) {
        Step step;
        step.time = time;
        step.kind = StepKind::Down;
        step.button = button;
        return step;
    };
    const auto up_step = [](double time, uint32_t button) {
        Step step;
        step.time = time;
        step.kind = StepKind::Up;
        step.button = button;
        return step;
    };

    for (const ScriptEvent& event : script_.events()) {
        Step step;
        step.time = event.time;
        step.button = event.button;
        step.lx = event.lx;
        step.ly = event.ly;
        step.rx = event.rx;
        step.ry = event.ry;
        step.text = event.text;
        switch (event.kind) {
            case ScriptEvent::Kind::Press:
                steps_.push_back(down_step(event.time, event.button));
                steps_.push_back(up_step(event.time + kPressSeconds, event.button));
                break;
            case ScriptEvent::Kind::Hold:
                steps_.push_back(down_step(event.time, event.button));
                steps_.push_back(up_step(event.time + event.duration, event.button));
                break;
            case ScriptEvent::Kind::Down:
                step.kind = StepKind::Down;
                steps_.push_back(std::move(step));
                break;
            case ScriptEvent::Kind::Up:
                step.kind = StepKind::Up;
                steps_.push_back(std::move(step));
                break;
            case ScriptEvent::Kind::Stick:
                step.kind = StepKind::Stick;
                steps_.push_back(std::move(step));
                break;
            case ScriptEvent::Kind::Shot:
                step.kind = StepKind::Shot;
                steps_.push_back(std::move(step));
                break;
            case ScriptEvent::Kind::Mark:
                step.kind = StepKind::Mark;
                steps_.push_back(std::move(step));
                break;
            case ScriptEvent::Kind::Quit:
                step.kind = StepKind::Quit;
                steps_.push_back(std::move(step));
                break;
        }
    }

    // Stable, so events at the same time keep their line order (a `down` line
    // before a `shot` line fires in that order).
    std::stable_sort(steps_.begin(), steps_.end(),
                     [](const Step& a, const Step& b) { return a.time < b.time; });
    start();
}

void ScriptPlayer::start() {
    next_ = 0;
    buttons_ = 0;
    std::fill(std::begin(held_), std::end(held_), 0);
    lx_ = ly_ = rx_ = ry_ = 0.0f;
    now_ = 0.0;
    quit_ = false;
}

void ScriptPlayer::apply(const Step& step, ScriptFrame& frame) {
    switch (step.kind) {
        case StepKind::Down: {
            const int index = bit_index(step.button);
            if (index < 0) return;
            // Counted, so two overlapping holds of one button keep it down until
            // the second one ends.
            if (held_[index]++ == 0) buttons_ |= step.button;
            return;
        }
        case StepKind::Up: {
            const int index = bit_index(step.button);
            if (index < 0) return;
            if (held_[index] > 0 && --held_[index] == 0) buttons_ &= ~step.button;
            return;
        }
        case StepKind::Stick:
            lx_ = step.lx;
            ly_ = step.ly;
            rx_ = step.rx;
            ry_ = step.ry;
            return;
        case StepKind::Shot:
            frame.shots.push_back(step.text);
            return;
        case StepKind::Mark:
            frame.marks.push_back(step.text);
            return;
        case StepKind::Quit:
            quit_ = true;
            return;
    }
}

ScriptFrame ScriptPlayer::advance(double seconds_since_start) {
    ScriptFrame frame;

    double time = seconds_since_start;
    if (!(time >= 0.0)) time = 0.0;  // NaN
    if (time < now_) time = now_;    // no time travel: no event fires twice
    now_ = time;

    while (next_ < steps_.size() && steps_[next_].time <= time) {
        apply(steps_[next_], frame);
        ++next_;
    }

    frame.buttons = buttons_;
    frame.lx = lx_;
    frame.ly = ly_;
    frame.rx = rx_;
    frame.ry = ry_;
    // Sticky: the run ends on a quit, so every later frame still reports it.
    frame.quit = quit_;
    return frame;
}

bool ScriptPlayer::finished() const { return next_ >= steps_.size() && buttons_ == 0; }

}  // namespace splat
