cmake_minimum_required(VERSION 3.28...3.30)

add_library(obs-filters MODULE)
add_library(OBS::filters ALIAS obs-filters)

if(OS_WINDOWS)
  include(cmake/onnxruntime.cmake)
endif()

target_sources(
  obs-filters
  PRIVATE
    async-delay-filter.c
    chroma-key-filter.c
    clarity-filter.c
    color-correction-filter.c
    color-grade-filter.c
    color-key-filter.c
    compressor-filter.c
    crop-filter.c
    eq-filter.c
    expander-filter.c
    gain-filter.c
    gpu-delay.c
    hdr-tonemap-filter.c
    invert-audio-polarity.c
    limiter-filter.c
    luma-key-filter.c
    mask-filter.c
    noise-gate-filter.c
    obs-filters.c
    scale-filter.c
    scroll-filter.c
    sharpness-filter.c
    temporal-denoise-filter.c
    beauty-filter.c
    face-swap-filter.c
    face-swap-ort.c
    face-swap-ort.h
    face-swap-preprocess.c
    face-swap-preprocess.h
    face-swap-scrfd.c
    face-swap-scrfd.h
    face-swap-yunet.c
    face-swap-yunet.h
    face-swap-stabilize.c
    face-swap-stabilize.h
    face-swap-debug.c
    face-swap-debug.h
    face-swap-align.c
    face-swap-align.h
    face-swap-image.c
    face-swap-image.h
    face-swap-output.c
    face-swap-output.h
    face-swap-blend.c
    face-swap-blend.h
)

target_link_libraries(obs-filters PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>)

if(TARGET OBS::onnxruntime)
  target_compile_definitions(obs-filters PRIVATE FACE_SWAP_HAS_ORT=1)
  target_link_libraries(obs-filters PRIVATE OBS::onnxruntime)
  # onnxruntime.dll has to sit next to obs.exe: the loader searches the
  # executable's directory, not the plugin's.
  add_custom_command(
    TARGET obs-filters
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E make_directory
      "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different "${ONNXRUNTIME_DLL}"
      "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
    VERBATIM
  )
endif()

include(cmake/speexdsp.cmake)
include(cmake/rnnoise.cmake)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in obs-filters.rc)
  target_sources(obs-filters PRIVATE obs-filters.rc)
endif()

set_target_properties_obs(obs-filters PROPERTIES FOLDER plugins PREFIX "")

if(ENABLE_TEST_INPUT)
  add_executable(face-swap-preprocess-test face-swap-preprocess.c face-swap-preprocess-test.c)
  target_link_libraries(face-swap-preprocess-test PRIVATE OBS::libobs)
  add_test(NAME face-swap-preprocess COMMAND face-swap-preprocess-test)
  set_target_properties(face-swap-preprocess-test PROPERTIES FOLDER tests)

  add_executable(face-swap-scrfd-test face-swap-scrfd.c face-swap-scrfd-test.c)
  target_link_libraries(face-swap-scrfd-test PRIVATE OBS::libobs)
  add_test(NAME face-swap-scrfd COMMAND face-swap-scrfd-test)
  set_target_properties(face-swap-scrfd-test PROPERTIES FOLDER tests)

  add_executable(face-swap-yunet-test face-swap-yunet.c face-swap-yunet-test.c)
  target_link_libraries(face-swap-yunet-test PRIVATE OBS::libobs)
  add_test(NAME face-swap-yunet COMMAND face-swap-yunet-test)
  set_target_properties(face-swap-yunet-test PROPERTIES FOLDER tests)

  add_executable(face-swap-stabilize-test face-swap-stabilize.c face-swap-stabilize-test.c)
  target_link_libraries(face-swap-stabilize-test PRIVATE OBS::libobs)
  add_test(NAME face-swap-stabilize COMMAND face-swap-stabilize-test)
  set_target_properties(face-swap-stabilize-test PROPERTIES FOLDER tests)

  add_executable(face-swap-debug-test face-swap-debug.c face-swap-debug-test.c)
  target_link_libraries(face-swap-debug-test PRIVATE OBS::libobs)
  add_test(NAME face-swap-debug COMMAND face-swap-debug-test)
  set_target_properties(face-swap-debug-test PROPERTIES FOLDER tests)

  add_executable(face-swap-align-test face-swap-align.c face-swap-align-test.c)
  target_link_libraries(face-swap-align-test PRIVATE OBS::libobs)
  add_test(NAME face-swap-align COMMAND face-swap-align-test)
  set_target_properties(face-swap-align-test PROPERTIES FOLDER tests)

  add_executable(face-swap-output-test face-swap-output.c face-swap-output-test.c)
  target_link_libraries(face-swap-output-test PRIVATE OBS::libobs)
  add_test(NAME face-swap-output COMMAND face-swap-output-test)
  set_target_properties(face-swap-output-test PROPERTIES FOLDER tests)

  add_executable(face-swap-blend-test face-swap-align.c face-swap-blend.c face-swap-preprocess.c face-swap-blend-test.c)
  target_link_libraries(face-swap-blend-test PRIVATE OBS::libobs)
  add_test(NAME face-swap-blend COMMAND face-swap-blend-test)
  set_target_properties(face-swap-blend-test PROPERTIES FOLDER tests)

  add_executable(
    face-swap-frameblend-test
    face-swap-align.c
    face-swap-preprocess.c
    face-swap-blend.c
    face-swap-frameblend-test.c
  )
  target_link_libraries(face-swap-frameblend-test PRIVATE OBS::libobs)
  add_test(NAME face-swap-frameblend COMMAND face-swap-frameblend-test)
  set_target_properties(face-swap-frameblend-test PROPERTIES FOLDER tests)
endif()
