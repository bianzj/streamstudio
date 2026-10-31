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

find_package(GDAL CONFIG REQUIRED)
find_package(netCDF CONFIG REQUIRED)
target_link_libraries(stream PRIVATE GDAL::GDAL netCDF::netcdf)

add_custom_command(TARGET stream POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/defined"
                "$<TARGET_FILE_DIR:stream>/stream-defined"
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_CURRENT_SOURCE_DIR}/classes/classes.json"
                "$<TARGET_FILE_DIR:stream>/stream-defined/classes.json"
        COMMAND ${CMAKE_COMMAND}
                "-DSOURCE_DIR=$<IF:$<CONFIG:Debug>,${VCPKG_INSTALLED_DIR}/x64-windows/debug/bin,${VCPKG_INSTALLED_DIR}/x64-windows/bin>"
                "-DDESTINATION_DIR=$<TARGET_FILE_DIR:stream>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/copy-runtime-dependencies.cmake"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${VCPKG_INSTALLED_DIR}/x64-windows/share/proj"
                "$<TARGET_FILE_DIR:stream>/proj"
        COMMENT "Deploying STREAM coefficients, class table, runtime libraries and PROJ database")
