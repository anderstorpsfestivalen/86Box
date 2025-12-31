#
# 86Box    A hypervisor and IBM PC system emulator that specializes in
#          running old operating systems and software designed for IBM
#          PC systems and compatibles from 1981 through fairly recent
#          system designs based on the PCI bus.
#
#          This file is part of the 86Box distribution.
#
#          CMake build script for headless/library mode.
#
# Authors: Juke Project
#
#          Copyright 2024 Juke Project.
#

# Headless platform implementation
# Uses real Unix platform code but with headless UI stubs
add_library(plat OBJECT
    headless.c
    ../unix/unix_serial_passthrough.c
    ../unix/unix_netsocket.c
)

if (NOT CPPTHREADS)
    target_sources(plat PRIVATE ../unix/unix_thread.c)
endif()

# Empty UI library for headless mode (just the cdrom ioctl stub)
add_library(ui OBJECT
    ../unix/dummy_cdrom_ioctl.c
)

set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)
target_link_libraries(${86BOX_TARGET} Threads::Threads)

if(APPLE)
    target_link_libraries(${86BOX_TARGET} "-framework CoreFoundation")
endif()
