#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "StateMachineLib" for configuration "Debug"
set_property(TARGET StateMachineLib APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(StateMachineLib PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libStateMachineLib.a"
  )

list(APPEND _IMPORT_CHECK_TARGETS StateMachineLib )
list(APPEND _IMPORT_CHECK_FILES_FOR_StateMachineLib "${_IMPORT_PREFIX}/lib/libStateMachineLib.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
