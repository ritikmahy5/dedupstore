dds_add_library(dds_store
  SOURCES
    compression.cc
    container_format.cc
    container_reader.cc
    container_set.cc
    container_writer.cc
  DEPS dds_chunk libzstd_static)
