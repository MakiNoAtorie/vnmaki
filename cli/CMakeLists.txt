add_executable(vnmaki main.cpp)
target_link_libraries(vnmaki PRIVATE vnmaki::engine vnmaki::options)
target_sources(vnmaki PRIVATE window_command.cpp window_command.hpp)
target_sources(vnmaki PRIVATE image_view.cpp image_view.hpp)
