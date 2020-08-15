# - Find the native GtkOSXApplication includes and library
#
# This module defines
#  MacIntegration_INCLUDE_DIR, where to find gtkosxapplication.h, etc.
#  MacIntegration_LIBRARIES, the libraries to link against to use GtkOSXApplication.
#  MacIntegration_FOUND, If false, do not try to use GtkOSXApplication.

#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

find_path(MACINTEGRATION_INCLUDE_DIR gtkosxapplication.h PATH_SUFFIXES gtkmacintegration gtkmacintegration-gtk3)
mark_as_advanced(MACINTEGRATION_INCLUDE_DIR)

set(MacIntegration_NAMES ${MacIntegration_NAMES} gtkMacIntegration libgtkMacIntegration gtkMacIntegration-gtk3 libgtkMacIntegration-gtk3)
find_library(MacIntegration_LIBRARY NAMES ${MacIntegration_NAMES})
mark_as_advanced(MacIntegration_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set MacIntegration_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MacIntegration DEFAULT_MSG MACINTEGRATION_LIBRARY MACINTEGRATION_INCLUDE_DIR)

IF(MacIntegration_FOUND)
  SET(MacIntegration_LIBRARIES ${MACINTEGRATION_LIBRARY})
  SET(MacIntegration_INCLUDE_DIRS ${MACINTEGRATION_INCLUDE_DIR})
ENDIF(MacIntegration_FOUND)
