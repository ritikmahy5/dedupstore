# Fuzz targets (TESTING.md 3.3 step 4; D34). Each target is one fuzz_<name>.cc defining
# LLVMFuzzerTestOneInput, with a seed corpus in corpus/<name>/.
# - fuzz preset (DDS_FUZZ): a libFuzzer binary; ctest runs it over its corpus once.
# - every other preset with tests: the same source linked with replay_main.cc, so the corpus is
#   replayed under the preset's sanitizers (label fuzz-replay).

set(DDS_FUZZ_TARGETS checkpoint config container decompress frame lock manifest message recipe
  tree)

foreach(name IN LISTS DDS_FUZZ_TARGETS)
  set(corpus "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name}")
  if(DDS_FUZZ)
    add_executable(fuzz_${name} fuzz_${name}.cc)
    target_link_libraries(fuzz_${name} PRIVATE dds_repl)
    target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
    add_test(NAME fuzz.${name} COMMAND fuzz_${name} -runs=0 "${corpus}")
    set_tests_properties(fuzz.${name} PROPERTIES LABELS fuzz-replay)
  elseif(DDS_BUILD_TESTS)
    add_executable(fuzz_${name}_replay fuzz_${name}.cc replay_main.cc)
    target_link_libraries(fuzz_${name}_replay PRIVATE dds_repl)
    add_test(NAME fuzz_replay.${name} COMMAND fuzz_${name}_replay "${corpus}")
    set_tests_properties(fuzz_replay.${name} PROPERTIES LABELS fuzz-replay)
  endif()
endforeach()
