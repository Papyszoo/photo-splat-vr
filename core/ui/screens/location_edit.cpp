// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/location_edit.h"

#include <cstdlib>
#include <utility>

#include "ui/screen_stack.h"
#include "ui/text_fit.h"

namespace splat {
namespace {

std::string masked_text(const std::string& text) {
    // The password row shows one '*' per character, never the value.
    return std::string(text.size(), '*');
}

// The note the NFS rows carry: AUTH_SYS is the console's uid, and there is no
// password anywhere in the protocol (docs/network-locations.md).
constexpr const char* kNfsNote = "NFS uses the console's user id; there is no password";

// What the Test row says once the mount worker answers: the state word plus
// the share root, so a successful test names what it reached.
std::string ready_line(const LocationView& location) {
    std::string line = "Ready: ";
    line += location.protocol == "nfs" ? "NFS " : "SMB ";
    line += location.host;
    if (!location.share.empty()) line += "/" + location.share;
    if (!location.path.empty()) line += "/" + location.path;
    return line;
}

// A UID/GID/port typed on the panel keyboard: digits only, saturated rather
// than wrapping. Text with no digit at all keeps the previous value: a stray
// "abc" must never become 0, which on an NFS export is AUTH_SYS root.
uint32_t numeric_field(const std::string& text, uint32_t previous, uint32_t maximum) {
    if (text.find_first_of("0123456789") == std::string::npos) return previous;
    uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') continue;
        value = value * 10 + static_cast<uint64_t>(ch - '0');
        if (value >= maximum) return maximum;
    }
    return static_cast<uint32_t>(value);
}

}  // namespace

const char* LocationEditScreen::field_label(Field field) {
    switch (field) {
        case Field::Protocol: return "Protocol";
        case Field::Host: return "Host";
        case Field::Port: return "Port";
        case Field::Share: return "Share";
        case Field::Path: return "Path";
        case Field::User: return "User";
        case Field::Password: return "Password";
        case Field::Encrypt: return "Encrypt";
        case Field::Uid: return "UID";
        case Field::Gid: return "GID";
        case Field::NfsVersion: return "Version";
        case Field::PhotoSource: return "Photos tab";
        case Field::SplatLibrary: return "Splat library";
        case Field::Test: return "Test";
        case Field::Save: return "Save";
    }
    return "";
}

void LocationEditScreen::on_enter(UiContext& ctx, const ScreenArgs& args) {
    // on_enter runs again when the keyboard pops: the draft keeps what was
    // typed, so only a different location argument re-loads it.
    if (!initialised_ || args.a != entered_id_) {
        entered_id_ = args.a;
        initialised_ = true;
        testing_ = false;
        existing_ = !args.a.empty();
        draft_ = LocationView{};
        draft_.protocol = "smb";
        draft_.domain = "WORKGROUP";
        // Added from the picker (stage U1 fix): the new share is a photo source
        // unless the owner turns the row off, because the tab it came from is
        // where photos are looked for. Without this the saved location had no
        // Photos tab at all.
        if (!existing_ && args.b == kPickSourceMode) draft_.photo_source = true;
        if (existing_ && ctx.svc.locations != nullptr) {
            for (const LocationView& location : ctx.svc.locations->list()) {
                if (location.id != args.a) continue;
                draft_ = location;
                draft_.state.clear();
                break;
            }
        }
    }
    validity_.clear();
    refresh_rows();
}

