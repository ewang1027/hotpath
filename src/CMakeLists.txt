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

add_executable(signal_study signal_study.cpp)
target_link_libraries(signal_study PRIVATE hotpath hotpath_instrument)

add_executable(strategy_eval strategy_eval.cpp)
target_link_libraries(strategy_eval PRIVATE hotpath hotpath_instrument)

add_executable(shm_pub shm_pub.cpp)
target_link_libraries(shm_pub PRIVATE hotpath hotpath_instrument)

add_executable(shm_sub shm_sub.cpp)
target_link_libraries(shm_sub PRIVATE hotpath hotpath_instrument)
