# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/nini/.espressif/v5.5.4/esp-idf/components/bootloader/subproject"
  "/home/nini/esp-claw-master (1)/esp-claw-master/application/edge_agent/build/bootloader"
  "/home/nini/esp-claw-master (1)/esp-claw-master/application/edge_agent/build/bootloader-prefix"
  "/home/nini/esp-claw-master (1)/esp-claw-master/application/edge_agent/build/bootloader-prefix/tmp"
  "/home/nini/esp-claw-master (1)/esp-claw-master/application/edge_agent/build/bootloader-prefix/src/bootloader-stamp"
  "/home/nini/esp-claw-master (1)/esp-claw-master/application/edge_agent/build/bootloader-prefix/src"
  "/home/nini/esp-claw-master (1)/esp-claw-master/application/edge_agent/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/nini/esp-claw-master (1)/esp-claw-master/application/edge_agent/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/nini/esp-claw-master (1)/esp-claw-master/application/edge_agent/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
