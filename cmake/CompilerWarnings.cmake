# Warning helpers, kept out of the top-level file.

function(proxy_set_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion              # a silently narrowed byte count is a correctness bug
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2)
endfunction()

if(PROXY_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
