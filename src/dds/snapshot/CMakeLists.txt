dds_add_library(dds_snapshot
  SOURCES
    manifest.cc
    mem_node.cc
    names.cc
    recipe.cc
    restore_sink.cc
    source_tree.cc
    tree.cc
  DEPS dds_index)
