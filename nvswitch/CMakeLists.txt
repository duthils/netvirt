cmake_minimum_required(VERSION 2.6)

if(COMMAND cmake_policy)
        cmake_policy(SET CMP0003 NEW)
endif(COMMAND cmake_policy)

include_directories("../libnvcore/")
include_directories("../libnvcore/protocol/")

add_subdirectory(src)

install(FILES nvswitch.conf
        DESTINATION /etc/netvirt/)
