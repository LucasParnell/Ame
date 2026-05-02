add_library(libk STATIC
    libk/string.cc
    libk/printf.cc
    libk/panic.cc
)

target_include_directories(libk PUBLIC include)
