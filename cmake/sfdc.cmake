# Copyright © 2020 Michal Schulz <michal.schulz@gmx.de>
# https://github.com/michalsc
#
# This Source Code Form is subject to the terms of the
# Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
# with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

# the directory of this file, for the script that corrects the names inside the generated files
set(SFDC_FIX_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/sfdc-fix.cmake)

function(sfdc SFD_FILE)
    make_directory(${CMAKE_CURRENT_BINARY_DIR}/include/clib)
    make_directory(${CMAKE_CURRENT_BINARY_DIR}/include/pragmas)
    make_directory(${CMAKE_CURRENT_BINARY_DIR}/include/proto)
    make_directory(${CMAKE_CURRENT_BINARY_DIR}/include/inline)
    make_directory(${CMAKE_CURRENT_BINARY_DIR}/FD)

    get_filename_component(LIB_NAME_FULL ${SFD_FILE} NAME_WLE)
    string(REGEX REPLACE "_lib$" "" NAME ${LIB_NAME_FULL})

    # The files are named after the module (brcm-dma.fd, proto/brcm-dma.h, clib/brcm-dma_protos.h ...): Scout and the tools look for them that way.
    # sfdc turns a hyphen into an underscore, in the names it writes inside the files too: the name with the underscore is what it wrote
    # (CNAME), and the script puts the real one back (the name of the CMake target is CNAME, an identifier).
    string(REPLACE "-" "_" CNAME ${NAME})
    set(FIX ${CMAKE_COMMAND} -DFROM=${CNAME} -DTO=${NAME})

    add_custom_command(
        OUTPUT include/clib/${NAME}_protos.h
        COMMAND sfdc ${CMAKE_CURRENT_SOURCE_DIR}/${SFD_FILE} --mode=clib
                     --output=include/clib/${NAME}_protos.h
        COMMAND ${FIX} -DFILE=include/clib/${NAME}_protos.h -P ${SFDC_FIX_SCRIPT}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SFD_FILE}
        VERBATIM
    )

    add_custom_command(
        OUTPUT include/pragmas/${NAME}_pragmas.h
        COMMAND sfdc ${CMAKE_CURRENT_SOURCE_DIR}/${SFD_FILE} --mode=pragmas
                     --output=include/pragmas/${NAME}_pragmas.h
        COMMAND ${FIX} -DFILE=include/pragmas/${NAME}_pragmas.h -P ${SFDC_FIX_SCRIPT}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SFD_FILE}
        VERBATIM
    )

    add_custom_command(
        OUTPUT include/proto/${NAME}.h
        COMMAND sfdc ${CMAKE_CURRENT_SOURCE_DIR}/${SFD_FILE} --mode=proto
                     --output=include/proto/${NAME}.h
        COMMAND ${FIX} -DFILE=include/proto/${NAME}.h -P ${SFDC_FIX_SCRIPT}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SFD_FILE}
        VERBATIM
    )

    add_custom_command(
        OUTPUT include/inline/${NAME}.h
        COMMAND sfdc ${CMAKE_CURRENT_SOURCE_DIR}/${SFD_FILE} --mode=macros
                     --output=include/inline/${NAME}.h
        COMMAND ${FIX} -DFILE=include/inline/${NAME}.h -P ${SFDC_FIX_SCRIPT}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SFD_FILE}
        VERBATIM
    )

    add_custom_command(
        OUTPUT FD/${NAME}.fd
        COMMAND sfdc ${CMAKE_CURRENT_SOURCE_DIR}/${SFD_FILE} --mode=fd
                     --output=FD/${NAME}.fd
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SFD_FILE}
        VERBATIM
    )

    add_library(${CNAME} INTERFACE
        include/clib/${NAME}_protos.h
        include/pragmas/${NAME}_pragmas.h
        include/proto/${NAME}.h
        include/inline/${NAME}.h
        FD/${NAME}.fd
    )
    target_include_directories(${CNAME} INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/include)

endfunction(sfdc)
