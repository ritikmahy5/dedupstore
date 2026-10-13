dds_add_library(dds_fs
  SOURCES
    crash_sim_fs.cc
    fs.cc
    mem_fs.cc
    posix_fs.cc
  DEPS dds_common)
