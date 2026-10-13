dds_add_library(dds_chunk
  SOURCES
    chunk_reader.cc
    chunker.cc
    fastcdc_chunker.cc
    fd_byte_source.cc
    fixed_chunker.cc
    gear_table.cc
  DEPS dds_hash)
