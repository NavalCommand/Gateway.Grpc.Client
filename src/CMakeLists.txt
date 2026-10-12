add_library(NavalCommand.Gateway.Grpc.Client STATIC NavalCommand/Gateway/Grpc/Client/Connection.cpp)
target_include_directories(NavalCommand.Gateway.Grpc.Client PUBLIC "${PROJECT_SOURCE_DIR}/public")
target_link_libraries(NavalCommand.Gateway.Grpc.Client PUBLIC
    NavalCommand.Units.Api NavalCommand.Observations.Api NavalCommand.Operations.Api
    NavalCommand.Environment.Api NavalCommand.Simulation.Api)
target_link_libraries(NavalCommand.Gateway.Grpc.Client PRIVATE NavalCommand.Gateway.Api NavalCommand.Gateway.Grpc.Api)
NavalCommandConfigureTarget(NavalCommand.Gateway.Grpc.Client)