void LocationEditScreen::refresh_rows() {
    const bool smb = draft_.protocol != "nfs";
    std::vector<Field> fields = {Field::Protocol, Field::Host, Field::Port, Field::Share,
                                 Field::Path};
    if (smb) {
        fields.push_back(Field::User);
        fields.push_back(Field::Password);
        fields.push_back(Field::Encrypt);
    } else {
        // NFS has no password (AUTH_SYS): uid, gid and the NFS version are the
        // whole credential and the whole protocol choice.
        fields.push_back(Field::Uid);
        fields.push_back(Field::Gid);
        fields.push_back(Field::NfsVersion);
    }
    fields.push_back(Field::PhotoSource);
    fields.push_back(Field::SplatLibrary);
    fields.push_back(Field::Test);
    fields.push_back(Field::Save);
    fields_ = fields;

    const int focus = list_.focus();
    std::vector<ListRow> rows;
    rows.reserve(fields.size());
    for (const Field field : fields) {
        ListRow row;
        row.label = field_label(field);
        switch (field) {
            case Field::Protocol:
                row.right = smb ? "SMB" : "NFS";
                break;
            case Field::Host:
                row.right = draft_.host;
                // The console's process has no name resolver: a host name
                // fails before any packet is sent (console pass 3, P1/P2), so
                // the screen says what works before the user types.
                row.sublabel = "Use the server's IP address; this console has no name service";
                break;
            case Field::Port:
                // A fixed port is what a server without rpcbind needs; empty is
                // the protocol default and the normal NAS setup.
                row.sublabel = "0 or empty = the protocol default (445 SMB, 2049 NFS)";
                row.right = draft_.port > 0 ? std::to_string(draft_.port) : std::string("Default");
                break;
            case Field::Share:
                row.label = smb ? "Share" : "Export";
                row.sublabel = smb ? "The share name on the server"
                                   : "The export path on the server, e.g. /export";
                row.right = draft_.share;
                break;
            case Field::Path:
                row.sublabel = "Folder inside the share; empty is its root";
                row.right = draft_.path;
                break;
            case Field::User:
                row.sublabel = "Empty is a guest connection";
                row.right = draft_.user;
                break;
            case Field::Password:
                row.sublabel = "Stored as plain text in locations.json";
                row.right = masked_text(draft_.password);
                break;
            case Field::Encrypt:
                row.sublabel = "SMB3 encryption (AES-128-CCM)";
                row.right = draft_.encrypt ? "On" : "Off";
                break;
            case Field::Uid:
            case Field::Gid:
                // AUTH_SYS carries exactly this number; 65534 is nobody.
                row.sublabel = "AUTH_SYS id the server sees (65534 = nobody)";
                row.right = std::to_string(field == Field::Uid ? draft_.uid : draft_.gid);
                break;
            case Field::NfsVersion:
                row.sublabel = "NFSv3 (export paths) or NFSv4 (a share root)";
                row.right = std::to_string(draft_.nfs_version);
                break;
            case Field::PhotoSource:
                row.sublabel = "Show a Photos tab for this share";
                row.right = draft_.photo_source ? "On" : "Off";
                break;
            case Field::SplatLibrary:
                row.sublabel = "Offer it as a library of scenes";
                row.right = draft_.splat_library ? "On" : "Off";
                break;
            case Field::Test:
                row.sublabel = "Save and mount now";
                break;
            case Field::Save:
                row.sublabel = existing_ ? "Update this location" : "Add and mount it";
                break;
        }
        rows.push_back(std::move(row));
    }
    list_.set_rows(std::move(rows));
    list_.set_focus(focus);
}

