cmake_minimum_required(VERSION 3.16)

# The boot guard lives two levels up: the bootloader override and the app component.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../bootloader_components" APPEND)

project(crashloop)
