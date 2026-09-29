cmake_minimum_required(VERSION 4.2)
project(ReloadMakerV2 LANGUAGES NONE)
set(NIFSKOPE_SOURCE_DIR "F:/fo4dev/devtools/nifskope-upgraded/nifskope-cli" CACHE PATH "Read-only native engine source")
set(QT_BIN "C:/msys64/ucrt64/bin" CACHE PATH "Installed Qt/MinGW kit")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(STAGED "${CMAKE_BINARY_DIR}/native-engine")
set(DIST "${CMAKE_BINARY_DIR}/dist/ReloadMakerV2")
file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/local/tmp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/prepare_build.py" "${NIFSKOPE_SOURCE_DIR}" "${STAGED}" "${CMAKE_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
add_custom_target(ReloadMakerV2 ALL
  COMMAND "${CMAKE_COMMAND}" -E env "PATH=${QT_BIN}\;C:/msys64/usr/bin\;$ENV{PATH}" "TEMP=${CMAKE_SOURCE_DIR}/local/tmp" "TMP=${CMAKE_SOURCE_DIR}/local/tmp" "${QT_BIN}/qmake6.exe" "${STAGED}/NifSkope.pro" "CONFIG+=release"
  COMMAND "${CMAKE_COMMAND}" -E env "PATH=${QT_BIN}\;C:/msys64/usr/bin\;$ENV{PATH}" "TEMP=${CMAKE_SOURCE_DIR}/local/tmp" "TMP=${CMAKE_SOURCE_DIR}/local/tmp" "${QT_BIN}/mingw32-make.exe" -s -j2
  COMMAND "${CMAKE_COMMAND}" -DSTAGE_SOURCE=${STAGED}/release -DSTAGE_DESTINATION=${DIST} -P "${CMAKE_SOURCE_DIR}/cmake/StageDistribution.cmake"
  WORKING_DIRECTORY "${STAGED}" VERBATIM)
