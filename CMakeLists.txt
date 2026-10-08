cmake_minimum_required(VERSION 3.14.0)
project(brcm-dma VERSION 0.1.0)

# "make install" gathers the products in build/install/ (CMake itself works in build/cmake/) (kernel/ the module and its blob for the ROM of Emu68, sdk/ what a developer needs,
# tools/ the programs for the Amiga) unless another prefix is given
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/build/install CACHE PATH "Install prefix" FORCE)
endif()

# The dependencies are submodules, and are used as such. devicetree.resource and mailbox.resource are projects of their own: their SFD generates the stubs
# (proto/devicetree.h, proto/mailbox.h) in their own build directory and the CMake targets devicetree and mailbox carry the include directory. They are
# not built for themselves (EXCLUDE_FROM_ALL: they go to the ROM of Emu68 on their own). mailbox.resource has a devicetree.resource of its own as a submodule:
# not needed, the target devicetree exists already (see its CMakeLists.txt).
if(NOT TARGET devicetree)
    add_subdirectory(devicetree.resource EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET mailbox)
    add_subdirectory(mailbox.resource EXCLUDE_FROM_ALL)
endif()

# gic400.library ships its headers already generated (proto/, inline/, clib/, pragmas/, libraries/gic400.h): only those are used. The project itself is
# not added: it needs Emu68Common to be built, and the library is in the ROM of Emu68 anyway.
add_library(gic400 INTERFACE)
target_include_directories(gic400 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/gic400.library/include)

# What is ours comes last: the projects above define a bin_to_header(), a get_verstring() and an sfdc() of their own, and the last definition is the one
# that stays (the sfdc() of this repository names the generated files after the module, see cmake/sfdc.cmake)
include(cmake/bin2h.cmake)
include(cmake/verstring.cmake)
include(cmake/sfdc.cmake)

get_verstring(VERSTRING)
sfdc(sfd/brcm-dma_lib.sfd)

add_executable(brcm-dma.resource
    src/main.c
    src/init.c
    src/tags.c
    src/utility.c
    src/engine.c
    src/vc6.c
    src/vc4.c
    src/mbox.c
    src/bdma_queryinfotaglist.c
    src/job.c
    src/bdma_openclienttaglist.c
    src/bdma_closeclient.c
    src/bdma_allocjobtaglist.c
    src/bdma_setjobtaglist.c
    src/bdma_startjob.c
    src/bdma_freejob.c
    src/bdma_queryjobtaglist.c
    src/bdma_addjobtaglist.c
    src/bdma_waitjob.c
    src/bdma_checkjob.c
    src/bdma_abortjob.c
)

bin_to_header(brcm-dma.resource)

target_compile_options(brcm-dma.resource PRIVATE -Os -m68040 -msmall-code -fomit-frame-pointer -Wall -Wno-pointer-sign -Wno-main)
target_compile_definitions(brcm-dma.resource PRIVATE VERSION_STRING="${VERSTRING}")

target_include_directories(brcm-dma.resource PRIVATE include src)
target_link_options(brcm-dma.resource PRIVATE -ffreestanding -nostdlib -nostartfiles -s -Wl,-e_code_start -Wl,-T${CMAKE_CURRENT_SOURCE_DIR}/ldscript.lds)
target_link_libraries(brcm-dma.resource devicetree gic400 mailbox)

set_target_properties(brcm-dma.resource PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ldscript.lds)

add_executable(brcm-dma-test
    test/startup.c
    test/test.c
)

target_compile_options(brcm-dma-test PRIVATE -Os -m68040 -fomit-frame-pointer -Wall -Wno-pointer-sign -Wno-main)
target_include_directories(brcm-dma-test PRIVATE include src)
target_link_options(brcm-dma-test PRIVATE -ffreestanding -m68040 -nostdlib -nostartfiles -Wl,-e,__start)
target_link_libraries(brcm-dma-test amiga brcm_dma gic400)

add_executable(brcm-dma-stress
    test/startup.c
    test/stress.c
)

