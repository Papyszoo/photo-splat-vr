// One network location's fields (L3-C, NFS rows L4): protocol, host, port,
// share/export, path, user, password, encryption, the two roles (photo source
// / splat library), then Test and Save.
//
// Text rows push the panel keyboard in its "free-text" mode with on_text set to
// the field (the password row masked; the drawn field is one '*' per
// character). Cross on Protocol cycles SMB/NFS, on Encrypt toggles it. Save
// writes the store through ILocationService (a new location gets its smbN id
// there); Test saves first, then mounts on the worker so the Locations screen
// shows "ready" or "offline: ..." a moment later.
//
// The location's fields are the same ones core/vfs/location.h stores; this
// screen never names the file. For NFS the share row reads Export, User,
// Password and Encrypt are hidden, UID/GID/Version take their place and the
// screen says "NFS uses the console's user id; there is no password".
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "ui/screen.h"
#include "ui/services.h"
#include "ui/widgets.h"

namespace splat {

class LocationEditScreen : public Screen {
public:
    // The rows, in the order they are drawn. NFS hides User/Password/Encrypt
    // and shows Export, UID, GID and Version instead.
    enum class Field {
        Protocol,
        Host,
        Port,
        Share,
        Path,
        User,
        Password,
        Encrypt,
        Uid,
        Gid,
        NfsVersion,
        PhotoSource,
        SplatLibrary,
        Test,
        Save,
    };

    // ScreenArgs::b of a new location opened from the photo picker's
    // "+ Add network location" tab: that tab exists to pick photos from the
    // share, so the draft starts with "Photos tab" on. Saving a location whose
    // role was left off produced no tab and read like the add had failed.
    static constexpr const char* kPickSourceMode = "pick-photos";

    void on_enter(UiContext& ctx, const ScreenArgs& args) override;
    void update(UiContext& ctx, const UiInput& input) override;
    void draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) override;
    std::vector<Hint> hints() const override;
    std::string title() const override;

    // For the tests: the rows, the draft, and what Cross on a row does.
    const ListView& list() const { return list_; }
    const LocationView& draft() const { return draft_; }
    // The line under the fields: a validation error, "Testing …", or the
    // mount's answer ("Ready: SMB host/share", "offline: …").
    const std::string& status_text() const { return validity_; }
    void activate(UiContext& ctx, int row);
    Field field_at(int row) const;
    static const char* field_label(Field field);

private:
    void refresh_rows();
    void cycle_protocol(int direction);
    void open_text(UiContext& ctx, Field field);
    void save(UiContext& ctx, bool then_test);

    LocationView draft_;
    std::string entered_id_;
    bool initialised_ = false;
    bool existing_ = false;
    bool testing_ = false;  // Test was pressed; the next update reads the state
    std::string validity_;
    ListView list_;
    std::vector<Field> fields_;
};

}  // namespace splat
