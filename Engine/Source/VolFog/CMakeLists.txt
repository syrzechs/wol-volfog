file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(VolFog STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(VolFog
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)
