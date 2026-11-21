# ESP-IDF component registration for the application sources.
# Only C sources are compiled; llm/ also holds the generated tokenizer blob.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/src/*.S)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "llm" "llm/dsp"
)
