add_executable(bench_book bench_book.cpp)
target_link_libraries(bench_book PRIVATE hotpath hotpath_instrument)

add_executable(bench_ring bench_ring.cpp)
target_link_libraries(bench_ring PRIVATE hotpath)

add_executable(bench_parse bench_parse.cpp)
target_link_libraries(bench_parse PRIVATE hotpath)
