// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screen_stack.h"

#include <utility>

namespace splat {

void ScreenRegistry::add(ScreenId id, ScreenFactory factory) {
    factories_[id] = std::move(factory);
}

bool ScreenRegistry::has(ScreenId id) const { return factories_.find(id) != factories_.end(); }

std::unique_ptr<Screen> ScreenRegistry::create(ScreenId id) const {
    const auto found = factories_.find(id);
    if (found == factories_.end() || !found->second) return nullptr;
    return found->second();
}

ScreenStack::ScreenStack(const ScreenRegistry& registry) : registry_(registry) {}

void ScreenStack::set_unavailable_handler(std::function<void(const std::string&)> handler) {
    unavailable_ = std::move(handler);
}

void ScreenStack::enter(Entry& entry) {
    if (context_ == nullptr) return;
    // Copy first: on_enter may push or replace, which can reallocate entries_.
    Screen* screen = entry.screen.get();
    const ScreenArgs args = entry.args;
    screen->on_enter(*context_, args);
}

bool ScreenStack::push(ScreenId id, const ScreenArgs& args) {
    std::unique_ptr<Screen> screen = registry_.create(id);
    if (!screen) {
        if (unavailable_) unavailable_(screen_id_name(id));
        return false;
    }
    Entry entry;
    entry.id = id;
    entry.args = args;
    entry.screen = std::move(screen);
    entries_.push_back(std::move(entry));
    enter(entries_.back());
    return true;
}

bool ScreenStack::replace(ScreenId id, const ScreenArgs& args) {
    std::unique_ptr<Screen> screen = registry_.create(id);
    if (!screen) {
        if (unavailable_) unavailable_(screen_id_name(id));
        return false;
    }
    Entry entry;
    entry.id = id;
    entry.args = args;
    entry.screen = std::move(screen);
    if (entries_.empty()) {
        entries_.push_back(std::move(entry));
    } else {
        entries_.back() = std::move(entry);
    }
    enter(entries_.back());
    return true;
}

bool ScreenStack::pop() {
    if (entries_.size() <= 1) return false;
    entries_.pop_back();
    enter(entries_.back());
    return true;
}

bool ScreenStack::pop_to(std::size_t depth) {
    if (depth < 1) depth = 1;
    if (entries_.size() <= depth) return false;
    entries_.resize(depth);
    enter(entries_.back());
    return true;
}

int ScreenStack::find(ScreenId id) const {
    for (std::size_t index = entries_.size(); index > 0; --index) {
        if (entries_[index - 1].id == id) return static_cast<int>(index - 1);
    }
    return -1;
}

Screen* ScreenStack::top() { return entries_.empty() ? nullptr : entries_.back().screen.get(); }

const Screen* ScreenStack::top() const {
    return entries_.empty() ? nullptr : entries_.back().screen.get();
}

ScreenId ScreenStack::top_id() const {
    // The root is the folder browser: UiApp keeps one screen and starts there.
    return entries_.empty() ? ScreenId::Albums : entries_.back().id;
}

void ScreenStack::update(UiContext& ctx, const UiInput& input) {
    if (Screen* screen = top()) screen->update(ctx, input);
}

void ScreenStack::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    Screen* screen = top();
    if (screen == nullptr) return;
    if (screen->overlay() && entries_.size() >= 2) {
        // A dialog: the screen below, dimmed, then the dialog on top.
        entries_[entries_.size() - 2].screen->draw(ctx, canvas, content);
        canvas.fill_rect(0, 0, static_cast<int>(canvas.width()), static_cast<int>(canvas.height()),
                         ctx.theme.veil);
    }
    screen->draw(ctx, canvas, content);
}

std::vector<Hint> ScreenStack::hints() const {
    const Screen* screen = top();
    return screen == nullptr ? std::vector<Hint>{} : screen->hints();
}

std::string ScreenStack::title() const {
    const Screen* screen = top();
    if (screen == nullptr) return std::string{};
    // A dialog keeps the title of the screen it is drawn over.
    if (screen->overlay() && entries_.size() >= 2) {
        return entries_[entries_.size() - 2].screen->title();
    }
    return screen->title();
}

std::vector<std::string> ScreenStack::titles() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const Entry& entry : entries_) out.push_back(entry.screen->title());
    return out;
}

}  // namespace splat