void LocationEditScreen::open_text(UiContext& ctx, Field field) {
    ScreenArgs args;
    args.a = "free-text";
    args.masked = field == Field::Password;
    args.max_length = field == Field::Path ? 256 : 128;
    switch (field) {
        case Field::Host: args.b = draft_.host; break;
        case Field::Port: args.b = std::to_string(draft_.port); break;
        case Field::Share: args.b = draft_.share; break;
        case Field::Path: args.b = draft_.path; break;
        case Field::User: args.b = draft_.user; break;
        case Field::Password: args.b = draft_.password; break;
        case Field::Uid: args.b = std::to_string(draft_.uid); break;
        case Field::Gid: args.b = std::to_string(draft_.gid); break;
        default: return;
    }
    if (field == Field::Uid || field == Field::Gid) args.max_length = 10;
    if (field == Field::Port) {
        args.max_length = 5;
        // L2: empty is the protocol default, so Backspace has to be able to
        // take the value back to it. Without this the keyboard's "type a
        // value" rule disables OK and the old port is impossible to clear.
        args.allow_empty = true;
        args.title = "0 = default";
    }
    // The keyboard pops itself after calling this; the draft is the state, so
    // on_enter does not reload it.
    args.on_text = [this, field](const std::string& text) {
        switch (field) {
            case Field::Host: draft_.host = text; break;
            case Field::Port:
                // Empty (the keyboard allows it for this field) means the
                // protocol default; text with no digit keeps the value so a
                // stray "abc" is never read as a port.
                draft_.port = text.empty()
                                  ? 0
                                  : static_cast<int>(numeric_field(
                                        text,
                                        static_cast<uint32_t>(draft_.port < 0 ? 0 : draft_.port),
                                        65535));
                break;
            case Field::Share: draft_.share = text; break;
            case Field::Path: draft_.path = text; break;
            case Field::User: draft_.user = text; break;
            case Field::Password: draft_.password = text; break;
            case Field::Uid: draft_.uid = numeric_field(text, draft_.uid, 4294967295u); break;
            case Field::Gid: draft_.gid = numeric_field(text, draft_.gid, 4294967295u); break;
            default: break;
        }
        refresh_rows();
    };
    ctx.nav.push(ScreenId::Keyboard, args);
}

void LocationEditScreen::cycle_protocol(int direction) {
    if (direction == 0) return;
    const bool to_nfs = draft_.protocol != "nfs";
    draft_.protocol = to_nfs ? "nfs" : "smb";
    if (to_nfs) {
        draft_.user.clear();
        draft_.password.clear();
        draft_.encrypt = false;
    } else {
        draft_.domain = draft_.domain.empty() ? std::string("WORKGROUP") : draft_.domain;
    }
    refresh_rows();
}

void LocationEditScreen::save(UiContext& ctx, bool then_test) {
    if (ctx.svc.locations == nullptr) {
        ctx.toast("Network locations are not available in this build");
        return;
    }
    if (draft_.host.empty()) {
        validity_ = "A location needs a host";
        ctx.toast(validity_);
        return;
    }
    if (draft_.share.empty()) {
        validity_ = "A location needs a share";
        ctx.toast(validity_);
        return;
    }

    Status status = Status::success();
    const bool adding = !existing_;
    if (adding) {
        std::string id;
        status = ctx.svc.locations->add(draft_, &id);
        if (status.ok()) {
            draft_.id = id;
            existing_ = true;
            entered_id_ = id;
        }
    } else {
        status = ctx.svc.locations->update(draft_);
    }
    if (!status.ok()) {
        validity_ = status.message();
        ctx.toast(validity_);
        return;
    }
    ctx.toast((adding ? "Added " : "Saved ") + draft_.id);
    if (then_test) {
        const Status started = ctx.svc.locations->mount(draft_.id);
        if (!started.ok()) {
            validity_ = started.message();
            ctx.toast(validity_);
            return;
        }
        testing_ = true;
        validity_ = "Testing \xE2\x80\xA6";
        return;
    }
    ctx.nav.pop();
}

void LocationEditScreen::activate(UiContext& ctx, int row) {
    if (row < 0 || row >= static_cast<int>(fields_.size())) return;
    switch (fields_[static_cast<std::size_t>(row)]) {
        case Field::Protocol: cycle_protocol(1); break;
        case Field::Host:
        case Field::Port:
        case Field::Share:
        case Field::Path:
        case Field::User:
        case Field::Password:
            open_text(ctx, fields_[static_cast<std::size_t>(row)]);
            break;
        case Field::Encrypt: draft_.encrypt = !draft_.encrypt; refresh_rows(); break;
        case Field::Uid:
        case Field::Gid:
            open_text(ctx, fields_[static_cast<std::size_t>(row)]);
            break;
        case Field::NfsVersion:
            draft_.nfs_version = draft_.nfs_version == 4 ? 3 : 4;
            refresh_rows();
            break;
        case Field::PhotoSource:
            draft_.photo_source = !draft_.photo_source;
            refresh_rows();
            break;
        case Field::SplatLibrary:
            draft_.splat_library = !draft_.splat_library;
            refresh_rows();
            break;
        case Field::Test: save(ctx, true); break;
        case Field::Save: save(ctx, false); break;
    }
}

