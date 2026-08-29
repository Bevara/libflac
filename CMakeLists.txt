cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBFLAC_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/reframe_flac.c
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_flac.c
)

SET(LIBFLAC_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libFLAC.a
)

SET(LIBFLAC_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libflac
        "${LIBFLAC_SRC}"
        "${LIBFLAC_LIB}"
        []
        "FLAC__NO_DLL"
        "${LIBFLAC_INC}"
        ""
        "1")
