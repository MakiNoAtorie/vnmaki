# libFuzzer targets for loaders of untrusted data (docs/SPEC.md section 5).
# Enabled with VNMAKI_BUILD_FUZZERS=ON under LLVM Clang (the `fuzz` preset).
#
# vnmaki_add_fuzzer(<name> <source>...) builds fuzz_<name> and registers a
# short ctest smoke run so every fuzz target is exercised in CI.
function(vnmaki_add_fuzzer name)
  set(target fuzz_${name})
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE vnmaki::engine vnmaki::options)
  target_link_options(${target} PRIVATE -fsanitize=fuzzer)
  add_test(NAME fuzz.${name}.smoke COMMAND ${target} -runs=10000 -seed=1)
endfunction()

vnmaki_add_fuzzer(noop noop_fuzzer.cpp)
vnmaki_add_fuzzer(vfs_path vfs_path_fuzzer.cpp)
