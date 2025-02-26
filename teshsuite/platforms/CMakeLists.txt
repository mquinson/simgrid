add_executable       (flatifier EXCLUDE_FROM_ALL flatifier.cpp)
target_link_libraries(flatifier  simgrid)
set_target_properties(flatifier  PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_property(TARGET flatifier APPEND PROPERTY INCLUDE_DIRECTORIES "${INTERNAL_INCLUDES}")
add_dependencies(tests flatifier)

add_library(asymetric_nested_zones SHARED asymetric_nested_zones.cpp)
target_link_libraries(asymetric_nested_zones simgrid)
add_dependencies(tests asymetric_nested_zones)

set(teshsuite_src ${teshsuite_src}  ${CMAKE_CURRENT_SOURCE_DIR}/flatifier.cpp PARENT_SCOPE)
set(tesh_files    ${tesh_files}     ${CMAKE_CURRENT_SOURCE_DIR}/bogus_two_hosts_asymetric.tesh
                                    ${CMAKE_CURRENT_SOURCE_DIR}/bogus_missing_gateway.tesh
                                    ${CMAKE_CURRENT_SOURCE_DIR}/flatifier.tesh
                                    PARENT_SCOPE)
set(xml_files     ${xml_files}      ${CMAKE_CURRENT_SOURCE_DIR}/four_hosts_floyd.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/two_hosts_multi_hop.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/host_attributes.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/link_attributes.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/one_cluster_multicore.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/one_cluster_splitduplex.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/one_cluster_router_id.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/one_cluster.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/properties.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/test_of_is_router.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/three_hosts_non_symmetric_route.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/two_clusters_one_name.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/two_clusters_router_id.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/two_clusters.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/bogus_missing_src_gateway.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/bogus_missing_dst_gateway.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/bogus_two_hosts_asymetric.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/two_hosts_one_link_splitduplex.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/two_hosts_one_link.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/cluster_dragonfly_noncontiguous_rad.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/cluster_fat_tree_noncontiguous_rad.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/cluster_torus_noncontiguous_rad.xml
                                    ${CMAKE_CURRENT_SOURCE_DIR}/Dijkstra.xml
                                    PARENT_SCOPE)
set(txt_files     ${txt_files}      ${CMAKE_CURRENT_SOURCE_DIR}/carol.fail
                                    ${CMAKE_CURRENT_SOURCE_DIR}/bob.trace
                                    ${CMAKE_CURRENT_SOURCE_DIR}/erin.avail
                                    ${CMAKE_CURRENT_SOURCE_DIR}/link.bw
                                    ${CMAKE_CURRENT_SOURCE_DIR}/link.fail
                                    ${CMAKE_CURRENT_SOURCE_DIR}/link.lat
                                    PARENT_SCOPE)

ADD_TESH(tesh-platform-flatifier --setenv bindir=${CMAKE_BINARY_DIR}/teshsuite/platforms --setenv srcdir=${CMAKE_HOME_DIRECTORY} --setenv libdir=${CMAKE_BINARY_DIR}/lib --cd ${CMAKE_HOME_DIRECTORY}/teshsuite/platforms flatifier.tesh)

if(enable_debug)
  # these tests need assertions
  ADD_TESH(tesh-parser-bogus-symmetric         --setenv bindir=${CMAKE_BINARY_DIR}/teshsuite/platforms --cd ${CMAKE_HOME_DIRECTORY}/teshsuite/platforms ${CMAKE_HOME_DIRECTORY}/teshsuite/platforms/bogus_two_hosts_asymetric.tesh)
  ADD_TESH(tesh-parser-bogus-missing-gw        --setenv bindir=${CMAKE_BINARY_DIR}/teshsuite/platforms --cd ${CMAKE_HOME_DIRECTORY}/teshsuite/platforms ${CMAKE_HOME_DIRECTORY}/teshsuite/platforms/bogus_missing_gateway.tesh)
endif()
