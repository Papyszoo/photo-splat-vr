#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "scene/scene.h"

TEST_CASE("core reports a version") {
    CHECK(splat::core_version() != nullptr);
    CHECK(splat::core_version()[0] != '\0');
}
