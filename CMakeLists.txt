cmake_minimum_required(VERSION 3.21)
project(GitWallet VERSION 0.2.0 LANGUAGES CXX)

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

# --- SQLCipher (vcpkg): the encrypted database; its header is in its own folder, as a
# plain SQLite may sit beside it -------------------------------------------------
find_path(SQLCIPHER_INCLUDE_DIR NAMES sqlcipher/sqlite3.h REQUIRED)
find_library(SQLCIPHER_LIBRARY NAMES sqlcipher REQUIRED)

# Reused design-system / GUI kit from FlowSuite (style + widgets only) + our
# own chain/ (crypto) and ui/ pages. GLOB keeps the lists in step with the tree.
file(GLOB_RECURSE GW_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h")

option(GW_BUILD_SMOKE "Compile the --selftest/-smoke entry points" ON)

qt_add_executable(GitWallet WIN32
    ${GW_SOURCES}
    res/app.qrc
    res/app.rc
    res/app.manifest
)

target_include_directories(GitWallet PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${BOTAN_INCLUDE_DIR}"
    "${SECP256K1_INCLUDE_DIR}"
    "${SQLCIPHER_INCLUDE_DIR}"
)

# vcpkg builds secp256k1 as a static library only; on Windows its header then
# needs SECP256K1_STATIC, or the symbols are declared dllimport and fail to link.
target_compile_definitions(GitWallet PRIVATE SECP256K1_STATIC)

target_link_libraries(GitWallet PRIVATE
    Qt6::Widgets Qt6::Svg Qt6::Network
    "${BOTAN_LIBRARY}" "${SECP256K1_LIBRARY}" "${SQLCIPHER_LIBRARY}"
)

if(WIN32)
    # The exe starts from whichever folder it is built in: its Qt DLLs beside it and
    # the plugins in lib/plugins, where the embedded qt.conf (Prefix = lib) looks.
    find_program(GW_WINDEPLOYQT windeployqt REQUIRED
        HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
    add_custom_command(TARGET GitWallet POST_BUILD
        COMMAND "${GW_WINDEPLOYQT}" --release --no-translations --no-system-d3d-compiler
            --no-system-dxc-compiler --no-opengl-sw --no-compiler-runtime
            --plugindir "$<TARGET_FILE_DIR:GitWallet>/lib/plugins" "$<TARGET_FILE:GitWallet>"
        COMMENT "Deploying Qt beside GitWallet.exe"
        VERBATIM)

    # LEAN_AND_MEAN keeps rpcndr.h out (it #defines `small`, which breaks
    # tokens::Radii::small in the reused ThemeManager); NOMINMAX keeps
    # std::min/max usable.
    target_compile_definitions(GitWallet PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(GitWallet PRIVATE bcrypt crypt32 ws2_32 psapi dwmapi)
endif()
