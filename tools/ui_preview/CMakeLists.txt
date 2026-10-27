# ui_preview: draws a UiCanvas pattern and writes it as a PNG, so a panel
# screen can be judged on the desktop without the console (see
# tools/ui_preview/patterns.cpp for the pattern registry).
add_executable(ui_preview main.cpp patterns.cpp test_pattern.cpp)
target_link_libraries(ui_preview PRIVATE splat_core)
