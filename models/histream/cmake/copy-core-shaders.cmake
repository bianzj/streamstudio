# Never remove deployed user files. Clean OFF staging is verified independently.
file(GLOB_RECURSE SOURCES LIST_DIRECTORIES false RELATIVE "${SOURCE}" "${SOURCE}/*")
list(FILTER SOURCES EXCLUDE REGEX "^animation/")
foreach(FILE IN LISTS SOURCES)
    get_filename_component(DIRECTORY "${DESTINATION}/${FILE}" DIRECTORY)
    file(MAKE_DIRECTORY "${DIRECTORY}")
    configure_file("${SOURCE}/${FILE}" "${DESTINATION}/${FILE}" COPYONLY)
endforeach()
