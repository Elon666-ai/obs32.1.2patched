set(ONNXRUNTIME_VERSION "1.23.2" CACHE STRING "ONNX Runtime version")
set(ONNXRUNTIME_ROOT "" CACHE PATH "Root of the ONNX Runtime NuGet package")

if(NOT ONNXRUNTIME_ROOT)
  message(STATUS "ONNX Runtime not configured; face swap inference will be unavailable")
  return()
endif()

set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_ROOT}/build/native/include")
set(ONNXRUNTIME_DLL "${ONNXRUNTIME_ROOT}/runtimes/win-x64/native/onnxruntime.dll")

if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_c_api.h" OR NOT EXISTS "${ONNXRUNTIME_DLL}")
  message(FATAL_ERROR "ONNXRUNTIME_ROOT does not contain the pinned Windows x64 ONNX Runtime ${ONNXRUNTIME_VERSION} package")
endif()

# A plain INTERFACE library rather than an IMPORTED one: OBS walks the link
# dependencies of every target while bundling the frontend, and an IMPORTED
# target is not visible outside the directory that declares it.
add_library(obs-onnxruntime INTERFACE)
add_library(OBS::onnxruntime ALIAS obs-onnxruntime)
target_include_directories(obs-onnxruntime INTERFACE "${ONNXRUNTIME_INCLUDE_DIR}")
