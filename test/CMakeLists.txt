NavalCommandAddDependency(https://github.com/NavalCommand/Gateway.Grpc.git 8c1bfb771182eef0f0fc9ae2054fd908d7fdf138)
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)
add_executable(NavalCommand.Gateway.Grpc.Client.Test NavalCommand/Gateway/Grpc/Client/ProxyTest.cpp)
target_link_libraries(NavalCommand.Gateway.Grpc.Client.Test PRIVATE
    NavalCommand.Gateway.Grpc.Client NavalCommand.Gateway.Grpc.Mock GTest::gtest_main)
NavalCommandConfigureTarget(NavalCommand.Gateway.Grpc.Client.Test)
gtest_discover_tests(NavalCommand.Gateway.Grpc.Client.Test DISCOVERY_MODE PRE_TEST)

# Explicit development executable; never registered as an unattended CTest.
add_executable(NavalCommand.Gateway.Grpc.Client.LiveProbe NavalCommand/Gateway/Grpc/Client/LiveProbe.cpp)
target_link_libraries(NavalCommand.Gateway.Grpc.Client.LiveProbe PRIVATE NavalCommand.Gateway.Grpc.Client)
NavalCommandConfigureTarget(NavalCommand.Gateway.Grpc.Client.LiveProbe)
