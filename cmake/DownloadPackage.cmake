#####################################################################################
# Downloads the URL to FILENAME and extracts its content if EXTRACT option is present
# ZIP files should have a folder of the name of the archive
# - ex. foo.zip -> foo/<data>
# Arguments:
#  FILENAMES   : all filenames to download
#  URLS        : if present, a custom download URL for each FILENAME.
#                If only one FILENAME is provided, a list of alternate download locations.
#                Defaults to ${NVPRO_CORE2_DOWNLOAD_SITE}${SOURCE_DIR}/${FILENAME}.
#  EXTRACT     : if present, will extract the content of the file
#  NOINSTALL   : if present, will not make files part of install
#  INSTALL_DIR : folder for the 'install' build, default is 'media' next to the executable
#  TARGET_DIR  : folder where to download to, default is {DOWNLOAD_TARGET_DIR}
#  SOURCE_DIR  : folder on server, if not present 'scenes'
#
# Examples:
# download_files(FILENAMES sample1.zip EXTRACT) 
# download_files(FILENAMES env.hdr)
# download_files(FILENAMES zlib.zip EXTRACT TARGET_DIR ${BASE_DIRECTORY}/blah SOURCE_DIR /libraries NOINSTALL) 
# 
function(download_files)
  set(options EXTRACT NOINSTALL)
  set(oneValueArgs INSTALL_DIR SOURCE_DIR TARGET_DIR)
  set(multiValueArgs FILENAMES URLS)
  cmake_parse_arguments(DOWNLOAD_FILES  "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN} )

  if(NOT DEFINED DOWNLOAD_FILES_INSTALL_DIR)
    set(DOWNLOAD_FILES_INSTALL_DIR "resources")
  endif()
  if(NOT DEFINED DOWNLOAD_FILES_SOURCE_DIR)
    set(DOWNLOAD_FILES_SOURCE_DIR "")
  endif()
  if(NOT DEFINED DOWNLOAD_FILES_TARGET_DIR)
    set(DOWNLOAD_FILES_TARGET_DIR ${NVPRO_CORE2_DOWNLOAD_DIR})
  endif()

  # Check each file to download
  # In CMake 3.17+, we can change this to _FILENAME_URL IN ZIP_LISTS DOWNLOAD_FILES_FILENAMES DOWNLOAD_FILES_URLS
  list(LENGTH DOWNLOAD_FILES_FILENAMES _NUM_DOWNLOADS)
  math(EXPR _DOWNLOAD_IDX_MAX "${_NUM_DOWNLOADS}-1")
  foreach(_DOWNLOAD_IDX RANGE ${_DOWNLOAD_IDX_MAX})
    list(GET DOWNLOAD_FILES_FILENAMES ${_DOWNLOAD_IDX} FILENAME)
    set(_TARGET_FILENAME ${DOWNLOAD_FILES_TARGET_DIR}/${FILENAME})
    
    set(_DO_DOWNLOAD ON)
    if(EXISTS ${_TARGET_FILENAME})
      file(SIZE ${_TARGET_FILENAME} _FILE_SIZE)
      if(${_FILE_SIZE} GREATER 0)
        set(_DO_DOWNLOAD OFF)
      endif()
    endif()
    
    if(_DO_DOWNLOAD)
      if(DOWNLOAD_FILES_URLS AND (_NUM_DOWNLOADS GREATER 1)) # One URL per file
        list(GET DOWNLOAD_FILES_URLS ${_DOWNLOAD_IDX} _DOWNLOAD_URLS)
      elseif(DOWNLOAD_FILES_URLS) # One file, multiple URLs
        set(_DOWNLOAD_URLS ${DOWNLOAD_FILES_URLS})
      else()
        set(_DOWNLOAD_URLS ${NVPRO_CORE2_DOWNLOAD_SITE}${DOWNLOAD_FILES_SOURCE_DIR}/${FILENAME})
      endif()
      
      foreach(_DOWNLOAD_URL ${_DOWNLOAD_URLS})
        message(STATUS "Downloading ${_DOWNLOAD_URL} to ${_TARGET_FILENAME}")
        
        file(DOWNLOAD ${_DOWNLOAD_URL} ${_TARGET_FILENAME}
          SHOW_PROGRESS
          STATUS _DOWNLOAD_STATUS)

        # Check whether the download succeeded. _DOWNLOAD_STATUS is a list of
        # length 2; element 0 is the return value (0 == no error), element 1 is
        # a string value for the error.
        list(GET _DOWNLOAD_STATUS 0 _DOWNLOAD_STATUS_CODE)
        if(${_DOWNLOAD_STATUS_CODE} EQUAL 0)
          break() # Successful download!
        else()
          list(GET _DOWNLOAD_STATUS 1 _DOWNLOAD_STATUS_MESSAGE)
          # CMake usually creates a 0-byte file in this case. Remove it:
          file(REMOVE ${_TARGET_FILENAME})
          message(WARNING "Download of ${_DOWNLOAD_URL} to ${_TARGET_FILENAME} failed with code ${_DOWNLOAD_STATUS_CODE}: ${_DOWNLOAD_STATUS_MESSAGE}")
        endif()
      endforeach()
      
      if(NOT EXISTS ${_TARGET_FILENAME})
        message(FATAL_ERROR "All possible downloads to ${_TARGET_FILENAME} failed. See above warnings for more info.")
      endif()
  
      # Extracting the ZIP file
	    if(DOWNLOAD_FILES_EXTRACT)
		    execute_process(COMMAND ${CMAKE_COMMAND} -E tar -xf ${_TARGET_FILENAME}
						          WORKING_DIRECTORY ${DOWNLOAD_FILES_TARGET_DIR})
        # We could use ARCHIVE_EXTRACT instead, but it needs CMake 3.18+:
        # file(ARCHIVE_EXTRACT INPUT ${_TARGET_FILENAME}
        #      DESTINATION ${DOWNLOAD_FILES_TARGET_DIR})
      endif()
    endif()

    # Installing the files or directory
    if (NOT DOWNLOAD_FILES_NOINSTALL)
      if(DOWNLOAD_FILES_EXTRACT)
       get_filename_component(FILE_DIR ${FILENAME} NAME_WE)
       install(DIRECTORY ${DOWNLOAD_FILES_TARGET_DIR}/${FILE_DIR} DESTINATION "${DOWNLOAD_FILES_INSTALL_DIR}")
      else()
       install(FILES ${_TARGET_FILENAME} DESTINATION "${DOWNLOAD_FILES_INSTALL_DIR}")
      endif()
    endif()
  endforeach()
endfunction()


#####################################################################################
# Downloads and extracts a package of source code and places it in
# downloaded_resources, if it doesn't already exist there. By default, downloads
# from the nvpro-samples server.
# Sets the variable in the DIR argument to its location.
# If it doesn't exist and couldn't be found, produces a fatal error.
# This is intended as a sort of lightweight package manager, for packages that
# are used by 2 or fewer samples, can be downloaded without authentication, and
# are not fundamental graphics APIs (like DirectX 12 or OptiX).
#
# Arguments:
#  NAME     : The name of the package to find. E.g. NVAPI looks for
#             a folder named NVAPI or downloads NVAPI.zip.
#  URLS     : Optional path to an archive to download. By default, this
#             downloads from ${NVPRO_CORE2_DOWNLOAD_SITE}/libraries/${NAME}-${VERSION}.zip.
#             If more than one URL is specified, tries them in turn until one works.
#  VERSION  : The package's version number, like "555.0.0" or "1.1.0".
#  LOCATION : Will be set to the package's directory.
#
function(download_package)
  set(oneValueArgs NAME VERSION LOCATION)
  set(multiValueArgs URLS)
  cmake_parse_arguments(DOWNLOAD_PACKAGE "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN} )
  
  set(_TARGET_DIR ${CMAKE_BINARY_DIR}/_deps/${DOWNLOAD_PACKAGE_NAME}-${DOWNLOAD_PACKAGE_VERSION})
  if(EXISTS ${_TARGET_DIR})
    # An empty directory is not a valid cache entry; that usually indicates
    # a failed download.
    file(GLOB _TARGET_DIR_FILES "${_TARGET_DIR}/*")
    list(LENGTH _TARGET_DIR_FILES _TARGET_DIR_NUM_FILES)
    if(_TARGET_DIR_NUM_FILES GREATER 0)
      set(${DOWNLOAD_PACKAGE_LOCATION} ${_TARGET_DIR} PARENT_SCOPE)
      return()
    endif()
  endif()
  
  # Cache couldn't be used. Download the package:
  if(DOWNLOAD_PACKAGE_URLS)
    set(_URLS ${DOWNLOAD_PACKAGE_URLS})
  else()
    set(_URLS ${NVPRO_CORE2_DOWNLOAD_SITE}/libraries/${DOWNLOAD_PACKAGE_NAME}-${DOWNLOAD_PACKAGE_VERSION}.zip)
  endif()
  download_files(FILENAMES "${DOWNLOAD_PACKAGE_NAME}.zip"
                 URLS ${_URLS}
                 EXTRACT
                 TARGET_DIR ${_TARGET_DIR}
                 NOINSTALL
  )
  # Save some space by cleaning up the archive we extracted from.
  file(REMOVE ${_TARGET_DIR}/${DOWNLOAD_PACKAGE_NAME}.zip)
  
  set(${DOWNLOAD_PACKAGE_LOCATION} ${_TARGET_DIR} PARENT_SCOPE)
endfunction()
