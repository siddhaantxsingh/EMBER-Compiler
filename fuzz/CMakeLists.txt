# libFuzzer targets. Configure with:
#   CC=clang CXX=clang++ cmake -S . -B build-fuzz -G Ninja -DEMBER_FUZZ=ON -DEMBER_ASAN=ON -DEMBER_UBSAN=ON
foreach(t fuzz_lexer fuzz_compile fuzz_run fuzz_bytecode fuzz_mutate_bytecode)
  add_executable(${t} ${t}.cc)
  target_link_libraries(${t} PRIVATE ember_tools)
  target_compile_options(${t} PRIVATE -fsanitize=fuzzer)
  target_link_options(${t} PRIVATE -fsanitize=fuzzer)
endforeach()
