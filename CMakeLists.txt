cmake_minimum_required(VERSION 3.20)
project(BinaryCoinCore VERSION 0.1.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
option(BINARYCOIN_REQUIRE_LIBSECP256K1 "Require libsecp256k1 instead of the test-only OpenSSL fallback" ON)
option(BINARYCOIN_STATIC_MSVC_RUNTIME "Link the static MSVC runtime for portable Windows packages" ON)

if(MSVC AND BINARYCOIN_STATIC_MSVC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(BINARYCOIN_SECP_TARGET "")
find_package(secp256k1 CONFIG QUIET)
find_package(libsecp256k1 CONFIG QUIET)
if(TARGET libsecp256k1::secp256k1)
    set(BINARYCOIN_SECP_TARGET libsecp256k1::secp256k1)
elseif(TARGET secp256k1::secp256k1)
    set(BINARYCOIN_SECP_TARGET secp256k1::secp256k1)
elseif(TARGET unofficial::secp256k1::secp256k1)
    set(BINARYCOIN_SECP_TARGET unofficial::secp256k1::secp256k1)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SECP256K1 QUIET IMPORTED_TARGET libsecp256k1)
        if(SECP256K1_FOUND)
            set(BINARYCOIN_SECP_TARGET PkgConfig::SECP256K1)
        endif()
    endif()
endif()

if(BINARYCOIN_REQUIRE_LIBSECP256K1 AND NOT BINARYCOIN_SECP_TARGET)
    message(FATAL_ERROR "libsecp256k1 is required. Install it with your package manager or vcpkg.")
endif()

add_library(binarycoin_core
    src/platform.cpp
    src/json.cpp
    src/logger.cpp
    src/rpc.cpp
    src/rpc_commands.cpp
    src/node_app.cpp
    src/hash.cpp
    src/pow.cpp
    src/serialize.cpp
    src/transaction.cpp
    src/merkle.cpp
    src/fees.cpp
    src/key.cpp
    src/mnemonic.cpp
    src/hdkey.cpp
    src/bech32.cpp
    src/script.cpp
    src/utxo.cpp
    src/hd_wallet_backend.cpp
    src/wallet_manager.cpp
    src/wallet_amount.cpp
    src/mempool.cpp
    src/block.cpp
    src/storage.cpp
    src/chain.cpp
    src/net.cpp
    src/bootstrap.cpp
    src/peer_store.cpp
    src/ban_store.cpp
    src/branch_store.cpp
)

target_include_directories(binarycoin_core PUBLIC src)
target_link_libraries(binarycoin_core PUBLIC OpenSSL::Crypto)
if(BINARYCOIN_SECP_TARGET)
    target_link_libraries(binarycoin_core PUBLIC ${BINARYCOIN_SECP_TARGET})
    target_compile_definitions(binarycoin_core PUBLIC BINARYCOIN_USE_LIBSECP256K1=1)
    message(STATUS "BinaryCoin secp256k1 backend: libsecp256k1")
else()
    message(WARNING "BinaryCoin secp256k1 backend: OpenSSL fallback (test build only)")
endif()

if(WIN32)
    target_compile_definitions(binarycoin_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(binarycoin_core PUBLIC ws2_32 advapi32)
endif()

function(binarycoin_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    endif()
endfunction()

binarycoin_warnings(binarycoin_core)
if(NOT MSVC)
    set_source_files_properties(src/key.cpp PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations")
endif()

add_executable(binarycoind src/main.cpp)
target_link_libraries(binarycoind PRIVATE binarycoin_core)
binarycoin_warnings(binarycoind)

add_executable(binarycoin-cli src/cli_main.cpp)
target_link_libraries(binarycoin-cli PRIVATE binarycoin_core)
binarycoin_warnings(binarycoin-cli)

install(TARGETS binarycoind binarycoin-cli RUNTIME DESTINATION .)

include(CTest)
if(BUILD_TESTING)
    add_executable(binarycoin_tests tests/testnet_tests.cpp)
    target_link_libraries(binarycoin_tests PRIVATE binarycoin_core)
    binarycoin_warnings(binarycoin_tests)
    add_test(NAME binarycoin_testnet_alpha_v013 COMMAND binarycoin_tests)

    add_executable(binarycoin_bootstrap_tests tests/bootstrap_tests.cpp)
    target_link_libraries(binarycoin_bootstrap_tests PRIVATE binarycoin_core)
    binarycoin_warnings(binarycoin_bootstrap_tests)
    add_test(NAME binarycoin_bootstrap_alpha_v013 COMMAND binarycoin_bootstrap_tests)

    add_executable(binarycoin_rpc_tests tests/rpc_tests.cpp)
    target_link_libraries(binarycoin_rpc_tests PRIVATE binarycoin_core)
    binarycoin_warnings(binarycoin_rpc_tests)
    add_test(NAME binarycoin_rpc_alpha_v013 COMMAND binarycoin_rpc_tests)

    if(NOT WIN32)
        add_executable(binarycoin_hardening_tests tests/network_hardening_tests.cpp)
        target_link_libraries(binarycoin_hardening_tests PRIVATE binarycoin_core)
        binarycoin_warnings(binarycoin_hardening_tests)
        add_test(NAME binarycoin_network_hardening_alpha_v013 COMMAND binarycoin_hardening_tests)
        set_tests_properties(binarycoin_network_hardening_alpha_v013 PROPERTIES TIMEOUT 300)
    endif()
endif()
