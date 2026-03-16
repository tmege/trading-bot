cmake_minimum_required(VERSION 3.20)
project(trading_bot C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Compiler hardening ──────────────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wformat -Wformat-security
    -fstack-protector-strong
    -fPIE
)
add_compile_definitions($<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>)
if(NOT APPLE)
    add_link_options(-pie -Wl,-z,relro,-z,now)
endif()

# ── Sanitizers (Debug builds only) ───────────────────────────────────────────
option(TB_SANITIZERS "Enable ASan + UBSan in Debug builds" OFF)
if(TB_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ── Options ──────────────────────────────────────────────────────────────────
option(TB_BUILD_TESTS "Build tests" ON)

# ── FetchContent for vendored deps ───────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(yyjson
    GIT_REPOSITORY https://github.com/ibireme/yyjson.git
    GIT_TAG        0.10.0
)
FetchContent_MakeAvailable(yyjson)

# ── System dependencies ─────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# SQLite3
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

# Lua 5.4
pkg_check_modules(LUA QUIET lua5.4 lua-5.4 lua54)
if(NOT LUA_FOUND)
    pkg_check_modules(LUA QUIET lua)
endif()
if(NOT LUA_FOUND)
    find_package(Lua 5.4)
    if(LUA_FOUND)
        set(LUA_INCLUDE_DIRS ${LUA_INCLUDE_DIR})
        set(LUA_LIBRARIES ${LUA_LIBRARIES})
    else()
        message(FATAL_ERROR "Lua 5.4 not found. Install with: brew install lua@5.4")
    endif()
endif()

# libwebsockets
pkg_check_modules(LWS REQUIRED libwebsockets)

# libsecp256k1
pkg_check_modules(SECP256K1 QUIET libsecp256k1)
if(NOT SECP256K1_FOUND)
    # Try manual find
    find_library(SECP256K1_LIBRARIES NAMES secp256k1)
    find_path(SECP256K1_INCLUDE_DIRS NAMES secp256k1.h)
    if(NOT SECP256K1_LIBRARIES OR NOT SECP256K1_INCLUDE_DIRS)
        message(FATAL_ERROR "libsecp256k1 not found. Install with: brew install secp256k1")
    endif()
endif()

# msgpack-c
pkg_check_modules(MSGPACK QUIET msgpack-c msgpack)
if(NOT MSGPACK_FOUND)
    find_library(MSGPACK_LIBRARIES NAMES msgpack msgpackc msgpack-c)
    find_path(MSGPACK_INCLUDE_DIRS NAMES msgpack.h)
    if(NOT MSGPACK_LIBRARIES OR NOT MSGPACK_INCLUDE_DIRS)
        message(FATAL_ERROR "msgpack-c not found. Install with: brew install msgpack")
    endif()
endif()

# ── Sources ──────────────────────────────────────────────────────────────────
set(TB_SOURCES
    src/main.c
    src/core/config.c
    src/core/logging.c
    src/core/engine.c
    src/core/error.c
    src/core/db.c
    src/core/decimal.c
    src/exchange/keccak256.c
    src/exchange/hl_signing.c
    src/exchange/hl_types.c
    src/exchange/hl_json.c
    src/exchange/hl_rest.c
    src/exchange/hl_ws.c
    src/exchange/position_tracker.c
    src/exchange/order_manager.c
    src/risk/risk_manager.c
    src/strategy/lua_engine.c
    src/strategy/strategy_api.c
    src/strategy/indicators.c
    src/data/twitter_sentiment.c
    src/data/fear_greed.c
    src/data/data_manager.c
    src/report/dashboard.c
    src/report/report_gen.c
    src/backtest/backtest_engine.c
    src/exchange/paper_exchange.c
)

add_executable(trading_bot ${TB_SOURCES})

