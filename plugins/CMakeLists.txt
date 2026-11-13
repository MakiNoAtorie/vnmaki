# C++ plugins (task 48, docs/plugins.md). Each folder with a CMakeLists.txt is one plugin, and its
# file calls vnmaki_add_plugin(). -DVNMAKI_PLUGINS=<names> links them into the runner (cli/).
file(GLOB plugin_lists RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt")
foreach(plugin_list IN LISTS plugin_lists)
  get_filename_component(plugin_directory "${plugin_list}" DIRECTORY)
  add_subdirectory("${plugin_directory}")
endforeach()
