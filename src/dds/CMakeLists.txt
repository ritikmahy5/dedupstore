# One static library per module, linked only to modules on its left in ARCHITECTURE.md section 1
# (common <- fs <- hash <- chunk <- store <- index <- snapshot <- repo <- gc <- repl <- cli).
add_subdirectory(common)
add_subdirectory(fs)
add_subdirectory(hash)
add_subdirectory(chunk)
add_subdirectory(store)
add_subdirectory(index)
add_subdirectory(snapshot)
add_subdirectory(repo)
add_subdirectory(gc)
add_subdirectory(repl)

configure_file(version.h.in "${PROJECT_BINARY_DIR}/generated/dds/version.h" @ONLY)
add_subdirectory(cli)
