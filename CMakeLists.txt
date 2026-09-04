cmake_minimum_required(VERSION 3.20)
project(copper_options_montecarlo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(copper_mc src/main.cpp)
target_include_directories(copper_mc PRIVATE include)

if(MSVC)
    # MSVC's Parallel STL (used for std::execution::par_unseq in
    # MonteCarloEngine.h) ships in the CRT -- no extra library to link.
    target_compile_options(copper_mc PRIVATE /W4 /EHsc /O2)
else()
    # libstdc++'s <execution> parallel policies require linking Intel TBB
    # (or oneTBB); without it, GCC/Clang either fail to link or silently
    # execute the "parallel" policy sequentially, depending on version.
    # Look for it and warn clearly if it isn't there, rather than let the
    # build fail on an opaque linker error.
    target_compile_options(copper_mc PRIVATE -Wall -Wextra -O3)
    find_package(TBB QUIET)
    if(TBB_FOUND)
        target_link_libraries(copper_mc PRIVATE TBB::tbb)
    else()
        message(WARNING
            "TBB not found: std::execution::par_unseq in MonteCarloEngine.h "
            "needs it on GCC/Clang (libstdc++/libc++'s Parallel STL backend). "
            "Install it (e.g. 'apt install libtbb-dev' / 'brew install tbb') "
            "and re-run cmake, or the parallel path may fail to link.")
    endif()
endif()

enable_testing()

# --self-test prices against the Kemna-Vorst closed form and checks Heston
# put-call parity end to end; the two suites below cover the pieces underneath
# it (exact averaging/payoff/closed-form arithmetic) and the engine's
# statistical properties (reproducibility, variance reduction, 1/sqrt(N) decay).
add_test(NAME self_test COMMAND copper_mc --self-test)

foreach(test_name test_pricing_math test_engine_properties)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_include_directories(${test_name} PRIVATE include tests)
    if(MSVC)
        target_compile_options(${test_name} PRIVATE /W4 /EHsc /O2)
    else()
        target_compile_options(${test_name} PRIVATE -Wall -Wextra -O3)
        if(TBB_FOUND)
            target_link_libraries(${test_name} PRIVATE TBB::tbb)
        endif()
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
