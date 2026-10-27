# splat_ingest_server: the C9 LAN upload page without the rest of the app.
#
# Prints the PIN, serves the page and, with --state, runs the existing
# InboxWatcher + JobQueue so an uploaded photo shows up in /api/queue. It is for
# manual testing on a desktop; the console app reuses core/ingest directly.
add_executable(splat_ingest_server main.cpp)
target_link_libraries(splat_ingest_server PRIVATE splat_core)

find_package(Threads REQUIRED)
target_link_libraries(splat_ingest_server PRIVATE Threads::Threads)
