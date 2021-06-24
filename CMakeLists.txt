cmake_minimum_required(VERSION 3.0 FATAL_ERROR)
project(ds_trt_tsc_bridge)

set(CMAKE_VERBOSE_MAKEFILE ON)

set(Torch_DIR /home/jackie/workspace/aistack/tensorrt/running/libtorch)
set(TorchVision_DIR /home/jackie/workspace/aistack/tensorrt/running/vision)

list(APPEND CMAKE_PREFIX_PATH
  "/home/jackie/workspace/aistack/tensorrt/running/libtorch"
  "/home/jackie/workspace/aistack/tensorrt/running/vision"
)

find_package(Torch REQUIRED)
find_package(TorchVision REQUIRED)

add_compile_definitions(WITH_CUDA)

add_library(ds_trt_tsc_bridge SHARED ds_trt_tsc_bridge.cpp)
target_include_directories(ds_trt_tsc_bridge PRIVATE "/home/jackie/local/deepstream/deepstream-5.1/sources/includes/")
target_link_libraries(
    ds_trt_tsc_bridge
    "${TORCH_LIBRARIES}"
    -Wl,--no-as-needed
    TorchVision::TorchVision
    -Wl,--as-needed
)
set_property(TARGET ds_trt_tsc_bridge PROPERTY CXX_STANDARD 14)

