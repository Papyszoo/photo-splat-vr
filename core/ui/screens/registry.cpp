// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/registry.h"

#include <memory>

#include "ui/screens/album_pick.h"
#include "ui/screens/albums.h"
#include "ui/screens/convert_dialog.h"
#include "ui/screens/keyboard.h"
#include "ui/screens/licence.h"
#include "ui/screens/models.h"
#include "ui/screens/photos.h"
#include "ui/screens/queue.h"
#include "ui/screens/scenes.h"
#include "ui/screens/settings.h"

namespace splat {

void register_default_screens(ScreenRegistry& registry) {
    registry.add(ScreenId::Albums, [] { return std::make_unique<AlbumsScreen>(); });
    registry.add(ScreenId::Scenes, [] { return std::make_unique<ScenesScreen>(); });
    registry.add(ScreenId::Photos, [] { return std::make_unique<PhotosScreen>(); });
    registry.add(ScreenId::ConvertDialog, [] { return std::make_unique<ConvertDialog>(); });
    registry.add(ScreenId::Settings, [] { return std::make_unique<SettingsScreen>(); });
    registry.add(ScreenId::Queue, [] { return std::make_unique<QueueScreen>(); });
    registry.add(ScreenId::Keyboard, [] { return std::make_unique<KeyboardScreen>(); });
    registry.add(ScreenId::AlbumPick, [] { return std::make_unique<AlbumPickScreen>(); });
    registry.add(ScreenId::Models, [] { return std::make_unique<ModelsScreen>(); });
    registry.add(ScreenId::Licence, [] { return std::make_unique<LicenceScreen>(); });
}

}  // namespace splat
