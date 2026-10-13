add_executable(vnmaki main.cpp)
target_link_libraries(vnmaki PRIVATE vnmaki::engine vnmaki::options)
target_sources(vnmaki PRIVATE run_command.cpp run_command.hpp)
