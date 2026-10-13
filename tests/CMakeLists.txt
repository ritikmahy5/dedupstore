# Test suites (TESTING.md section 3). Each family is one executable with one ctest label.

add_library(dds_test_support STATIC
  support/fs_conformance_env.cc
  support/random_data.cc
  support/run_dds.cc
  support/temp_dir.cc
  support/test_seed.cc
  support/tree_compare.cc
  support/tree_generator.cc)
target_compile_definitions(dds_test_support PRIVATE DDS_BINARY_PATH="$<TARGET_FILE:dds>")
target_include_directories(dds_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(dds_test_support PUBLIC
  DDS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  DDS_BLAKE3_TEST_VECTORS="${DDS_BLAKE3_TEST_VECTORS}")
target_link_libraries(dds_test_support PUBLIC dds_options dds_snapshot)

dds_add_gtest(dds_unit_tests
  LABEL unit
  SLOW_FILTER "*SlowTest.*"
  SOURCES
    unit/common/bounded_queue_test.cc
    unit/common/clock_test.cc
    unit/common/crc32c_test.cc
    unit/common/encoding_test.cc
    unit/common/ordered_mutex_test.cc
    unit/common/process_identity_test.cc
    unit/common/status_test.cc
    unit/common/strings_test.cc
    unit/common/test_hooks_test.cc
    unit/common/unique_fd_test.cc
    unit/chunk/chunk_reader_test.cc
    unit/chunk/fastcdc_chunker_test.cc
    unit/chunk/fixed_chunker_test.cc
    unit/chunk/gear_table_test.cc
    unit/cli/args_test.cc
    unit/cli/json_test.cc
    unit/cli/numbers_test.cc
    unit/fs/crash_sim_fs_test.cc
    unit/fs/posix_fs_test.cc
    unit/hash/blake3_test.cc
    unit/hash/fingerprint_test.cc
    unit/gc/gc_test.cc
    unit/index/chunk_index_test.cc
    unit/index/flat_table_test.cc
    unit/repl/frame_messages_test.cc
    unit/repl/simulated_link_test.cc
    unit/repo/backup_batch_test.cc
    unit/repo/backup_parent_test.cc
    unit/repo/backup_restore_test.cc
    unit/repo/backup_split_test.cc
    unit/repo/epoch_manager_test.cc
    unit/repo/index_checkpoint_test.cc
    unit/repo/ingest_session_test.cc
    unit/repo/repo_lock_test.cc
    unit/repo/repository_test.cc
    unit/repo/snapshot_store_test.cc
    unit/repo/verify_stats_test.cc
    unit/snapshot/restore_sink_test.cc
    unit/snapshot/source_tree_test.cc
    unit/store/container_reader_test.cc
    unit/store/container_set_test.cc
    unit/store/container_writer_test.cc
  LIBS dds_cli dds_repl dds_test_support)

dds_add_gtest(dds_model_tests
  LABEL model
  SLOW_FILTER "*SlowTest.*"
  SOURCES
    model/flat_table_model_test.cc
    model/repository_model_test.cc
  LIBS dds_gc dds_test_support)

dds_add_gtest(dds_crash_tests
  LABEL crash
  SLOW_FILTER "*SlowTest.*"
  SOURCES
    crash/crash_harness.cc
    crash/crash_test.cc
  LIBS dds_repl dds_test_support)

dds_add_gtest(dds_stress_tests
  LABEL stress
  SLOW_FILTER "*SlowTest.*"
  SOURCES
    stress/gc_interleaving_test.cc
    stress/gc_stress_test.cc
    stress/ingest_stress_test.cc
  LIBS dds_gc dds_test_support)

dds_add_gtest(dds_repl_tests
  LABEL repl
  SLOW_FILTER "*SlowTest.*"
  SOURCES
    repl/hostile_peer_test.cc
    repl/pruning_test.cc
    repl/replication_test.cc
    repl/tcp_test.cc
  LIBS dds_repl dds_test_support)

dds_add_gtest(dds_codec_tests
  LABEL codec
  SOURCES
    codec/container_format_test.cc
    codec/index_checkpoint_format_test.cc
    codec/lock_record_test.cc
    codec/recipe_manifest_test.cc
    codec/repo_config_test.cc
    codec/tree_test.cc
  LIBS dds_repo dds_test_support)

dds_add_gtest(dds_property_tests
  LABEL property
  SOURCES property/chunker_property_test.cc
  LIBS dds_chunk dds_test_support)

dds_add_gtest(dds_conformance_tests
  LABEL conformance
  SOURCES conformance/fs_conformance_test.cc
  LIBS dds_fs dds_test_support)

dds_add_gtest(dds_cli_tests
  LABEL cli
  SOURCES
    cli/cli_basic_test.cc
    cli/cli_chunk_stats_test.cc
    cli/cli_init_test.cc
    cli/cli_repl_test.cc
    cli/cli_snapshot_test.cc
  LIBS dds_repo dds_test_support)
add_dependencies(dds_cli_tests dds)
