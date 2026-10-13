dds_add_library(dds_repl
  SOURCES
    frame.cc
    messages.cc
    receiver.cc
    sender.cc
    server.cc
    simulated_link.cc
    tcp.cc
  DEPS dds_gc)
