cmake_minimum_required(VERSION 3.21)
project(GitWallet VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # build/compile_commands.json for single-file checks

find_package(Qt6 6.10 REQUIRED COMPONENTS Widgets Svg Network)

# --- Botan (vcpkg): headers under <prefix>/include/botan, lib botan-3 ----------
find_path(BOTAN_INCLUDE_DIR NAMES botan/version.h REQUIRED)
find_library(BOTAN_LIBRARY NAMES botan-3 botan botan-2 REQUIRED)

# --- secp256k1 (vcpkg) --------------------------------------------------------
find_path(SECP256K1_INCLUDE_DIR NAMES secp256k1.h REQUIRED)
find_library(SECP256K1_LIBRARY NAMES secp256k1 libsecp256k1 REQUIRED)

# Reused design-system / GUI kit from FlowSuite (style + widgets only) + our
# own chain/ (crypto) and ui/ pages. GLOB keeps the lists in step with the tree.
file(GLOB_RECURSE GW_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h")

option(GW_BUILD_SMOKE "Compile the --selftest/-smoke entry points" ON)

qt_add_executable(GitWallet WIN32
    ${GW_SOURCES}
    res/app.qrc
)

target_include_directories(GitWallet PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${BOTAN_INCLUDE_DIR}"
    "${SECP256K1_INCLUDE_DIR}"
)

# vcpkg builds secp256k1 as a static library only; on Windows its header then
# needs SECP256K1_STATIC, or the symbols are declared dllimport and fail to link.
target_compile_definitions(GitWallet PRIVATE SECP256K1_STATIC)

target_link_libraries(GitWallet PRIVATE
    Qt6::Widgets Qt6::Svg Qt6::Network
    "${BOTAN_LIBRARY}" "${SECP256K1_LIBRARY}"
)

if(WIN32)
    # LEAN_AND_MEAN keeps rpcndr.h out (it #defines `small`, which breaks
    # tokens::Radii::small in the reused ThemeManager); NOMINMAX keeps
    # std::min/max usable.
    target_compile_definitions(GitWallet PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(GitWallet PRIVATE bcrypt crypt32 ws2_32)
endif()
