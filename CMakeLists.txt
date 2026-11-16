# Modified for Multistream OBS on 2026-11-16.
cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

list(APPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-33.0.1/build_x64/libobs)
list(APPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-33.0.1/build_x64/deps/w32-pthreads)
list(APPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-33.0.1/build_x64/frontend/api)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Network)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  ./src/pch.h
  ./src/obs-multi-rtmp.h
  ./src/obs-multi-rtmp.cpp 
  ./src/edit-widget.h
  ./src/edit-widget.cpp
  ./src/push-widget.h
  ./src/push-widget.cpp
  ./src/json-util.hpp
  ./src/output-config.h
  ./src/output-config.cpp
  ./src/obs-properties-widget.h
  ./src/obs-properties-widget.cpp
  ./src/protocols.h
  ./src/protocols.cpp
  ./src/helpers.h
  ./src/helpers.cpp
  ./src/update-package.h
  ./src/update-package.cpp
  ./src/update-manager.h
  ./src/update-manager.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
  ./dep/nlohmann-json
  .deps/obs-studio-33.0.1/libobs
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
if(WIN32)
  # OBS does not ship a Qt TLS backend; Schannel uses Windows certificate validation.
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-qt6-2026-10-23-x64/plugins/tls/qschannelbackend.dll"
          DESTINATION "${CMAKE_PROJECT_NAME}/qt-plugins/tls")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-qt6-2026-10-23-x64/licenses/qt6/LICENSE.GPL2"
          DESTINATION "${CMAKE_PROJECT_NAME}/licenses/qt6")
endif()

option(MULTISTREAM_BUILD_TESTS "Build update package tests" OFF)
if(MULTISTREAM_BUILD_TESTS)
  enable_testing()
  add_executable(multistream-update-tests tests/update-package-test.cpp src/update-package.cpp)
  target_include_directories(multistream-update-tests PRIVATE src)
  target_link_libraries(multistream-update-tests PRIVATE Qt6::Core)
  add_test(NAME update-package COMMAND multistream-update-tests)
endif()
