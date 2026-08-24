add_executable(itch_stat itch_stat.cpp)
target_link_libraries(itch_stat PRIVATE hotpath hotpath_instrument)

add_executable(hotpath_env env_report.cpp)
target_link_libraries(hotpath_env PRIVATE hotpath hotpath_instrument)

add_executable(extract_tape extract_tape.cpp)
target_link_libraries(extract_tape PRIVATE hotpath hotpath_instrument)

add_executable(book_crossval book_crossval.cpp)
target_link_libraries(book_crossval PRIVATE hotpath hotpath_instrument)

add_executable(litmus_ring litmus_ring.cpp)
target_link_libraries(litmus_ring PRIVATE hotpath)

add_executable(sim_mm sim_mm.cpp)
target_link_libraries(sim_mm PRIVATE hotpath hotpath_instrument)

add_executable(pipeline pipeline.cpp)
target_link_libraries(pipeline PRIVATE hotpath hotpath_instrument)

add_executable(latency_sweep latency_sweep.cpp)
target_link_libraries(latency_sweep PRIVATE hotpath hotpath_instrument)

add_executable(tape_stat tape_stat.cpp)
target_link_libraries(tape_stat PRIVATE hotpath hotpath_instrument)
