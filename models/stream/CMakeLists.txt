cmake_minimum_required(VERSION 3.20)
project(stream LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shares the runtime directory with histream.exe: both engines are built against the same
# vcpkg tree, so the GDAL/netCDF/HDF5 libraries are deployed once. STREAM data stays in stream-defined/.
set(STREAM_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bin_x64" CACHE PATH "Engine runtime output root")

add_executable(stream
        app/main.cpp
        app/assimilation.cpp
        app/config.cpp
        app/image.cpp
        app/inversion.cpp
        app/thermal_view.cpp
        forcing/era5.cpp
        forcing/forcing_metadata.cpp
        forcing/forcing_source.cpp
        forcing/pixel_forcing.cpp
        forcing/utc.cpp
        physics/canopy.cpp
        physics/effective_lai.cpp
        physics/energy.cpp
        physics/heat_conduction.cpp
        physics/optics.cpp
        physics/radiosity.cpp
        physics/shortwave.cpp
        physics/spa.cpp
        scene/classes.cpp
        scene/raster.cpp)
set_target_properties(stream PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${STREAM_RUNTIME_DIR}/$<CONFIG>/")

if (MSVC)
    target_compile_options(stream PRIVATE /utf-8 /bigobj /openmp /wd4244 /wd4267 /wd4305 /wd4996)
    target_compile_definitions(stream PRIVATE NOMINMAX _USE_MATH_DEFINES)
else ()
    find_package(OpenMP REQUIRED COMPONENTS CXX)
    target_link_libraries(stream PRIVATE OpenMP::OpenMP_CXX)
endif ()

# FIX(2026-11-21 B-S5): GDAL / netCDF are found through their CMake package configs on every platform (vcpkg on
# Windows, the system packages on Linux, e.g. Ubuntu libgdal-dev / libnetcdf-dev); a module / pkg-config fallback
# covers distributions that ship no config file.
find_package(GDAL CONFIG QUIET)
if (NOT TARGET GDAL::GDAL)
    find_package(GDAL REQUIRED)
endif ()
find_package(netCDF CONFIG QUIET)
if (TARGET netCDF::netcdf)
    set(STREAM_NETCDF_TARGET netCDF::netcdf)
else ()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(STREAM_NETCDF REQUIRED IMPORTED_TARGET netcdf)
    set(STREAM_NETCDF_TARGET PkgConfig::STREAM_NETCDF)
endif ()
target_link_libraries(stream PRIVATE GDAL::GDAL ${STREAM_NETCDF_TARGET})

# FIX(2026-11-21 B-S5): the coefficients and the class table are deployed on every platform; the runtime DLLs and
# the PROJ database are copied only for a Windows vcpkg build (the former unconditional x64-windows paths made the
# Linux build fail in the copy step). Windows behaviour is unchanged: same triplet (default x64-windows), same files.
add_custom_command(TARGET stream POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/defined"
                "$<TARGET_FILE_DIR:stream>/stream-defined"
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_CURRENT_SOURCE_DIR}/classes/classes.json"
                "$<TARGET_FILE_DIR:stream>/stream-defined/classes.json"
        COMMENT "Deploying STREAM coefficients and class table")
if (WIN32 AND DEFINED VCPKG_INSTALLED_DIR)
    if (DEFINED VCPKG_TARGET_TRIPLET AND NOT VCPKG_TARGET_TRIPLET STREQUAL "")
        set(STREAM_VCPKG_TRIPLET "${VCPKG_TARGET_TRIPLET}")
    else ()
        set(STREAM_VCPKG_TRIPLET "x64-windows")
    endif ()
    set(STREAM_VCPKG_ROOT "${VCPKG_INSTALLED_DIR}/${STREAM_VCPKG_TRIPLET}")
    set(STREAM_COPY_RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/copy-runtime-dependencies.cmake")
    if (EXISTS "${STREAM_COPY_RUNTIME}")
        add_custom_command(TARGET stream POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                        "-DSOURCE_DIR=$<IF:$<CONFIG:Debug>,${STREAM_VCPKG_ROOT}/debug/bin,${STREAM_VCPKG_ROOT}/bin>"
                        "-DDESTINATION_DIR=$<TARGET_FILE_DIR:stream>"
                        -P "${STREAM_COPY_RUNTIME}"
                COMMENT "Deploying STREAM runtime libraries (${STREAM_VCPKG_TRIPLET})")
    else ()
        message(WARNING "copy-runtime-dependencies.cmake not found; runtime DLLs are not deployed next to stream.exe")
    endif ()
    if (EXISTS "${STREAM_VCPKG_ROOT}/share/proj")
        add_custom_command(TARGET stream POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${STREAM_VCPKG_ROOT}/share/proj"
                        "$<TARGET_FILE_DIR:stream>/proj"
                COMMENT "Deploying the PROJ database")
    endif ()
endif ()
