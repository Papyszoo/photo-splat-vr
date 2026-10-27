// The screen stack and the registry of screen factories. Screens are created
// lazily by ScreenId, so the registry only needs the screens this build has;
// pushing an unregistered id reports "not available yet" instead of crashing
// (every screen brief after W7 adds one line to
// core/ui/screens/registry.cpp).
//
// on_enter() runs whenever a screen becomes the top of the stack: on push and
// replace, and on the screen a pop reveals (with the args it was pushed with).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ui/screen.h"

namespace splat {

using ScreenFactory = std::function<std::unique_ptr<Screen>()>;

class ScreenRegistry {
public:
    void add(ScreenId id, ScreenFactory factory);
    bool has(ScreenId id) const;
    // Null when the id was never registered.
    std::unique_ptr<Screen> create(ScreenId id) const;
    std::size_t size() const { return factories_.size(); }

private:
    std::map<ScreenId, ScreenFactory> factories_;
};

class ScreenStack {
public:
    explicit ScreenStack(const ScreenRegistry& registry);

    // The stack calls on_enter() through this context; UiApp sets it once.
    void set_context(UiContext* context) { context_ = context; }
    // Called with the screen's display name when push/replace names an
    // unregistered id. UiApp turns it into a toast.
    void set_unavailable_handler(std::function<void(const std::string&)> handler);

    // False when the id is unregistered; the stack is unchanged then.
    bool push(ScreenId id, const ScreenArgs& args = ScreenArgs{});
    // Replaces the top (pushes when empty). False and unchanged when the id is
    // unregistered.
    bool replace(ScreenId id, const ScreenArgs& args = ScreenArgs{});
    // Pops the top; false at the root (the app always keeps one screen).
    bool pop();
    // Pops until `depth` screens are left (at least one); on_enter() runs once,
    // on the screen that ends up on top. False when nothing was popped.
    bool pop_to(std::size_t depth);
    // Position of the topmost screen with `id` (0 = the root), or -1.
    int find(ScreenId id) const;

    Screen* top();
    const Screen* top() const;
    ScreenId top_id() const;
    bool empty() const { return entries_.empty(); }
    std::size_t size() const { return entries_.size(); }

    // Delegates to the top; empty/default when the stack is empty. An overlay
    // top (Screen::overlay) is drawn over the screen below it, dimmed, and
    // reports that screen's title.
    void update(UiContext& ctx, const UiInput& input);
    void draw(UiContext& ctx, UiCanvas& canvas, const Rect& content);
    std::vector<Hint> hints() const;
    std::string title() const;
    // Titles bottom to top, for the breadcrumb.
    std::vector<std::string> titles() const;

private:
    struct Entry {
        ScreenId id = ScreenId::Albums;
        ScreenArgs args;
        std::unique_ptr<Screen> screen;
    };

    void enter(Entry& entry);

    const ScreenRegistry& registry_;
    std::vector<Entry> entries_;
    UiContext* context_ = nullptr;
    std::function<void(const std::string&)> unavailable_;
};

}  // namespace splat
