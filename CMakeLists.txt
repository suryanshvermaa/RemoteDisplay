cmake_minimum_required(VERSION 3.20)
project(RemoteDisplay CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options(/utf-8)

set(MF_LIBS d3d11 dxgi dxguid mf mfplat mfuuid wmcodecdspuuid strmiids ole32 oleaut32 ws2_32 winmm)

add_executable(rd-host service/src/main.cpp)
target_include_directories(rd-host PRIVATE service/include service/src app)
target_link_libraries(rd-host PRIVATE ${MF_LIBS} bcrypt crypt32 iphlpapi comctl32 advapi32 user32 gdi32 secur32)

add_executable(rd-client WIN32 client/src/main.cpp)
target_include_directories(rd-client PRIVATE service/include app)  # shared protocol.h, common.h
target_link_libraries(rd-client PRIVATE ${MF_LIBS} user32 bcrypt crypt32 iphlpapi comctl32)

add_executable(test-session tests/test_session.cpp)
target_include_directories(test-session PRIVATE service/include)
target_link_libraries(test-session PRIVATE bcrypt)
enable_testing()
add_test(NAME session COMMAND test-session)

add_executable(RemoteDisplay WIN32 app/main.cpp)
target_include_directories(RemoteDisplay PRIVATE service/include service/src client/src)
target_link_libraries(RemoteDisplay PRIVATE ${MF_LIBS} user32 gdi32 bcrypt crypt32 iphlpapi comctl32 advapi32 wtsapi32 userenv shell32)

add_executable(test-input tests/test_input.cpp)
target_include_directories(test-input PRIVATE service/include)
target_link_libraries(test-input PRIVATE bcrypt ws2_32 user32)

add_executable(test-admin tests/test_admin.cpp)
target_include_directories(test-admin PRIVATE service/include service/src client/src app)
target_link_libraries(test-admin PRIVATE ${MF_LIBS} user32 gdi32 bcrypt crypt32 iphlpapi comctl32 advapi32 wtsapi32 userenv shell32 secur32)

add_executable(test-secure tests/test_secure.cpp)
target_include_directories(test-secure PRIVATE service/include service/src client/src app)
target_link_libraries(test-secure PRIVATE ${MF_LIBS} user32 gdi32 bcrypt crypt32 iphlpapi comctl32 advapi32 wtsapi32 userenv shell32 secur32)

# Virtual monitor EDID (plain C++, no WDK needed).
add_executable(test-edid tests/test_edid.cpp driver/src/Edid.cpp)
target_include_directories(test-edid PRIVATE driver/src)

add_executable(test-display tests/test_display.cpp)
target_include_directories(test-display PRIVATE service/include service/src client/src app)
target_link_libraries(test-display PRIVATE ${MF_LIBS} user32 gdi32 bcrypt crypt32 iphlpapi comctl32 advapi32 wtsapi32 userenv shell32 secur32)
