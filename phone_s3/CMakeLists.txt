# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(phone)

# Local patches to managed components (idempotent, at every configure): managed_components/ is not in git and is
# downloaded again by the component manager, so the changes live in patches/<component>-<what>.patch
find_program(PATCH_EXECUTABLE patch)
file(GLOB _local_patches "${CMAKE_SOURCE_DIR}/patches/*.patch")
foreach(_p ${_local_patches})
    get_filename_component(_name ${_p} NAME_WE)
    string(REGEX REPLACE "-.*$" "" _comp ${_name})
    set(_dir "${CMAKE_SOURCE_DIR}/managed_components/espressif__${_comp}")
    if(NOT PATCH_EXECUTABLE)
        message(FATAL_ERROR "'patch' not found, needed for ${_p}")
    elseif(NOT EXISTS ${_dir})
        message(WARNING "${_dir} missing, skipping ${_p}")
    else()
        execute_process(COMMAND ${PATCH_EXECUTABLE} -p1 -R --dry-run -s -f -i ${_p}
                        WORKING_DIRECTORY ${_dir} RESULT_VARIABLE _already OUTPUT_QUIET ERROR_QUIET)
        if(_already EQUAL 0)
            message(STATUS "patch ${_name}: already applied")
        else()
            execute_process(COMMAND ${PATCH_EXECUTABLE} -p1 -N -i ${_p} WORKING_DIRECTORY ${_dir} RESULT_VARIABLE _rc)
            if(NOT _rc EQUAL 0)
                message(FATAL_ERROR "failed to apply ${_p} (component version changed?)")
            endif()
            message(STATUS "patch ${_name}: applied")
        endif()
    endif()
endforeach()
