# The headers of the libraries that the resource calls (gic400.library, mailbox.resource), generated from their SFD:
# private to this build, not part of the headers that it publishes in include/.
cmake_minimum_required(VERSION 3.14.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/sfdc.cmake)

sfdc(gic400_lib.sfd)
sfdc(mailbox_lib.sfd)
