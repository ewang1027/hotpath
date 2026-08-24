add_executable(itch_stat itch_stat.cpp)
target_link_libraries(itch_stat PRIVATE hotpath hotpath_instrument)

add_executable(hotpath_env env_report.cpp)
target_link_libraries(hotpath_env PRIVATE hotpath hotpath_instrument)

add_executable(extract_tape extract_tape.cpp)
target_link_libraries(extract_tape PRIVATE hotpath hotpath_instrument)

add_executable(book_crossval book_crossval.cpp)
target_link_libraries(book_crossval PRIVATE hotpath hotpath_instrument)
