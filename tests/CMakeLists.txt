add_executable(difftest difftest.cc)
target_link_libraries(difftest PRIVATE ember_tools)
target_compile_options(difftest PRIVATE ${EMBER_WARNINGS})
add_test(NAME difftest COMMAND difftest --seeds 300)

# GCC's -Wnull-dereference reports false positives inside libstdc++ at -O3
# for the test code's vector manipulation; keep it for the libraries only.
set(EMBER_TEST_WARNINGS ${EMBER_WARNINGS})
list(REMOVE_ITEM EMBER_TEST_WARNINGS -Wnull-dereference)
target_compile_options(difftest PRIVATE -Wno-null-dereference)

file(GLOB UNIT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cc)
add_executable(unit_tests ${UNIT_SOURCES})
target_link_libraries(unit_tests PRIVATE ember_tools)
target_compile_options(unit_tests PRIVATE ${EMBER_TEST_WARNINGS})
add_test(NAME unit COMMAND unit_tests)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME golden COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_golden.py $<TARGET_FILE:ember>)
  add_test(NAME cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_tests.py $<TARGET_FILE:ember>)
endif()

if(EMBER_HAVE_LLVM)
  # Native backend vs VM on every test/bench/example program (exact stdout,
  # stderr and exit status), and on generated programs vs the reference
  # interpreter.
  if(Python3_FOUND)
    add_test(NAME native_programs COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/native_difftest.py $<TARGET_FILE:ember>)
  endif()
  add_test(NAME native_difftest COMMAND difftest --seeds 40 --start 500000 --native)
endif()
