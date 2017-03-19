project(libff2)

find_package(FFmpeg REQUIRED
	COMPONENTS avcodec avdevice avutil avformat)

include_directories(
	${CMAKE_SOURCE_DIR}/libobs
	${FFMPEG_INCLUDE_DIRS}
	)

set(libff2_HEADERS
	libff2/decode.h
	libff2/media.h
	)
set(libff2_SOURCES
	libff2/decode.c
	libff2/media.c
	)

add_library(libff2 STATIC
	${libff2_HEADERS}
	${libff2_SOURCES}
	)
target_include_directories(libff2
	PUBLIC .
	)

if(NOT MSVC)
        if(NOT MINGW)
                target_compile_options(libff2 PRIVATE -fPIC)
        endif()
endif()

target_link_libraries(libff2
	${FFMPEG_LIBRARIES}
	)
