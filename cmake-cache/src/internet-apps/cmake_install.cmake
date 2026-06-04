# Install script for directory: /home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "debug")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/researchvm/Documents/DrGeordonResearch/ns-3.48/cmake-cache/src/internet-apps/examples/cmake_install.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libns3.48-internet-apps-debug.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libns3.48-internet-apps-debug.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libns3.48-internet-apps-debug.so"
         RPATH "/usr/local/lib:\$ORIGIN/:\$ORIGIN/../lib:/usr/local/lib64:\$ORIGIN/:\$ORIGIN/../lib64")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE SHARED_LIBRARY FILES "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/build/optimized/lib/libns3.48-internet-apps-debug.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libns3.48-internet-apps-debug.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libns3.48-internet-apps-debug.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libns3.48-internet-apps-debug.so"
         OLD_RPATH "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/build/optimized/lib:::::::::"
         NEW_RPATH "/usr/local/lib:\$ORIGIN/:\$ORIGIN/../lib:/usr/local/lib64:\$ORIGIN/:\$ORIGIN/../lib64")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libns3.48-internet-apps-debug.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/ns3" TYPE FILE FILES
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/helper/dhcp-helper.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/helper/dhcp6-helper.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/helper/ping-helper.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/helper/radvd-helper.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/helper/v4traceroute-helper.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/model/dhcp-client.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/model/dhcp-header.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/model/dhcp-server.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/model/dhcp6-client.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/model/dhcp6-duid.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/model/dhcp6-header.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/model/dhcp6-options.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/model/dhcp6-server.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/model/ping.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/model/radvd-interface.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/model/radvd-prefix.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/model/radvd.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/src/internet-apps/model/v4traceroute.h"
    "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/build/optimized/include/ns3/internet-apps-module.h"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/home/researchvm/Documents/DrGeordonResearch/ns-3.48/cmake-cache/src/internet-apps/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
