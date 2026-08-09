# Все внешние зависимости лежат прямо в репозитории (vendored) — ни один
# CMake-проект в этом репо не обращается к сети при конфигурации/сборке.
# Этот файл — единственное место, где описано, как их собирать: и
# core/server (корневой CMakeLists.txt), и clients/desktop подключают
# именно его через add_subdirectory, чтобы код и описание сборки библиотек
# не повторялись в двух местах.

# --- EnTT: header-only, единый заголовок ---
add_library(EnTT INTERFACE)
target_include_directories(EnTT INTERFACE entt)

# --- nlohmann/json: header-only, единый заголовок ---
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# --- IXWebSocket: минимальный набор исходников без TLS и без zlib ---
# (шифрование и сжатие не нужны для локальной сети). Список файлов
# соответствует тому, что сама библиотека собирает при
# USE_TLS=OFF, USE_ZLIB=OFF.
set(IXWEBSOCKET_DIR ixwebsocket/ixwebsocket)

add_library(ixwebsocket STATIC
    ${IXWEBSOCKET_DIR}/IXBench.cpp
    ${IXWEBSOCKET_DIR}/IXCancellationRequest.cpp
    ${IXWEBSOCKET_DIR}/IXConnectionState.cpp
    ${IXWEBSOCKET_DIR}/IXDNSLookup.cpp
    ${IXWEBSOCKET_DIR}/IXExponentialBackoff.cpp
    ${IXWEBSOCKET_DIR}/IXGetFreePort.cpp
    ${IXWEBSOCKET_DIR}/IXGzipCodec.cpp
    ${IXWEBSOCKET_DIR}/IXHttp.cpp
    ${IXWEBSOCKET_DIR}/IXHttpClient.cpp
    ${IXWEBSOCKET_DIR}/IXHttpServer.cpp
    ${IXWEBSOCKET_DIR}/IXNetSystem.cpp
    ${IXWEBSOCKET_DIR}/IXSelectInterrupt.cpp
    ${IXWEBSOCKET_DIR}/IXSelectInterruptFactory.cpp
    ${IXWEBSOCKET_DIR}/IXSelectInterruptPipe.cpp
    ${IXWEBSOCKET_DIR}/IXSelectInterruptEvent.cpp
    ${IXWEBSOCKET_DIR}/IXSetThreadName.cpp
    ${IXWEBSOCKET_DIR}/IXSocket.cpp
    ${IXWEBSOCKET_DIR}/IXSocketConnect.cpp
    ${IXWEBSOCKET_DIR}/IXSocketFactory.cpp
    ${IXWEBSOCKET_DIR}/IXSocketServer.cpp
    ${IXWEBSOCKET_DIR}/IXSocketTLSOptions.cpp
    ${IXWEBSOCKET_DIR}/IXStrCaseCompare.cpp
    ${IXWEBSOCKET_DIR}/IXUdpSocket.cpp
    ${IXWEBSOCKET_DIR}/IXUrlParser.cpp
    ${IXWEBSOCKET_DIR}/IXUuid.cpp
    ${IXWEBSOCKET_DIR}/IXUserAgent.cpp
    ${IXWEBSOCKET_DIR}/IXWebSocket.cpp
    ${IXWEBSOCKET_DIR}/IXWebSocketCloseConstants.cpp
    ${IXWEBSOCKET_DIR}/IXWebSocketHandshake.cpp
    ${IXWEBSOCKET_DIR}/IXWebSocketHttpHeaders.cpp
    ${IXWEBSOCKET_DIR}/IXWebSocketPerMessageDeflate.cpp
    ${IXWEBSOCKET_DIR}/IXWebSocketPerMessageDeflateCodec.cpp
    ${IXWEBSOCKET_DIR}/IXWebSocketPerMessageDeflateOptions.cpp
    ${IXWEBSOCKET_DIR}/IXWebSocketProxyServer.cpp
    ${IXWEBSOCKET_DIR}/IXWebSocketServer.cpp
    ${IXWEBSOCKET_DIR}/IXWebSocketTransport.cpp
)

target_include_directories(ixwebsocket PUBLIC ixwebsocket)
target_compile_definitions(ixwebsocket PRIVATE _CRT_SECURE_NO_WARNINGS)

if (WIN32)
    target_link_libraries(ixwebsocket PRIVATE wsock32 ws2_32 shlwapi)
    # windows.h (тянется через IXNetSystem/IXSelectInterruptEvent) сам
    # определяет макросы min/max, которые ломают std::min/std::max в коде,
    # который его транзитивно подключает. NOMINMAX это отключает. PUBLIC —
    # чтобы define применился и к тем, кто линкуется на ixwebsocket.
    target_compile_definitions(ixwebsocket PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if (UNIX AND NOT APPLE)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)
    target_link_libraries(ixwebsocket PRIVATE Threads::Threads)
endif()
