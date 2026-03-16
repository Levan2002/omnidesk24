# Platform detection
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(OMNIDESK_PLATFORM_LINUX TRUE)
    set(OMNIDESK_PLATFORM_WINDOWS FALSE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(OMNIDESK_PLATFORM_LINUX FALSE)
    set(OMNIDESK_PLATFORM_WINDOWS TRUE)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

message(STATUS "OmniDesk24 platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "OmniDesk24 processor: ${CMAKE_SYSTEM_PROCESSOR}")
