// W6: the scripted-input parser and player (core/ui/input_script.h).
//
// The parser is strict (a typo must fail with its line number, not press
// nothing) and the player is pure, so the whole unattended console test method
// rests on these checks: press and hold lengths, overlapping holds, stick
// persistence, one-shot events firing exactly once even across a frame longer
// than the event, and the same script and steps always producing the same
// frames.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "../platform/ps5/viewer/pad_input.h"
#include "ui/input_script.h"
#include "ui/pad_bits.h"

namespace {

using splat::InputScript;
using splat::kPressSeconds;
using splat::ScriptEvent;
using splat::ScriptFrame;
using splat::ScriptPlayer;

splat::Result<InputScript> parse(const char* text) {
    return splat::parse_input_script(text);
}

bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("input_script parses every event kind") {
    const auto parsed = parse(
        "# a comment\n"
        "\n"
        "0.0 press Cross\n"
        "0.5 down L1\n"
        "0.6 up L1\n"
        "0.7 hold R1 2.5\n"
        "1.0 stick 0.5 -0.5 1 -1\n"
        "1.5 shot menu-1\n"
        "1.6 mark menu opens\n"
        "2.0 quit\r\n");
    REQUIRE_MESSAGE(parsed.ok(), parsed.message());
    const InputScript& script = parsed.value();
    REQUIRE(script.events().size() == 8);

    CHECK(script.events()[0].kind == ScriptEvent::Kind::Press);
    CHECK(script.events()[0].time == doctest::Approx(0.0));
    CHECK(script.events()[0].button == splat::pad::kCross);
    CHECK(script.events()[0].duration == doctest::Approx(kPressSeconds));

    CHECK(script.events()[1].kind == ScriptEvent::Kind::Down);
    CHECK(script.events()[1].button == splat::pad::kL1);
    CHECK(script.events()[2].kind == ScriptEvent::Kind::Up);
    CHECK(script.events()[2].button == splat::pad::kL1);

    CHECK(script.events()[3].kind == ScriptEvent::Kind::Hold);
    CHECK(script.events()[3].button == splat::pad::kR1);
    CHECK(script.events()[3].duration == doctest::Approx(2.5));

    CHECK(script.events()[4].kind == ScriptEvent::Kind::Stick);
    CHECK(script.events()[4].lx == doctest::Approx(0.5f));
    CHECK(script.events()[4].ly == doctest::Approx(-0.5f));
    CHECK(script.events()[4].rx == doctest::Approx(1.0f));
    CHECK(script.events()[4].ry == doctest::Approx(-1.0f));

    CHECK(script.events()[5].kind == ScriptEvent::Kind::Shot);
    CHECK(script.events()[5].text == "menu-1");
    CHECK(script.events()[6].kind == ScriptEvent::Kind::Mark);
    CHECK(script.events()[6].text == "menu opens");  // CRLF and spaces trimmed
    CHECK(script.events()[7].kind == ScriptEvent::Kind::Quit);
    CHECK(script.duration() == doctest::Approx(0.7 + 2.5));
}

TEST_CASE("input_script parses every button name to the pad bit") {
    const auto parsed = parse(
        "0 press Cross\n"
        "0 press Circle\n"
        "0 press Square\n"
        "0 press Triangle\n"
        "0 press L1\n"
        "0 press R1\n"
        "0 press L2\n"
        "0 press R2\n"
        "0 press L3\n"
        "0 press R3\n"
        "0 press Options\n"
        "0 press Create\n"
        "0 press Up\n"
        "0 press Down\n"
        "0 press Left\n"
        "0 press Right\n");
    REQUIRE_MESSAGE(parsed.ok(), parsed.message());
    REQUIRE(parsed.value().events().size() == 16);
    const uint32_t expected[] = {
        splat::pad::kCross,    splat::pad::kCircle, splat::pad::kSquare,
        splat::pad::kTriangle, splat::pad::kL1,     splat::pad::kR1,
        splat::pad::kL2,       splat::pad::kR2,     splat::pad::kL3,
        splat::pad::kR3,       splat::pad::kOptions, splat::pad::kCreate,
        splat::pad::kUp,       splat::pad::kDown,   splat::pad::kLeft,
        splat::pad::kRight,
    };
    uint32_t seen = 0;
    for (size_t i = 0; i < 16; ++i) {
        CHECK(parsed.value().events()[i].button == expected[i]);
        CHECK((seen & expected[i]) == 0);  // one bit each, no duplicates
        seen |= expected[i];
    }
}

TEST_CASE("input_script pad bits are the viewer's one table") {
    // core/ui/pad_bits.h is the single table; pad_input.h re-exports it. If the
    // two ever drift, a script would press a different bit from a real pad.
    CHECK(viewer::pad::kCross == splat::pad::kCross);
    CHECK(viewer::pad::kCircle == splat::pad::kCircle);
    CHECK(viewer::pad::kSquare == splat::pad::kSquare);
    CHECK(viewer::pad::kTriangle == splat::pad::kTriangle);
    CHECK(viewer::pad::kOptions == splat::pad::kOptions);
    CHECK(viewer::pad::kCreate == splat::pad::kCreate);
    CHECK(viewer::pad::kL1 == splat::pad::kL1);
    CHECK(viewer::pad::kR1 == splat::pad::kR1);
    CHECK(viewer::pad::kL2 == splat::pad::kL2);
    CHECK(viewer::pad::kR2 == splat::pad::kR2);
    CHECK(viewer::pad::kL3 == splat::pad::kL3);
    CHECK(viewer::pad::kR3 == splat::pad::kR3);
    CHECK(viewer::pad::kUp == splat::pad::kUp);
    CHECK(viewer::pad::kDown == splat::pad::kDown);
    CHECK(viewer::pad::kLeft == splat::pad::kLeft);
    CHECK(viewer::pad::kRight == splat::pad::kRight);
    CHECK(viewer::pad::kIntercepted == splat::pad::kIntercepted);
    // The d-pad bits are the PS5 word's (HLA-PSVR2 psvr2.c:386-389).
    CHECK(splat::pad::kUp == 0x00000010);
    CHECK(splat::pad::kRight == 0x00000020);
    CHECK(splat::pad::kDown == 0x00000040);
    CHECK(splat::pad::kLeft == 0x00000080);
}

TEST_CASE("input_script rejects bad lines with the line number") {
    struct Bad {
        const char* text;
        int line;
        const char* needle;
    };
    const Bad bad[] = {
        {"0.0 press Foo\n", 1, "unknown button 'Foo'"},
        {"0.0 jump Cross\n", 1, "unknown command 'jump'"},
        {"0.0 press\n", 1, "missing button"},
        {"0.0 press Cross now\n", 1, "too many arguments"},
        {"0.0 hold Cross\n", 1, "missing duration"},
        {"0.0 hold Cross 1.5 now\n", 1, "too many arguments"},
        {"0.0 hold Cross -1\n", 1, "must not be negative"},
        {"0.0 stick 0 0 0\n", 1, "missing stick values"},
        {"0.0 stick 0 0 0 2\n", 1, "out of range"},
        {"0.0 stick 0 0 0 zz\n", 1, "bad stick value 'zz'"},
        {"abc press Cross\n", 1, "bad time 'abc'"},
        {"-1 press Cross\n", 1, "must not be negative"},
        {"0.0 shot\n", 1, "missing name"},
        {"0.0 shot bad/name\n", 1, "bad shot name"},
        {"0.0 shot 0123456789012345678901234567890123456789012345678\n", 1, "bad shot name"},
        {"0.0 mark\n", 1, "missing text"},
        {"0.0 quit now\n", 1, "too many arguments"},
        {"0.0\n", 1, "missing command"},
        // Blank lines and comments do not shift the reported line.
        {"0.0 press Cross\n\n# note\n0.5 down Foo\n", 4, "unknown button 'Foo'"},
        // Time must not go backwards, though equal times are fine.
        {"1.0 press Cross\n2.0 press Circle\n1.5 press Square\n", 3, "time goes backwards"},
        {"0 press Cross\n0 press Circle\n", 0, nullptr},  // accepted: equal times
    };
    for (const Bad& item : bad) {
        const auto parsed = parse(item.text);
        if (item.line == 0) {
            CHECK_MESSAGE(parsed.ok(), item.text);
            continue;
        }
        REQUIRE_MESSAGE(!parsed.ok(), item.text);
        const std::string& message = parsed.message();
        CHECK_MESSAGE(has(message, "line " + std::to_string(item.line) + ": "), message);
        CHECK_MESSAGE(has(message, item.needle), message);
    }
}

TEST_CASE("input_script press lasts kPressSeconds then releases") {
    const auto parsed = parse("1.0 press Cross\n");
    REQUIRE_MESSAGE(parsed.ok(), parsed.message());
    ScriptPlayer player(parsed.value());
    CHECK_FALSE(player.finished());

    CHECK(player.advance(0.99).buttons == 0);
    CHECK(player.advance(1.0).buttons == splat::pad::kCross);
    CHECK(player.advance(1.05).buttons == splat::pad::kCross);
    CHECK(player.advance(1.0 + kPressSeconds - 0.001).buttons == splat::pad::kCross);
    CHECK(player.advance(1.0 + kPressSeconds).buttons == 0);
    CHECK(player.finished());
    // kPressSeconds is the 0.1 s the format promises.
    CHECK(kPressSeconds == doctest::Approx(0.1));
}

TEST_CASE("input_script hold lasts its duration and overlaps another hold") {
    // L1 from 1.0 to 1.5, R1 from 1.2 to 2.2: both are down between 1.2 and
    // 1.5, and one ending does not release the other.
    const auto parsed = parse("1.0 hold L1 0.5\n1.2 hold R1 1.0\n");
    REQUIRE_MESSAGE(parsed.ok(), parsed.message());
    ScriptPlayer player(parsed.value());

    CHECK(player.advance(1.0).buttons == splat::pad::kL1);
    CHECK(player.advance(1.2).buttons == (splat::pad::kL1 | splat::pad::kR1));
    CHECK(player.advance(1.49).buttons == (splat::pad::kL1 | splat::pad::kR1));
    CHECK(player.advance(1.5).buttons == splat::pad::kR1);
    CHECK(player.advance(2.19).buttons == splat::pad::kR1);
    CHECK(player.advance(2.2).buttons == 0);
    CHECK(player.finished());

    // A `hold` of kPressSeconds is exactly a `press`, frame for frame.
    const auto press = parse("1.0 press Cross\n");
    const auto hold = parse("1.0 hold Cross 0.1\n");
    REQUIRE(press.ok());
    REQUIRE(hold.ok());
    ScriptPlayer a(press.value());
    ScriptPlayer b(hold.value());
    for (const double t : {0.5, 0.99, 1.0, 1.05, 1.099, 1.1, 2.0}) {
        CHECK(a.advance(t).buttons == b.advance(t).buttons);
    }
}

TEST_CASE("input_script stick persists until the next stick line") {
    const auto parsed = parse("1.0 stick 0.5 -0.5 1 -1\n3.0 stick 0 0 0.25 0.75\n");
    REQUIRE_MESSAGE(parsed.ok(), parsed.message());
    ScriptPlayer player(parsed.value());

    ScriptFrame frame = player.advance(0.5);
    CHECK(frame.lx == 0.0f);
    CHECK(frame.ly == 0.0f);
    CHECK(frame.rx == 0.0f);
    CHECK(frame.ry == 0.0f);

    frame = player.advance(1.0);
    CHECK(frame.lx == doctest::Approx(0.5f));
    CHECK(frame.ly == doctest::Approx(-0.5f));
    CHECK(frame.rx == doctest::Approx(1.0f));
    CHECK(frame.ry == doctest::Approx(-1.0f));

    // Held, not a one-frame blip.
    frame = player.advance(2.9);
    CHECK(frame.lx == doctest::Approx(0.5f));
    CHECK(frame.ry == doctest::Approx(-1.0f));

    frame = player.advance(3.0);
    CHECK(frame.lx == 0.0f);
    CHECK(frame.ly == 0.0f);
    CHECK(frame.rx == doctest::Approx(0.25f));
    CHECK(frame.ry == doctest::Approx(0.75f));

    frame = player.advance(9.0);
    CHECK(frame.rx == doctest::Approx(0.25f));
}

TEST_CASE("input_script every event fires once across big time steps") {
    const auto parsed = parse(
        "1.0 shot a\n"
        "1.2 mark first\n"
        "2.0 shot b\n"
        "3.0 mark second\n"
        "3.5 shot c\n"
        "4.0 quit\n");
    REQUIRE_MESSAGE(parsed.ok(), parsed.message());

    // One advance past everything: a long frame (a scene load) must still
    // deliver every shot and mark, in script order, and the quit.
    ScriptPlayer player(parsed.value());
    const ScriptFrame frame = player.advance(10.0);
    REQUIRE(frame.shots.size() == 3);
    CHECK(frame.shots[0] == "a");
    CHECK(frame.shots[1] == "b");
    CHECK(frame.shots[2] == "c");
    REQUIRE(frame.marks.size() == 2);
    CHECK(frame.marks[0] == "first");
    CHECK(frame.marks[1] == "second");
    CHECK(frame.quit);
    CHECK(player.finished());

    // The same advance again fires nothing twice.
    const ScriptFrame again = player.advance(10.0);
    CHECK(again.shots.empty());
    CHECK(again.marks.empty());
    CHECK(again.quit);  // sticky: the run has ended
    CHECK(again.buttons == 0);

    // The same script in 60 Hz steps also gives each one-shot exactly once.
    ScriptPlayer stepwise(parsed.value());
    std::vector<std::string> shots;
    std::vector<std::string> marks;
    bool quit = false;
    for (int i = 0; i <= 500; ++i) {
        const ScriptFrame step = stepwise.advance(static_cast<double>(i) * 0.01);
        shots.insert(shots.end(), step.shots.begin(), step.shots.end());
        marks.insert(marks.end(), step.marks.begin(), step.marks.end());
        quit = quit || step.quit;
    }
    REQUIRE(shots.size() == 3);
    CHECK(shots[0] == "a");
    CHECK(shots[1] == "b");
    CHECK(shots[2] == "c");
    REQUIRE(marks.size() == 2);
    CHECK(quit);
    CHECK(stepwise.finished());
}

TEST_CASE("input_script shot and quit ordering") {
    const auto parsed = parse("1.0 shot first\n1.2 shot second\n1.5 quit\n");
    REQUIRE_MESSAGE(parsed.ok(), parsed.message());
    ScriptPlayer player(parsed.value());

    ScriptFrame frame = player.advance(1.0);
    REQUIRE(frame.shots.size() == 1);
    CHECK(frame.shots[0] == "first");
    CHECK_FALSE(frame.quit);

    frame = player.advance(1.4);
    REQUIRE(frame.shots.size() == 1);
    CHECK(frame.shots[0] == "second");
    CHECK_FALSE(frame.quit);

    frame = player.advance(1.5);
    CHECK(frame.shots.empty());
    CHECK(frame.quit);

    // The same steps as one long frame: the quit does not swallow the shots
    // before it, and the shots keep their order.
    ScriptPlayer one_shot(parsed.value());
    frame = one_shot.advance(2.0);
    REQUIRE(frame.shots.size() == 2);
    CHECK(frame.shots[0] == "first");
    CHECK(frame.shots[1] == "second");
    CHECK(frame.quit);

    // A shot on the same second as the quit is still reported (it is a line
    // before the quit, and stable ordering keeps it first).
    const auto same = parse("1.0 shot last\n1.0 quit\n");
    REQUIRE(same.ok());
    ScriptPlayer same_player(same.value());
    frame = same_player.advance(1.0);
    REQUIRE(frame.shots.size() == 1);
    CHECK(frame.shots[0] == "last");
    CHECK(frame.quit);
}

TEST_CASE("input_script playback is deterministic and repeatable") {
    const char* text =
        "0.2 down Down\n"
        "0.4 up Down\n"
        "1.0 press Cross\n"
        "1.5 hold L1 0.75\n"
        "1.6 stick -0.25 0.5 0.75 -1\n"
        "2.0 shot a\n"
        "2.5 mark after a\n"
        "3.0 shot b\n"
        "3.5 quit\n";
    const auto parsed = parse(text);
    REQUIRE_MESSAGE(parsed.ok(), parsed.message());

    const std::vector<double> steps = {0.1, 0.1, 0.5, 0.75, 1.2, 1.5, 1.6,
                                       1.9, 2.0, 2.5, 2.9, 3.0, 3.5, 4.0};
    const auto run = [&steps](const InputScript& script) {
        ScriptPlayer player(script);
        std::vector<ScriptFrame> frames;
        for (const double step : steps) frames.push_back(player.advance(step));
        return frames;
    };

    const std::vector<ScriptFrame> first = run(parsed.value());
    const std::vector<ScriptFrame> second = run(parsed.value());
    REQUIRE(first.size() == second.size());
    for (size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].buttons == second[i].buttons);
        CHECK(first[i].lx == second[i].lx);
        CHECK(first[i].ly == second[i].ly);
        CHECK(first[i].rx == second[i].rx);
        CHECK(first[i].ry == second[i].ry);
        CHECK(first[i].shots == second[i].shots);
        CHECK(first[i].marks == second[i].marks);
        CHECK(first[i].quit == second[i].quit);
    }

    // start() replays the whole script from zero on the same player.
    ScriptPlayer reused(parsed.value());
    for (const double step : steps) reused.advance(step);
    reused.start();
    CHECK_FALSE(reused.finished());
    CHECK(reused.advance(0.1).buttons == 0);
    const ScriptFrame replay = reused.advance(2.0);
    REQUIRE(replay.shots.size() == 1);
    CHECK(replay.shots[0] == "a");
}

