cmake_minimum_required(VERSION 3.22)
project(DisaggregationFabric VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /WX /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Werror -Wpedantic)
endif()
option(DFABRIC_ASAN "Build with AddressSanitizer (MSVC)" OFF)
if(DFABRIC_ASAN AND MSVC)
  add_compile_options(/fsanitize=address /Zi)
  add_link_options(/fsanitize=address)
endif()

add_library(disaggregation_fabric_system STATIC
  backends/system/system_backend.cpp
)
target_link_libraries(disaggregation_fabric_system PUBLIC disaggregation_fabric_core)
target_include_directories(disaggregation_fabric_system PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/backends/system>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/dfabric/backends/system>)

add_library(disaggregation_fabric_synthetic STATIC
  backends/synthetic/synthetic_backend.cpp
)
target_link_libraries(disaggregation_fabric_synthetic PUBLIC disaggregation_fabric_core)
target_include_directories(disaggregation_fabric_synthetic PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/backends/synthetic>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/dfabric/backends/synthetic>)

add_library(disaggregation_fabric_core STATIC
  src/dfabric/engine.cpp
  src/dfabric/persistence.cpp
  src/dfabric/protocol.cpp
)
target_include_directories(disaggregation_fabric_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
if(WIN32)
  target_link_libraries(disaggregation_fabric_core PUBLIC ws2_32)
endif()
set_target_properties(disaggregation_fabric_core PROPERTIES EXPORT_NAME core)
set_target_properties(disaggregation_fabric_system PROPERTIES EXPORT_NAME system)
set_target_properties(disaggregation_fabric_synthetic PROPERTIES EXPORT_NAME synthetic)

enable_testing()
add_executable(t_core_unit tests/t_core_unit.cpp)
target_link_libraries(t_core_unit PRIVATE disaggregation_fabric_core)
add_test(NAME core_unit COMMAND t_core_unit)

add_executable(t_composition tests/t_composition.cpp)
target_link_libraries(t_composition PRIVATE disaggregation_fabric_synthetic)
add_test(NAME composition COMMAND t_composition)
add_executable(dsh_coordinator deploy/coordinator.cpp)
target_link_libraries(dsh_coordinator PRIVATE disaggregation_fabric_synthetic)
add_executable(dsh_worker deploy/worker.cpp)
target_link_libraries(dsh_worker PRIVATE disaggregation_fabric_core)
add_executable(t_workerdeath tests/t_workerdeath.cpp)
target_link_libraries(t_workerdeath PRIVATE disaggregation_fabric_synthetic)
add_dependencies(t_workerdeath dsh_coordinator dsh_worker)
add_test(NAME workerdeath COMMAND t_workerdeath)
add_executable(t_semantics tests/t_semantics.cpp)
target_link_libraries(t_semantics PRIVATE disaggregation_fabric_synthetic)
add_test(NAME semantics COMMAND t_semantics)
add_executable(t_protocol tests/t_protocol.cpp)
target_link_libraries(t_protocol PRIVATE disaggregation_fabric_core)
add_test(NAME protocol COMMAND t_protocol)
add_executable(t_persistence tests/t_persistence.cpp)
target_link_libraries(t_persistence PRIVATE disaggregation_fabric_core)
add_test(NAME persistence COMMAND t_persistence)
add_executable(t_restart tests/t_restart.cpp)
target_link_libraries(t_restart PRIVATE disaggregation_fabric_synthetic)
add_dependencies(t_restart dsh_coordinator dsh_worker)
add_test(NAME restart COMMAND t_restart)
add_executable(t_interrupted tests/t_interrupted.cpp)
target_link_libraries(t_interrupted PRIVATE disaggregation_fabric_synthetic)
add_dependencies(t_interrupted dsh_coordinator dsh_worker)
add_test(NAME interrupted COMMAND t_interrupted)
add_executable(t_robustness tests/t_robustness.cpp)
target_link_libraries(t_robustness PRIVATE disaggregation_fabric_synthetic)
add_test(NAME robustness COMMAND t_robustness)
add_executable(t_system tests/t_system.cpp)
target_link_libraries(t_system PRIVATE disaggregation_fabric_system)
add_test(NAME system COMMAND t_system)
if(CUDA_FOUND OR CMAKE_CUDA_COMPILER)
  add_executable(t_cuda tests/t_cuda.cu)
  target_link_libraries(t_cuda PRIVATE disaggregation_fabric_system)
  set_target_properties(t_cuda PROPERTIES CUDA_ARCHITECTURES 120)
  add_test(NAME cuda COMMAND t_cuda)
  add_dependencies(t_cuda disaggregation_fabric_system)
endif()
# Real CUDA proof: compile the .cu directly with nvcc and link the MSVC-built libs.
find_program(CUDA_NVCC nvcc PATHS "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA")
if(CUDA_NVCC)
  set(CUDA_EXE ${CMAKE_BINARY_DIR}/t_cuda.exe)
  set(NVCC_HOST "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64")
  add_custom_command(
    OUTPUT ${CUDA_EXE}
    COMMAND ${CUDA_NVCC} -ccbin "${NVCC_HOST}" -std=c++20 --generate-code=arch=compute_120,code=sm_120 -Xcompiler /MD
            -I${CMAKE_SOURCE_DIR}/include -I${CMAKE_SOURCE_DIR}/backends/system -I${CMAKE_SOURCE_DIR}/tests
            -D_WINDOWS ${CMAKE_SOURCE_DIR}/tests/t_cuda.cu -o ${CUDA_EXE}
            ${CMAKE_BINARY_DIR}/disaggregation_fabric_system.lib ${CMAKE_BINARY_DIR}/disaggregation_fabric_core.lib ws2_32.lib
    DEPENDS ${CMAKE_SOURCE_DIR}/tests/t_cuda.cu disaggregation_fabric_system
    VERBATIM
  )
  add_custom_target(t_cuda ALL DEPENDS ${CUDA_EXE})
  add_test(NAME cuda COMMAND ${CUDA_EXE})
endif()
# --- install / package -----------------------------------------------------
include(GNUInstallDirs)
install(TARGETS disaggregation_fabric_core disaggregation_fabric_system disaggregation_fabric_synthetic
        EXPORT DisaggregationFabricTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/dfabric DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES backends/system/system_backend.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dfabric/backends/system)
install(FILES backends/synthetic/synthetic_backend.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dfabric/backends/synthetic)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/DisaggregationFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/DisaggregationFabricConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DisaggregationFabric)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/DisaggregationFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(EXPORT DisaggregationFabricTargets
  FILE DisaggregationFabricTargets.cmake
  NAMESPACE DisaggregationFabric::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DisaggregationFabric)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/DisaggregationFabricConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/DisaggregationFabricConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DisaggregationFabric)
add_executable(dfabric_cli tools/dfabric_cli.cpp)
target_link_libraries(dfabric_cli PRIVATE disaggregation_fabric_system disaggregation_fabric_synthetic)
add_executable(dfabric_bench bench/bench.cpp)
target_link_libraries(dfabric_bench PRIVATE disaggregation_fabric_core)
add_executable(t_examples examples/examples.cpp)
target_link_libraries(t_examples PRIVATE disaggregation_fabric_synthetic)
add_test(NAME examples COMMAND t_examples)