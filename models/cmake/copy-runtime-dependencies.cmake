# Copy dependency runtime files without redeploying debug/linker build artifacts.
if(NOT DEFINED SOURCE_DIR OR NOT IS_DIRECTORY "${SOURCE_DIR}")
    message(FATAL_ERROR "Runtime dependency source directory is missing: ${SOURCE_DIR}")
endif()
if(NOT DEFINED DESTINATION_DIR OR NOT IS_ABSOLUTE "${DESTINATION_DIR}")
    message(FATAL_ERROR "Runtime dependency destination must be an absolute path")
endif()

file(COPY "${SOURCE_DIR}/" DESTINATION "${DESTINATION_DIR}"
    PATTERN "*.pdb" EXCLUDE
    PATTERN "*.lib" EXCLUDE
    PATTERN "*.exp" EXCLUDE
    PATTERN "*.ilk" EXCLUDE)
