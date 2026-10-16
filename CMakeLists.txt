cmake_minimum_required(VERSION 4.2)
cmake_policy(SET CMP0168 NEW)
project(NavalCommand.Gateway.Grpc.Client VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(ncbuild GIT_REPOSITORY https://github.com/NavalCommand/Build.CMake.git GIT_TAG main)
FetchContent_MakeAvailable(ncbuild)
include("${ncbuild_SOURCE_DIR}/NavalCommand.Build.CMake.cmake")

NavalCommandAddDependency(https://github.com/NavalCommand/Gateway.Api.git 94cca460b5d44d7c4211a6e8f06a5af3be2b6fd2)
NavalCommandAddDependency(https://github.com/NavalCommand/Gateway.Grpc.Api.git 82216b583f82fc60d784cc9d1a62b3488a99a88e)

NavalCommandConfigureModule()
