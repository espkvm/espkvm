# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Native Tailscale (microlink) lives in a submodule; its component and the nested
# wireguard_lwip it bundles are discovered here. Only kvm_net REQUIRES it, so it
# is compiled in but stays dormant unless the ts_* settings turn it on.
set(EXTRA_COMPONENT_DIRS third_party/microlink/components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espkvm)