void LocationEditScreen::update(UiContext& ctx, const UiInput& input) {
    // The mount is on the service's worker, so this screen has to drive its
    // tick as well: without it, Test said "Testing …" for the rest of the
    // visit, and the result was visible only after backing out to the list.
    if (ctx.svc.locations != nullptr) {
        ctx.svc.locations->tick(input.dt);
        if (testing_) {
            const std::string state =
                draft_.id.empty() ? std::string() : ctx.svc.locations->status(draft_.id);
            if (state.empty() || state == "mounting") {
                validity_ = "Testing \xE2\x80\xA6";
                ctx.invalidate();
            } else {
                testing_ = false;
                validity_ = state == "ready" ? ready_line(draft_) : state;
                if (ctx.svc.log != nullptr) {
                    ctx.svc.log("ui: location " + draft_.id + ": " + validity_);
                }
                ctx.invalidate();
            }
        }
    }
    if (ctx.content.empty()) return;
    const int focus = list_.focus();
    const Field field = field_at(focus);
    if ((input.pressed & (Left | Right)) != 0 && field == Field::Protocol) {
        cycle_protocol((input.pressed & Right) != 0 ? 1 : -1);
        return;
    }
    if ((input.pressed & (Left | Right)) != 0 && field == Field::NfsVersion) {
        draft_.nfs_version = draft_.nfs_version == 4 ? 3 : 4;
        refresh_rows();
        return;
    }
    if ((input.pressed & (Left | Right)) != 0 && field == Field::Encrypt) {
        draft_.encrypt = !draft_.encrypt;
        refresh_rows();
        return;
    }
    if ((input.pressed & (Left | Right)) != 0 && field == Field::PhotoSource) {
        draft_.photo_source = !draft_.photo_source;
        refresh_rows();
        return;
    }
    if ((input.pressed & (Left | Right)) != 0 && field == Field::SplatLibrary) {
        draft_.splat_library = !draft_.splat_library;
        refresh_rows();
        return;
    }

    list_.update(input, ctx.content, ctx.theme);
    if (!list_.activated()) return;
    list_.clear_activated();
    activate(ctx, list_.focus());
}

void LocationEditScreen::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    list_.draw(canvas, content, ctx.theme);
    int y = content.y + static_cast<int>(list_.rows().size()) * ctx.theme.row_height + 12;
    if (draft_.protocol == "nfs") {
        // The one thing a user looking for "Password" has to be told.
        canvas.text(content.x + 24, y, fit_text(kNfsNote, ctx.theme.body_size - 2, content.w - 32),
                    ctx.theme.body_size - 2, ctx.theme.dim_text);
        y += ctx.theme.body_size + 6;
    }
    if (!validity_.empty()) {
        canvas.text(content.x + 24, y, fit_text(validity_, ctx.theme.body_size - 2, content.w - 32),
                    ctx.theme.body_size - 2, ctx.theme.warning);
    }
}

std::vector<Hint> LocationEditScreen::hints() const {
    return {Hint{"Cross", "Edit"}, Hint{"Circle", "Back"}};
}

std::string LocationEditScreen::title() const {
    return existing_ ? "Edit location" : "Add location";
}

LocationEditScreen::Field LocationEditScreen::field_at(int row) const {
    if (row < 0 || row >= static_cast<int>(fields_.size())) return Field::Save;
    return fields_[static_cast<std::size_t>(row)];
}

}  // namespace splat
