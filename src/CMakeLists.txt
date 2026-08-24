add_executable(itch_stat itch_stat.cpp)
target_link_libraries(itch_stat PRIVATE hotpath hotpath_instrument)

add_executable(hotpath_env env_report.cpp)
target_link_libraries(hotpath_env PRIVATE hotpath hotpath_instrument)
