# Optional module: original model solvers never depend on animation classes.
set(HISTREAM_ANIMATION_RUNTIME_COPY)
if(NOT HISTREAM_ENABLE_SCENE_ANIMATION)
    return()
endif()

add_library(histream_scene_animation STATIC
    src/animation/scene_animation.cpp
    src/animation/transport/scene_transport.cpp
    src/animation/sensor/scene_sensor.cpp
    src/animation/observation/camera_observation.cpp
    src/animation/scene/material_table.cpp
    src/animation/scene/fire_sources.cpp
    src/animation/scene/supplemental_scenery.cpp
    src/animation/scene/mesh_scene.cpp
    src/animation/thermal/background_field.cpp
    src/animation/thermal/ground_thermal.cpp
    src/animation/sequence/mesh_timeline.cpp
    src/animation/collision/rigid_scene.cpp
    src/animation/render/compute_pass.cpp
    src/animation/render/mesh_sequence.cpp
    src/animation/bridge/histream_mesh_adapter.cpp
    src/animation/impact/blast.cpp
    src/animation/impact/penetration.cpp
    src/animation/impact/empirical.cpp
    src/animation/impact/event_model.cpp
    src/animation/render/mesh_renderer.cpp)
target_include_directories(histream_scene_animation PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" ${GDAL_INCLUDE_DIRS})
target_compile_definitions(histream_scene_animation PRIVATE ALLOC_DMA)
if(MSVC)
    target_compile_options(histream_scene_animation PRIVATE /utf-8)
endif()
target_link_libraries(histream_scene_animation PUBLIC
    nvpro_core ${GDAL_LIBRARIES} Eigen3::Eigen ${OpenCV_LIBS})

# Explicit opt-in: no downloads/dependency changes for core or legacy animation.
option(HISTREAM_ENABLE_ANIMATION_COLLISION "Native Bullet CPU contact/rigid animation" OFF)
if(HISTREAM_ENABLE_ANIMATION_COLLISION)
    include(FetchContent)
    FetchContent_Declare(histream_bullet
        URL "https://codeload.github.com/bulletphysics/bullet3/tar.gz/refs/tags/3.25"
        URL_HASH SHA256=c45afb6399e3f68036ddb641c6bf6f552bf332d5ab6be62f7e6c54eda05ceb77
        SOURCE_SUBDIR __histream_source_only__
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(histream_bullet)
    # Build just the three Bullet2 libraries; no demos, Python, OpenGL or install.
    file(GLOB_RECURSE BULLET_ANIMATION_SOURCES CONFIGURE_DEPENDS
        "${histream_bullet_SOURCE_DIR}/src/LinearMath/*.cpp"
        "${histream_bullet_SOURCE_DIR}/src/BulletCollision/*.cpp"
        "${histream_bullet_SOURCE_DIR}/src/BulletDynamics/*.cpp")
    add_library(histream_animation_bullet STATIC ${BULLET_ANIMATION_SOURCES})
    target_include_directories(histream_animation_bullet PUBLIC "${histream_bullet_SOURCE_DIR}/src")
    target_compile_definitions(histream_animation_bullet PUBLIC BT_USE_DOUBLE_PRECISION)
    target_compile_definitions(histream_scene_animation PRIVATE HISTREAM_ANIMATION_BULLET=1)
    target_link_libraries(histream_scene_animation PRIVATE histream_animation_bullet)
    add_executable(histream_collision_tests "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/scene-animation/tests/collision_fixture.cpp")
    set_target_properties(histream_collision_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_PATH}/$<CONFIG>/")
    target_include_directories(histream_collision_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(histream_collision_tests PRIVATE histream_scene_animation)
endif()
target_link_libraries(${PROJNAME} histream_scene_animation)
target_compile_definitions(${PROJNAME} PRIVATE HISTREAM_ENABLE_SCENE_ANIMATION=1)

set(HISTREAM_ANIMATION_SHADER_OUTPUTS)
foreach(KIND IN ITEMS scene transport sensor)
    set(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/${KIND}.comp")
    set(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shader/animation/${KIND}.comp.spv")
    set(HEADERS)
    if(NOT KIND STREQUAL "sensor")
        list(APPEND HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/geometry.glsl")
    endif()
    if(KIND STREQUAL "transport")
        list(APPEND HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/transport_solver.glsl")
    endif()
    add_custom_command(OUTPUT "${OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shader/animation"
        COMMAND "${HISTREAM_GLSLC}" --target-env=vulkan1.2 "${SOURCE}" -o "${OUTPUT}"
        DEPENDS "${SOURCE}" ${HEADERS} VERBATIM)
    list(APPEND HISTREAM_ANIMATION_SHADER_OUTPUTS "${OUTPUT}")
endforeach()
set(MESH_SHADER "${CMAKE_CURRENT_BINARY_DIR}/shader/animation/mesh/static.comp.spv")
add_custom_command(OUTPUT "${MESH_SHADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shader/animation/mesh"
    COMMAND "${HISTREAM_GLSLC}" --target-env=vulkan1.2
        "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/mesh/static.comp" -o "${MESH_SHADER}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/mesh/static.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/mesh/render_common.glsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/mesh/sequence_effects.glsl" VERBATIM)
list(APPEND HISTREAM_ANIMATION_SHADER_OUTPUTS "${MESH_SHADER}")
set(HISTREAM_MESH_SHADER_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/mesh/render_common.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/mesh/sequence_effects.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/mesh/physical_appearance.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/mesh/persistent_fire.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/mesh/natural_fire.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/mesh/varied_fire.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/mesh/ground_sampling.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/ground_layout.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/fire_sources.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/transport_solver.glsl")
foreach(KIND IN ITEMS sequence sequence_fire sequence_natural sequence_varied sequence_ground sequence_impact mask transport transport_impact)
    set(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/mesh/${KIND}.comp")
    set(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shader/animation/mesh/${KIND}.comp.spv")
    add_custom_command(OUTPUT "${OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shader/animation/mesh"
        COMMAND "${HISTREAM_GLSLC}" --target-env=vulkan1.2 "${SOURCE}" -o "${OUTPUT}"
        DEPENDS "${SOURCE}" ${HISTREAM_MESH_SHADER_HEADERS} VERBATIM)
    list(APPEND HISTREAM_ANIMATION_SHADER_OUTPUTS "${OUTPUT}")
endforeach()
# S3 instanced-geometry variants: identical sources compiled with MESH_INSTANCED (instance custom index offset).
foreach(KIND IN ITEMS sequence sequence_fire sequence_natural sequence_varied sequence_ground sequence_impact)
    set(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/mesh/${KIND}.comp")
    set(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shader/animation/mesh/${KIND}_instanced.comp.spv")
    add_custom_command(OUTPUT "${OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shader/animation/mesh"
        COMMAND "${HISTREAM_GLSLC}" --target-env=vulkan1.2 -DMESH_INSTANCED "${SOURCE}" -o "${OUTPUT}"
        DEPENDS "${SOURCE}" ${HISTREAM_MESH_SHADER_HEADERS} VERBATIM)
    list(APPEND HISTREAM_ANIMATION_SHADER_OUTPUTS "${OUTPUT}")
endforeach()
foreach(KIND IN ITEMS filter integrate detector display regional_filter)
    set(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/camera/${KIND}.comp")
    set(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shader/animation/camera/${KIND}.comp.spv")
    add_custom_command(OUTPUT "${OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shader/animation/camera"
        COMMAND "${HISTREAM_GLSLC}" --target-env=vulkan1.2 "${SOURCE}" -o "${OUTPUT}"
        DEPENDS "${SOURCE}" VERBATIM)
    list(APPEND HISTREAM_ANIMATION_SHADER_OUTPUTS "${OUTPUT}")
endforeach()
add_executable(histream_camera_tests EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/scene-animation/tests/camera_fixture.cpp")
target_sources(histream_camera_tests PRIVATE
    src/base/appsetting.cpp src/facetrt/facetrt_vulkan.cpp
    ${COMMON_SOURCE_FILES})
_add_project_definitions(histream_camera_tests)
target_compile_definitions(histream_camera_tests PRIVATE ALLOC_DMA)
if(MSVC)
    target_compile_options(histream_camera_tests PRIVATE /utf-8)
endif()
set_target_properties(histream_camera_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_PATH}/$<CONFIG>/")
target_include_directories(histream_camera_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(histream_camera_tests PRIVATE histream_scene_animation)
add_custom_target(histream_animation_shaders ALL DEPENDS ${HISTREAM_ANIMATION_SHADER_OUTPUTS})
set(GROUND_THERMAL_SHADER "${CMAKE_CURRENT_BINARY_DIR}/shader/animation/ground_thermal.comp.spv")
add_custom_command(OUTPUT "${GROUND_THERMAL_SHADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shader/animation"
    COMMAND "${HISTREAM_GLSLC}" --target-env=vulkan1.2 "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/ground_thermal.comp" -o "${GROUND_THERMAL_SHADER}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/ground_thermal.comp" "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/ground_layout.glsl" VERBATIM)
add_custom_target(histream_ground_shader DEPENDS "${GROUND_THERMAL_SHADER}")
add_dependencies(histream_scene_animation histream_ground_shader)
add_executable(histream_ground_tests EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/scene-animation/tests/ground_thermal_fixture.cpp")
target_sources(histream_ground_tests PRIVATE src/base/appsetting.cpp src/facetrt/facetrt_vulkan.cpp ${COMMON_SOURCE_FILES})
_add_project_definitions(histream_ground_tests)
target_compile_definitions(histream_ground_tests PRIVATE ALLOC_DMA)
if(MSVC)
    target_compile_options(histream_ground_tests PRIVATE /utf-8)
endif()
set_target_properties(histream_ground_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_PATH}/$<CONFIG>/")
target_include_directories(histream_ground_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(histream_ground_tests PRIVATE histream_scene_animation)
set(FIRE_GROWTH_PROBE "${CMAKE_CURRENT_BINARY_DIR}/tests/fire_growth_probe.comp.spv")
add_custom_command(OUTPUT "${FIRE_GROWTH_PROBE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/tests"
    COMMAND "${HISTREAM_GLSLC}" --target-env=vulkan1.2 "-I${CMAKE_CURRENT_SOURCE_DIR}/shader/animation"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/scene-animation/tests/fire_growth_probe.comp" -o "${FIRE_GROWTH_PROBE}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/scene-animation/tests/fire_growth_probe.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation/fire_sources.glsl" VERBATIM)
add_custom_target(histream_fire_growth_probe DEPENDS "${FIRE_GROWTH_PROBE}")
add_executable(histream_fire_growth_tests EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/scene-animation/tests/fire_growth_fixture.cpp")
target_sources(histream_fire_growth_tests PRIVATE src/base/appsetting.cpp src/facetrt/facetrt_vulkan.cpp ${COMMON_SOURCE_FILES})
_add_project_definitions(histream_fire_growth_tests)
target_compile_definitions(histream_fire_growth_tests PRIVATE ALLOC_DMA)
if(MSVC)
    target_compile_options(histream_fire_growth_tests PRIVATE /utf-8)
endif()
set_target_properties(histream_fire_growth_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_PATH}/$<CONFIG>/")
target_include_directories(histream_fire_growth_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(histream_fire_growth_tests PRIVATE histream_scene_animation)
add_dependencies(histream_fire_growth_tests histream_fire_growth_probe)
add_dependencies(histream_scene_animation histream_animation_shaders)
# impact-v1 empirical formula tests (standard library only; no GPU).
set(HISTREAM_IMPACT_FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/scene-animation/tests/impact_fixture.cpp")
if(EXISTS "${HISTREAM_IMPACT_FIXTURE}")
add_executable(histream_impact_tests EXCLUDE_FROM_ALL "${HISTREAM_IMPACT_FIXTURE}"
    src/animation/impact/blast.cpp src/animation/impact/penetration.cpp src/animation/impact/empirical.cpp)
if(MSVC)
    target_compile_options(histream_impact_tests PRIVATE /utf-8)
endif()
set_target_properties(histream_impact_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_PATH}/$<CONFIG>/")
target_include_directories(histream_impact_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
list(APPEND HISTREAM_ANIMATION_RUNTIME_COPY
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/shader/animation" "$<TARGET_FILE_DIR:${PROJNAME}>/shader/animation"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_BINARY_DIR}/shader/animation" "$<TARGET_FILE_DIR:${PROJNAME}>/shader/animation")
