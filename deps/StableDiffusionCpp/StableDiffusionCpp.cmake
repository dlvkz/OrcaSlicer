# stable-diffusion.cpp (MIT) for the text to height map feature (src/libslic3r/TextToHeight.cpp). Built as shared libraries
# (stable-diffusion + ggml) so its bundled third-party code (miniz, stb) stays out of the slicer's link. The patch lets
# Conv2d take quantized kernels stored flat ([IC*KH*KW, OC]), which the 4-bit SD-Turbo model uses.
# Opt in with -DDEP_TEXT2HEIGHT=ON when building the dependencies.
#
# ggml's backends are libraries loaded at run time (GGML_BACKEND_DL): one processor backend per instruction set level,
# the best the processor supports is picked, and Vulkan when the Vulkan SDK is found at build time. Without a usable
# graphics card or Vulkan driver only the processor backend loads, so the program runs anywhere.
find_package(Vulkan QUIET COMPONENTS glslc)
if (Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE)
    set(_sdcpp_vulkan ON)
    message(STATUS "stable-diffusion.cpp: with the Vulkan backend")
else ()
    set(_sdcpp_vulkan OFF)
    message(STATUS "stable-diffusion.cpp: Vulkan SDK not found, processor only")
endif ()

Snapmaker_Orca_add_cmake_project(StableDiffusionCpp
    GIT_REPOSITORY  https://github.com/leejet/stable-diffusion.cpp.git
    GIT_TAG         32d516c0197ed5ca31305b8575e14eb1451fad33
    GIT_SHALLOW     OFF
    PATCH_COMMAND   git apply --whitespace=nowarn ${CMAKE_CURRENT_LIST_DIR}/0001-quantized-flat-conv2d.patch
    CMAKE_ARGS
        -DSD_BUILD_EXAMPLES=OFF
        -DSD_BUILD_SHARED_LIBS=ON
        -DSD_BUILD_SHARED_GGML_LIB=ON
        -DSD_WEBP=OFF
        -DSD_WEBM=OFF
        -DSD_VULKAN=${_sdcpp_vulkan}
        -DGGML_BACKEND_DL=ON
        -DGGML_CPU_ALL_VARIANTS=ON
        -DGGML_NATIVE=OFF
)
