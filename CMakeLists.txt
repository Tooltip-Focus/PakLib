cmake_minimum_required(VERSION 3.25)

project(paklib VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "paklib currently supports Windows only")
endif()

option(PAKLIB_BUILD_TOOLS "Build paktool" ON)
option(PAKLIB_BUILD_TESTS "Build GoogleTest tests" OFF)
option(PAKLIB_BUILD_BENCHMARKS "Build Google Benchmark benchmarks" OFF)
option(PAKLIB_ENABLE_INSTALL "Enable install targets" ON)
option(PAKLIB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(PAKLIB_ENABLE_IPO "Enable interprocedural optimization in Release" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PakCompilerOptions)

if(MSVC)
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

find_package(zstd CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)

if(TARGET zstd::libzstd_static)
    set(PAKLIB_ZSTD_TARGET zstd::libzstd_static)
elseif(TARGET zstd::libzstd_shared)
    set(PAKLIB_ZSTD_TARGET zstd::libzstd_shared)
elseif(TARGET zstd::libzstd)
    set(PAKLIB_ZSTD_TARGET zstd::libzstd)
else()
    message(FATAL_ERROR "No supported zstd CMake target was found")
endif()

set(PAKLIB_READER_SOURCES
    src/common/Path.cpp
    src/common/Win32IO.cpp
    src/reader/MappedView.cpp
    src/reader/Reader.cpp
)
set(PAKLIB_WRITER_SOURCES
    src/writer/Writer.cpp
)

# Static library with the shared include layout, zstd/xxHash and compiler options.
function(pak_add_library target export_name)
    add_library(${target} STATIC ${ARGN})
    add_library(pak::${export_name} ALIAS ${target})
    set_target_properties(${target} PROPERTIES EXPORT_NAME ${export_name})
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(${target} PRIVATE ${PAKLIB_ZSTD_TARGET} xxHash::xxhash)
    pak_apply_compiler_options(${target})
endfunction()

pak_add_library(pak_reader reader ${PAKLIB_READER_SOURCES})
pak_add_library(pak_writer writer ${PAKLIB_WRITER_SOURCES})
target_link_libraries(pak_writer PUBLIC pak_reader)
pak_add_library(pak_c_api c_api ${PAKLIB_READER_SOURCES} ${PAKLIB_WRITER_SOURCES} src/cabi/CAbi.cpp)

if(PAKLIB_BUILD_TOOLS)
    find_package(CLI11 CONFIG REQUIRED)
    add_executable(paktool tools/paktool/main.cpp)
    target_link_libraries(paktool PRIVATE pak::writer CLI11::CLI11)
    pak_apply_compiler_options(paktool ENABLE_EXCEPTIONS)
endif()

if(PAKLIB_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    add_executable(pak_tests
        tests/AssetTests.cpp
        tests/CAbiTests.cpp
        tests/ReaderTests.cpp
        tests/WriterTests.cpp
        tests/ConcurrencyTests.cpp
    )
    target_link_libraries(pak_tests PRIVATE pak::c_api GTest::gtest_main)
    target_compile_features(pak_tests PRIVATE cxx_std_20)
    target_compile_definitions(pak_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(pak_tests PRIVATE /EHsc)
    endif()
    include(GoogleTest)
    gtest_discover_tests(pak_tests)
endif()

if(PAKLIB_BUILD_BENCHMARKS)
    find_package(benchmark CONFIG REQUIRED)
    find_package(libzip CONFIG REQUIRED)
    add_executable(pak_bench
        benchmarks/BenchmarkMain.cpp
    )
    target_link_libraries(pak_bench PRIVATE
        pak::writer
        benchmark::benchmark
        libzip::zip
    )
    target_compile_features(pak_bench PRIVATE cxx_std_20)
    target_compile_definitions(pak_bench PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(pak_bench PRIVATE /EHsc)
    endif()
endif()

if(PAKLIB_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT PAKLIB_IPO_SUPPORTED)
    if(PAKLIB_IPO_SUPPORTED)
        # pak_c_api stays out.
        set_property(TARGET pak_reader pak_writer PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        if(TARGET paktool)
            set_property(TARGET paktool PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        endif()
    endif()
endif()

if(PAKLIB_ENABLE_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    install(TARGETS pak_reader pak_writer pak_c_api
        EXPORT paklibTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT paklibTargets
        NAMESPACE pak::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/paklib
    )
    configure_package_config_file(
        cmake/PaklibConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/paklibConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/paklib
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/paklibConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/paklibConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/paklibConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/paklib
    )
endif()
