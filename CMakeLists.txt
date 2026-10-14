cmake_minimum_required(VERSION 4.2)
cmake_policy(SET CMP0168 NEW)
project(NavalCommand.Gateway.Grpc.Client VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(ncbuild GIT_REPOSITORY https://github.com/NavalCommand/Build.CMake.git GIT_TAG main)
FetchContent_MakeAvailable(ncbuild)
include("${ncbuild_SOURCE_DIR}/NavalCommand.Build.CMake.cmake")

NavalCommandAddDependency(https://github.com/NavalCommand/Gateway.Api.git 36ec4c5f3a9629f31b8d298ac40f55b61f2231b4)
NavalCommandAddDependency(https://github.com/NavalCommand/Gateway.Grpc.Api.git a90fd10dc7f57189840ac6b19ed39855e1c7801f)

NavalCommandConfigureModule()
