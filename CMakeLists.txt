cmake_minimum_required(VERSION 3.23)

project(orcaSDK VERSION 1.1.0)

# Option to enable FTDI D2XX support (for non-standard baud rates on macOS)
option(orcaSDK_USE_FTDI "Enable FTDI D2XX support for non-standard baud rates" ON)

#TODO(Aiden Oct 31, 2024): 
#	Address when we've decided how to handle platform dependencies. Add conditional 
#	compilation of platform specific code. See Professional CMake section 16.2.6 for suggestions
add_library(orcaSDK_core 
    src/actuator.cpp
    src/orca_stream.cpp
    src/standard_modbus_functions.cpp
    src/command_and_confirm.cpp
)
add_library(orcaSDK::core ALIAS orcaSDK_core)

target_sources(orcaSDK_core
    PUBLIC
    	FILE_SET publicAPI
    	TYPE HEADERS
    FILES
    	actuator.h

		src/orca_modes.h
		src/orca_stream_config.h
		src/serial_interface.h
		src/message_priority.h
		src/command_and_confirm.h
		src/clock.h
		src/error_types.h
		src/command_stream_structs.h
		tools/log.h
		tools/log_interface.h
		tools/timer.h

		src/chrono_clock.h
		src/diagnostics_tracker.h
		src/function_code_parameters.h
		src/constants.h

		src/mb_crc.h
		src/message_queue.h
		src/modbus_client.h
		src/standard_modbus_functions.h
		src/orca_stream.h
		src/transaction.h
		src/SerialASIO.h
		src/SerialFTDI.h
)

target_compile_features(orcaSDK_core PUBLIC cxx_std_11)

include(GNUInstallDirs)
option(orcaSDK_VENDOR_ASIO "Should the SDK automatically download asio?" ON)
include(ASIO.cmake)
option(orcaSDK_VENDOR_ORCAAPI "Should the SDK automatically download orcaAPI?" ON)
include(orcaAPI.cmake)

# FTDI D2XX support
if(orcaSDK_USE_FTDI)
    # Find the FTDI library
    find_library(FTDI_LIBRARY 
        NAMES ftd2xx libftd2xx
        PATHS /usr/local/lib
        REQUIRED
    )
    
    # Find the FTDI headers
    find_path(FTDI_INCLUDE_DIR 
        NAMES ftd2xx.h
        PATHS /usr/local/include
        REQUIRED
    )
    
    message(STATUS "FTDI D2XX library found: ${FTDI_LIBRARY}")
    message(STATUS "FTDI D2XX headers found: ${FTDI_INCLUDE_DIR}")
    
    target_include_directories(orcaSDK_core PUBLIC ${FTDI_INCLUDE_DIR})
    target_link_libraries(orcaSDK_core PUBLIC ${FTDI_LIBRARY})
    target_compile_definitions(orcaSDK_core PUBLIC ORCASDK_HAS_FTDI=1)
endif()

set_target_properties(orcaSDK_core PROPERTIES
    VERSION ${orcaSDK_VERSION}
    SOVERSION ${orcaSDK_VERSION_MAJOR}
    EXPORT_NAME core
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    orcaSDKConfigVersion.cmake
    VERSION ${orcaSDK_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(TARGETS orcaSDK_core
    EXPORT orcaSDKTargets
    FILE_SET publicAPI
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT orcaSDKTargets 
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/orcaSDK
    NAMESPACE orcaSDK::
)

install(
    FILES
    	"orcaSDKConfig.cmake"
    DESTINATION
    	"${CMAKE_INSTALL_LIBDIR}/cmake/orcaSDK"
)

set(orcaSDK_TEST "false" CACHE BOOL "Should the orcaSDK test targets be included?")
if (orcaSDK_TEST)
    add_subdirectory(tests)
endif()