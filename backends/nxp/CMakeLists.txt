# Copyright 2024 NXP
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

set(
  _common_include_directories 
  ${CMAKE_CURRENT_SOURCE_DIR}/../../..
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/core/portable_type/c10
)
add_compile_definitions(C10_USING_CUSTOM_GENERATED_MACROS)

set(_neutron_sources ${CMAKE_CURRENT_SOURCE_DIR}/runtime/NeutronBackend.cpp )

add_library(executorch_delegate_neutron STATIC ${_neutron_sources})
target_include_directories(
  executorch_delegate_neutron PUBLIC ${_common_include_directories}
)
