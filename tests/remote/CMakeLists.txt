cmake_minimum_required(VERSION 3.16)
project(RemoteWorkspaceTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Test)
add_executable(remote-workspace-tests test_client.cpp
    ../../src/remoteworkspace/remoteclient.cpp ../../src/remoteworkspace/remoteworker.cpp
    ../../src/remoteworkspace/remote.qrc)
target_include_directories(remote-workspace-tests PRIVATE ../../src/remoteworkspace)
target_link_libraries(remote-workspace-tests PRIVATE Qt6::Core Qt6::Test)
