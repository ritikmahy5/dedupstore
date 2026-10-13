dds_add_library(dds_cli
  SOURCES
    args.cc
    chunker_flags.cc
    cli.cc
    cmd_backup.cc
    cmd_chunk_stats.cc
    cmd_gc.cc
    cmd_init.cc
    cmd_ls.cc
    cmd_replicate.cc
    cmd_restore.cc
    cmd_rm.cc
    cmd_serve.cc
    cmd_snapshots.cc
    cmd_stats.cc
    cmd_verify.cc
    commands.cc
    format.cc
    json.cc
    numbers.cc
    repo_access.cc
  DEPS dds_repl)
target_include_directories(dds_cli PUBLIC "${PROJECT_BINARY_DIR}/generated")

add_executable(dds main.cc)
target_link_libraries(dds PRIVATE dds_cli)
set_target_properties(dds PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
