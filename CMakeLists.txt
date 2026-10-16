cmake_minimum_required(VERSION 4.2)
cmake_policy(SET CMP0168 NEW)
project(NavalCommand.Gateway.Grpc.Client VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(ncbuild GIT_REPOSITORY https://github.com/NavalCommand/Build.CMake.git GIT_TAG main)
FetchContent_MakeAvailable(ncbuild)
include("${ncbuild_SOURCE_DIR}/NavalCommand.Build.CMake.cmake")

NavalCommandAddDependency(https://github.com/NavalCommand/Gateway.Api.git 406e3f76f4b58a473ef21767d28bf92f65d725cf)
NavalCommandAddDependency(https://github.com/NavalCommand/Gateway.Grpc.Api.git 3c0a2a682d6f7a595f7d31717707d12d9c2770ba)

NavalCommandConfigureModule()
