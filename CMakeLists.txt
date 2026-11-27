cmake_minimum_required(VERSION 3.20)
project(atchim LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
# SQLite3::SQLite3 is the current name; older CMake versions expose SQLite::SQLite3.
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()
include(cmake/CPM.cmake)
CPMAddPackage(
    NAME ftxui
    VERSION 7.0.3
    URL https://codeload.github.com/ArthurSonzogni/FTXUI/tar.gz/f921fad208912747c17d129a8ef75ec7624b6eec
    URL_HASH SHA256=00e1426f5b13d7748af44925768cca3bc4f64aa3b02e4c82567fd85716911a5b
    EXCLUDE_FROM_ALL YES
    OPTIONS "FTXUI_BUILD_EXAMPLES OFF" "FTXUI_BUILD_DOCS OFF"
            "FTXUI_BUILD_TESTS OFF" "FTXUI_ENABLE_INSTALL OFF")
add_executable(atchim src/atchim.cpp src/picker.cpp)
target_link_libraries(atchim PRIVATE SQLite3::SQLite3 Threads::Threads ftxui::component ftxui::dom ftxui::screen)
add_executable(assh src/assh.cpp)
target_link_libraries(assh PRIVATE SQLite3::SQLite3 uv util)
foreach(target IN ITEMS atchim assh)
    target_include_directories(${target} PRIVATE include)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
endforeach()