target_compile_options(brcm-dma-stress PRIVATE -Os -m68040 -fomit-frame-pointer -Wall -Wno-pointer-sign -Wno-main)
target_include_directories(brcm-dma-stress PRIVATE include src)
target_link_options(brcm-dma-stress PRIVATE -ffreestanding -m68040 -nostdlib -nostartfiles -Wl,-e,__start)
target_link_libraries(brcm-dma-stress amiga brcm_dma mailbox)

add_executable(brcm-dma-lab
    test/startup.c
    test/lab.c
    src/mbox.c
)

target_compile_options(brcm-dma-lab PRIVATE -Os -m68040 -fomit-frame-pointer -Wall -Wno-pointer-sign -Wno-main)
target_include_directories(brcm-dma-lab PRIVATE include src)
target_link_options(brcm-dma-lab PRIVATE -ffreestanding -m68040 -nostdlib -nostartfiles -Wl,-e,__start)
target_link_libraries(brcm-dma-lab amiga mailbox)

add_executable(brcm-dma-cost
    test/startup.c
    test/cost.c
)

target_compile_options(brcm-dma-cost PRIVATE -Os -m68040 -fomit-frame-pointer -Wall -Wno-pointer-sign -Wno-main)
target_include_directories(brcm-dma-cost PRIVATE include src)
target_link_options(brcm-dma-cost PRIVATE -ffreestanding -m68040 -nostdlib -nostartfiles -Wl,-e,__start)
target_link_libraries(brcm-dma-cost amiga brcm_dma)

add_executable(brcm-dma-stack
    test/startup.c
    test/stack.c
)

target_compile_options(brcm-dma-stack PRIVATE -Os -m68040 -fomit-frame-pointer -Wall -Wno-pointer-sign -Wno-main)
target_include_directories(brcm-dma-stack PRIVATE include src)
target_link_options(brcm-dma-stack PRIVATE -ffreestanding -m68040 -nostdlib -nostartfiles -Wl,-e,__start)
target_link_libraries(brcm-dma-stack amiga brcm_dma)

add_executable(brcm-dma-info
    test/startup.c
    test/info.c
)

target_compile_options(brcm-dma-info PRIVATE -Os -m68040 -fomit-frame-pointer -Wall -Wno-pointer-sign -Wno-main)
target_include_directories(brcm-dma-info PRIVATE include src)
target_link_options(brcm-dma-info PRIVATE -ffreestanding -m68040 -nostdlib -nostartfiles -Wl,-e,__start)
target_link_libraries(brcm-dma-info amiga brcm_dma)

add_custom_target(brcm-dma_autodoc ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/Autodocs
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/Autodocs/brcm-dma.doc ${CMAKE_CURRENT_BINARY_DIR}/Autodocs/brcm-dma.doc
    COMMENT "Copying Autodocs/brcm-dma.doc into build/"
)

add_custom_target(brcm-dma_sfd ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/SFD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/sfd/brcm-dma_lib.sfd ${CMAKE_CURRENT_BINARY_DIR}/SFD/brcm-dma_lib.sfd
    COMMENT "Copying sfd/brcm-dma_lib.sfd into build/"
)

add_custom_target(brcm-dma_resources_header ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/include/resources
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/include/resources/brcm-dma.h ${CMAKE_CURRENT_BINARY_DIR}/include/resources/brcm-dma.h
    COMMENT "Copying include/resources/brcm-dma.h into build/"
)

# no install for devicetree.resource itself, it goes to ROM
# the files themselves, not install(TARGETS): the executables are linked by a linker script, there is no relink step to run
install(PROGRAMS $<TARGET_FILE:brcm-dma.resource> DESTINATION kernel)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/brcm-dma.resource.h DESTINATION kernel)

install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/ DESTINATION sdk/include)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/FD/ DESTINATION sdk/FD)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/SFD/ DESTINATION sdk/SFD)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Autodocs/ DESTINATION sdk/Autodocs)

foreach(tool brcm-dma-test brcm-dma-stress brcm-dma-cost brcm-dma-lab brcm-dma-stack brcm-dma-info)
    install(PROGRAMS $<TARGET_FILE:${tool}> DESTINATION tools)
endforeach()
