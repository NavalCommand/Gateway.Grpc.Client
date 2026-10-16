NavalCommandAddDependency(https://github.com/NavalCommand/Gateway.Grpc.git 291a296f4d3ee4df1d978038d110c5b3faad116a)
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
