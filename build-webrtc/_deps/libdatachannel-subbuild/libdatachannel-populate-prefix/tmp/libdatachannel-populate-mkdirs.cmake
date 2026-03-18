# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "C:/Users/levan/OneDrive/Desktop/omnidesk24/build-webrtc/_deps/libdatachannel-src")
  file(MAKE_DIRECTORY "C:/Users/levan/OneDrive/Desktop/omnidesk24/build-webrtc/_deps/libdatachannel-src")
endif()
file(MAKE_DIRECTORY
  "C:/Users/levan/OneDrive/Desktop/omnidesk24/build-webrtc/_deps/libdatachannel-build"
  "C:/Users/levan/OneDrive/Desktop/omnidesk24/build-webrtc/_deps/libdatachannel-subbuild/libdatachannel-populate-prefix"
  "C:/Users/levan/OneDrive/Desktop/omnidesk24/build-webrtc/_deps/libdatachannel-subbuild/libdatachannel-populate-prefix/tmp"
  "C:/Users/levan/OneDrive/Desktop/omnidesk24/build-webrtc/_deps/libdatachannel-subbuild/libdatachannel-populate-prefix/src/libdatachannel-populate-stamp"
  "C:/Users/levan/OneDrive/Desktop/omnidesk24/build-webrtc/_deps/libdatachannel-subbuild/libdatachannel-populate-prefix/src"
  "C:/Users/levan/OneDrive/Desktop/omnidesk24/build-webrtc/_deps/libdatachannel-subbuild/libdatachannel-populate-prefix/src/libdatachannel-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/levan/OneDrive/Desktop/omnidesk24/build-webrtc/_deps/libdatachannel-subbuild/libdatachannel-populate-prefix/src/libdatachannel-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/levan/OneDrive/Desktop/omnidesk24/build-webrtc/_deps/libdatachannel-subbuild/libdatachannel-populate-prefix/src/libdatachannel-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
