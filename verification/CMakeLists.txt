# Verification harness: fuzzy driver + simulated lossy network + checker.
add_executable(color_verify src/fuzz_main.cpp)
target_include_directories(color_verify PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(color_verify PRIVATE color_core)

# A fast default run, wired into `ctest`, that fails the build on any violation.
add_test(NAME color_verify_smoke
         COMMAND color_verify --seeds 30 --steps 1500)

# A harsher run: high loss + duplication to stress retransmission and dedup.
add_test(NAME color_verify_lossy
         COMMAND color_verify --seeds 30 --steps 1500 --drop 0.5 --dup 0.3)
