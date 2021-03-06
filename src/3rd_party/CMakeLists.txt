
include_directories(.)

add_subdirectory(./yaml-cpp)
add_subdirectory(./SQLiteCpp)
add_subdirectory(./pathie-cpp)

if(USE_SENTENCEPIECE)
  if(USE_STATIC_LIBS)
    set(_ORIG_CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
    if(WIN32)
      list(INSERT CMAKE_FIND_LIBRARY_SUFFIXES 0 .lib .a)
    else()
      set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
    endif()
  endif()

  set(SPM_ENABLE_SHARED OFF CACHE BOOL "Builds shared libaries in addition to static libraries." FORCE)
  set(SPM_ENABLE_TCMALLOC ON CACHE BOOL "Enable TCMalloc if available." FORCE)
  set(SPM_TCMALLOC_STATIC ON CACHE BOOL "Link static library of TCMALLOC." FORCE)

  add_subdirectory(./sentencepiece)
  include_directories(./sentencepiece)

  set_target_properties(spm_encode spm_decode spm_train spm_normalize spm_export_vocab
                        PROPERTIES
                        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

  if(USE_STATIC_LIBS)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_ORIG_CMAKE_FIND_LIBRARY_SUFFIXES})
  endif()
endif(USE_SENTENCEPIECE)


include_directories(./SQLiteCpp/include)
include_directories(./CLI)
include_directories(./pathie-cpp/include)
