# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Native Tailscale (microlink) lives in a submodule; its component and the nested
# wireguard_lwip it bundles are discovered here. Only kvm_net REQUIRES it, so it
# is compiled in but stays dormant unless the ts_* settings turn it on.
set(EXTRA_COMPONENT_DIRS third_party/microlink/components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espkvm)

# Code that is never called is an error in our own components. 0.40.1 shipped a
# helper that nothing invoked, so the setting it belonged to did nothing at all,
# and the only sign was a warning that scrolled past in the build log. Applied
# per component, because third_party/ and the IDF itself are not ours to fix.
idf_build_get_property(kvm_built_components BUILD_COMPONENTS)
file(GLOB kvm_component_dirs ${CMAKE_CURRENT_LIST_DIR}/components/*)
# esp_tinyusb sits in components/ but is vendored, so it is not ours to police.
set(kvm_vendored_components esp_tinyusb)
set(kvm_own_components main)
foreach(dir ${kvm_component_dirs})
    if(IS_DIRECTORY ${dir})
        get_filename_component(name ${dir} NAME)
        if(NOT name IN_LIST kvm_vendored_components)
            list(APPEND kvm_own_components ${name})
        endif()
    endif()
endforeach()
foreach(comp ${kvm_own_components})
    if(comp IN_LIST kvm_built_components)
        idf_component_get_property(lib ${comp} COMPONENT_LIB)
        target_compile_options(${lib} PRIVATE -Werror=unused-function)
    endif()
endforeach()