TEST_CASE("input_script time never runs backwards") {
    const auto parsed = parse("1.0 shot a\n2.0 shot b\n");
    REQUIRE_MESSAGE(parsed.ok(), parsed.message());
    ScriptPlayer player(parsed.value());

    CHECK(player.advance(2.5).shots.size() == 2);
    // A smaller clock value is the previous time: no event fires twice, and the
    // held word does not rewind.
    const ScriptFrame rewound = player.advance(1.0);
    CHECK(rewound.shots.empty());
    CHECK(rewound.buttons == 0);
    CHECK(player.finished());
}

TEST_CASE("input_script finished after the last event and its holds") {
    // Empty and comment-only scripts are valid and already finished.
    for (const char* text : {"", "# nothing here\n\n"}) {
        const auto parsed = parse(text);
        REQUIRE_MESSAGE(parsed.ok(), parsed.message());
        CHECK(parsed.value().empty());
        CHECK(parsed.value().duration() == 0.0);
        ScriptPlayer player(parsed.value());
        CHECK(player.finished());
        CHECK(player.advance(5.0).buttons == 0);
    }

    const auto parsed = parse("0.5 down Cross\n1.5 up Cross\n2.0 hold L1 1.0\n");
    REQUIRE_MESSAGE(parsed.ok(), parsed.message());
    ScriptPlayer player(parsed.value());
    CHECK_FALSE(player.finished());
    player.advance(0.4);
    CHECK_FALSE(player.finished());  // the down is still ahead
    CHECK(player.advance(0.5).buttons == splat::pad::kCross);
    CHECK_FALSE(player.finished());  // held
    CHECK(player.advance(1.5).buttons == 0);
    CHECK_FALSE(player.finished());  // the hold at 2.0 is still ahead
    CHECK(player.advance(2.0).buttons == splat::pad::kL1);
    CHECK_FALSE(player.finished());  // the hold runs to 3.0
    CHECK(player.advance(2.99).buttons == splat::pad::kL1);
    CHECK(player.advance(3.0).buttons == 0);
    CHECK(player.finished());

    // A `down` with no `up` never ends, so the player never reports finished.
    const auto dangling = parse("0 down Cross\n");
    REQUIRE(dangling.ok());
    ScriptPlayer stuck(dangling.value());
    CHECK(stuck.advance(100.0).buttons == splat::pad::kCross);
    CHECK_FALSE(stuck.finished());
}
