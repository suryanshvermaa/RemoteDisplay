cmake_minimum_required(VERSION 3.20)
project(RemoteDisplay CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE_DISABLED)

set(MF_LIBS d3d11 dxgi dxguid mf mfplat mfuuid wmcodecdspuuid strmiids ole32 oleaut32 ws2_32 winmm)

add_executable(rd-host service/src/main.cpp)
target_include_directories(rd-host PRIVATE service/include)
target_link_libraries(rd-host PRIVATE ${MF_LIBS} bcrypt)

add_executable(rd-client WIN32 client/src/main.cpp)
target_include_directories(rd-client PRIVATE service/include)  # shared protocol.h
target_link_libraries(rd-client PRIVATE ${MF_LIBS} user32 bcrypt)

add_executable(test-session tests/test_session.cpp)
target_include_directories(test-session PRIVATE service/include)
target_link_libraries(test-session PRIVATE bcrypt)
enable_testing()
add_test(NAME session COMMAND test-session)