target_include_directories(trading_bot PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${LUA_INCLUDE_DIRS}
    ${LWS_INCLUDE_DIRS}
    ${SECP256K1_INCLUDE_DIRS}
    ${MSGPACK_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
)

target_link_libraries(trading_bot PRIVATE
    yyjson
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    ${LUA_LIBRARIES}
    ${LWS_LIBRARIES}
    ${SECP256K1_LIBRARIES}
    ${MSGPACK_LIBRARIES}
    ${SQLITE3_LIBRARIES}
)

target_link_directories(trading_bot PRIVATE
    ${LWS_LIBRARY_DIRS}
    ${SECP256K1_LIBRARY_DIRS}
    ${MSGPACK_LIBRARY_DIRS}
    ${SQLITE3_LIBRARY_DIRS}
    ${LUA_LIBRARY_DIRS}
)

# ── Tests ────────────────────────────────────────────────────────────────────
if(TB_BUILD_TESTS)
    enable_testing()

    # Test sources (exclude main.c from bot sources)
    set(TB_LIB_SOURCES ${TB_SOURCES})
    list(REMOVE_ITEM TB_LIB_SOURCES src/main.c)

    add_executable(test_exchange tests/test_exchange.c ${TB_LIB_SOURCES})
    target_include_directories(test_exchange PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${LUA_INCLUDE_DIRS}
        ${LWS_INCLUDE_DIRS}
        ${SECP256K1_INCLUDE_DIRS}
        ${MSGPACK_INCLUDE_DIRS}
        ${SQLITE3_INCLUDE_DIRS}
    )
    target_link_libraries(test_exchange PRIVATE
        yyjson CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads
        ${LUA_LIBRARIES} ${LWS_LIBRARIES} ${SECP256K1_LIBRARIES}
        ${MSGPACK_LIBRARIES} ${SQLITE3_LIBRARIES}
    )
    target_link_directories(test_exchange PRIVATE
        ${LWS_LIBRARY_DIRS} ${SECP256K1_LIBRARY_DIRS} ${MSGPACK_LIBRARY_DIRS}
        ${SQLITE3_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS}
    )
    add_test(NAME test_exchange COMMAND test_exchange)

    add_executable(test_risk tests/test_risk.c ${TB_LIB_SOURCES})
    target_include_directories(test_risk PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${LUA_INCLUDE_DIRS}
        ${LWS_INCLUDE_DIRS}
        ${SECP256K1_INCLUDE_DIRS}
        ${MSGPACK_INCLUDE_DIRS}
        ${SQLITE3_INCLUDE_DIRS}
    )
    target_link_libraries(test_risk PRIVATE
        yyjson CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads
        ${LUA_LIBRARIES} ${LWS_LIBRARIES} ${SECP256K1_LIBRARIES}
        ${MSGPACK_LIBRARIES} ${SQLITE3_LIBRARIES}
    )
    target_link_directories(test_risk PRIVATE
        ${LWS_LIBRARY_DIRS} ${SECP256K1_LIBRARY_DIRS} ${MSGPACK_LIBRARY_DIRS}
        ${SQLITE3_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS}
    )
    add_test(NAME test_risk COMMAND test_risk)

    add_executable(test_lua_engine tests/test_lua_engine.c ${TB_LIB_SOURCES})
    target_include_directories(test_lua_engine PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${LUA_INCLUDE_DIRS}
        ${LWS_INCLUDE_DIRS}
        ${SECP256K1_INCLUDE_DIRS}
        ${MSGPACK_INCLUDE_DIRS}
        ${SQLITE3_INCLUDE_DIRS}
    )
    target_link_libraries(test_lua_engine PRIVATE
        yyjson CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads
        ${LUA_LIBRARIES} ${LWS_LIBRARIES} ${SECP256K1_LIBRARIES}
        ${MSGPACK_LIBRARIES} ${SQLITE3_LIBRARIES}
    )
    target_link_directories(test_lua_engine PRIVATE
        ${LWS_LIBRARY_DIRS} ${SECP256K1_LIBRARY_DIRS} ${MSGPACK_LIBRARY_DIRS}
        ${SQLITE3_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS}
    )
    add_test(NAME test_lua_engine COMMAND test_lua_engine)

    add_executable(test_data tests/test_data.c ${TB_LIB_SOURCES})
    target_include_directories(test_data PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${LUA_INCLUDE_DIRS}
        ${LWS_INCLUDE_DIRS}
        ${SECP256K1_INCLUDE_DIRS}
        ${MSGPACK_INCLUDE_DIRS}
        ${SQLITE3_INCLUDE_DIRS}
    )
    target_link_libraries(test_data PRIVATE
        yyjson CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads
        ${LUA_LIBRARIES} ${LWS_LIBRARIES} ${SECP256K1_LIBRARIES}
        ${MSGPACK_LIBRARIES} ${SQLITE3_LIBRARIES}
    )
    target_link_directories(test_data PRIVATE
        ${LWS_LIBRARY_DIRS} ${SECP256K1_LIBRARY_DIRS} ${MSGPACK_LIBRARY_DIRS}
        ${SQLITE3_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS}
    )
    add_test(NAME test_data COMMAND test_data)

    add_executable(test_indicators tests/test_indicators.c src/strategy/indicators.c src/core/logging.c)
    target_include_directories(test_indicators PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_indicators PRIVATE Threads::Threads m)
    add_test(NAME test_indicators COMMAND test_indicators)

    add_executable(bench_speed tests/bench_speed.c ${TB_LIB_SOURCES})
    target_include_directories(bench_speed PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${LUA_INCLUDE_DIRS}
        ${LWS_INCLUDE_DIRS}
        ${SECP256K1_INCLUDE_DIRS}
        ${MSGPACK_INCLUDE_DIRS}
        ${SQLITE3_INCLUDE_DIRS}
    )
    target_link_libraries(bench_speed PRIVATE
        yyjson CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads
        ${LUA_LIBRARIES} ${LWS_LIBRARIES} ${SECP256K1_LIBRARIES}
        ${MSGPACK_LIBRARIES} ${SQLITE3_LIBRARIES}
    )
    target_link_directories(bench_speed PRIVATE
        ${LWS_LIBRARY_DIRS} ${SECP256K1_LIBRARY_DIRS} ${MSGPACK_LIBRARY_DIRS}
        ${SQLITE3_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS}
    )

    add_executable(bench_strategies tests/bench_strategies.c ${TB_LIB_SOURCES})
    target_include_directories(bench_strategies PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${LUA_INCLUDE_DIRS}
        ${LWS_INCLUDE_DIRS}
        ${SECP256K1_INCLUDE_DIRS}
        ${MSGPACK_INCLUDE_DIRS}
        ${SQLITE3_INCLUDE_DIRS}
    )
    target_link_libraries(bench_strategies PRIVATE
        yyjson CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads
        ${LUA_LIBRARIES} ${LWS_LIBRARIES} ${SECP256K1_LIBRARIES}
        ${MSGPACK_LIBRARIES} ${SQLITE3_LIBRARIES}
    )
    target_link_directories(bench_strategies PRIVATE
        ${LWS_LIBRARY_DIRS} ${SECP256K1_LIBRARY_DIRS} ${MSGPACK_LIBRARY_DIRS}
        ${SQLITE3_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS}
    )
    add_executable(backtest_real tests/backtest_real.c ${TB_LIB_SOURCES})
    target_include_directories(backtest_real PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${LUA_INCLUDE_DIRS}
        ${LWS_INCLUDE_DIRS}
        ${SECP256K1_INCLUDE_DIRS}
        ${MSGPACK_INCLUDE_DIRS}
        ${SQLITE3_INCLUDE_DIRS}
    )
    target_link_libraries(backtest_real PRIVATE
        yyjson CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads
        ${LUA_LIBRARIES} ${LWS_LIBRARIES} ${SECP256K1_LIBRARIES}
        ${MSGPACK_LIBRARIES} ${SQLITE3_LIBRARIES}
    )
    target_link_directories(backtest_real PRIVATE
        ${LWS_LIBRARY_DIRS} ${SECP256K1_LIBRARY_DIRS} ${MSGPACK_LIBRARY_DIRS}
        ${SQLITE3_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS}
    )

    add_executable(backtest_multi_coin tests/backtest_multi_coin.c ${TB_LIB_SOURCES})
    target_include_directories(backtest_multi_coin PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${LUA_INCLUDE_DIRS}
        ${LWS_INCLUDE_DIRS}
        ${SECP256K1_INCLUDE_DIRS}
        ${MSGPACK_INCLUDE_DIRS}
        ${SQLITE3_INCLUDE_DIRS}
    )
    target_link_libraries(backtest_multi_coin PRIVATE
        yyjson CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads
        ${LUA_LIBRARIES} ${LWS_LIBRARIES} ${SECP256K1_LIBRARIES}
        ${MSGPACK_LIBRARIES} ${SQLITE3_LIBRARIES}
    )
    target_link_directories(backtest_multi_coin PRIVATE
        ${LWS_LIBRARY_DIRS} ${SECP256K1_LIBRARY_DIRS} ${MSGPACK_LIBRARY_DIRS}
        ${SQLITE3_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS}
    )

    add_executable(backtest_json tests/backtest_json.c ${TB_LIB_SOURCES})
    target_include_directories(backtest_json PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${LUA_INCLUDE_DIRS}
        ${LWS_INCLUDE_DIRS}
        ${SECP256K1_INCLUDE_DIRS}
        ${MSGPACK_INCLUDE_DIRS}
        ${SQLITE3_INCLUDE_DIRS}
    )
    target_link_libraries(backtest_json PRIVATE
        yyjson CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads
        ${LUA_LIBRARIES} ${LWS_LIBRARIES} ${SECP256K1_LIBRARIES}
        ${MSGPACK_LIBRARIES} ${SQLITE3_LIBRARIES}
    )
    target_link_directories(backtest_json PRIVATE
        ${LWS_LIBRARY_DIRS} ${SECP256K1_LIBRARY_DIRS} ${MSGPACK_LIBRARY_DIRS}
        ${SQLITE3_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS}
    )

    add_executable(candle_fetcher tools/candle_fetcher.c ${TB_LIB_SOURCES})
    target_include_directories(candle_fetcher PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${LUA_INCLUDE_DIRS}
        ${LWS_INCLUDE_DIRS}
        ${SECP256K1_INCLUDE_DIRS}
        ${MSGPACK_INCLUDE_DIRS}
        ${SQLITE3_INCLUDE_DIRS}
    )
    target_link_libraries(candle_fetcher PRIVATE
        yyjson CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads
        ${LUA_LIBRARIES} ${LWS_LIBRARIES} ${SECP256K1_LIBRARIES}
        ${MSGPACK_LIBRARIES} ${SQLITE3_LIBRARIES}
    )
    target_link_directories(candle_fetcher PRIVATE
        ${LWS_LIBRARY_DIRS} ${SECP256K1_LIBRARY_DIRS} ${MSGPACK_LIBRARY_DIRS}
        ${SQLITE3_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS}
    )
endif()

# ── Install ──────────────────────────────────────────────────────────────────
install(TARGETS trading_bot DESTINATION bin)
install(DIRECTORY strategies/ DESTINATION share/trading_bot/strategies)
install(DIRECTORY config/ DESTINATION etc/trading_bot)
