cmake_minimum_required(VERSION 4.2)
project(ReloadMakerV2 LANGUAGES NONE)
set(NIFSKOPE_SOURCE_DIR "F:/fo4dev/devtools/nifskope-upgraded/nifskope-cli" CACHE PATH "Read-only native engine source")
set(QT_BIN "C:/msys64/ucrt64/bin" CACHE PATH "Installed Qt/MinGW kit")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(STAGED "${CMAKE_BINARY_DIR}/native-engine")
set(DIST "${CMAKE_BINARY_DIR}/dist/ReloadMakerV2")
set(RELOADMAKER_BACKEND_BUNDLE "" CACHE PATH "Locally staged portable native/Python backend bundle")
file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/local/tmp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/prepare_build.py" "${NIFSKOPE_SOURCE_DIR}" "${STAGED}" "${CMAKE_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
add_custom_target(ReloadMakerV2 ALL
  COMMAND "${CMAKE_COMMAND}" -E env "PATH=${QT_BIN}\;C:/msys64/usr/bin\;$ENV{PATH}" "TEMP=${CMAKE_SOURCE_DIR}/local/tmp" "TMP=${CMAKE_SOURCE_DIR}/local/tmp" "${QT_BIN}/qmake6.exe" "${STAGED}/NifSkope.pro" "CONFIG+=release"
  COMMAND "${CMAKE_COMMAND}" -E env "PATH=${QT_BIN}\;C:/msys64/usr/bin\;$ENV{PATH}" "TEMP=${CMAKE_SOURCE_DIR}/local/tmp" "TMP=${CMAKE_SOURCE_DIR}/local/tmp" "${QT_BIN}/mingw32-make.exe" -s -j2
  COMMAND "${CMAKE_COMMAND}" -DSTAGE_SOURCE=${STAGED}/release -DSTAGE_DESTINATION=${DIST} -P "${CMAKE_SOURCE_DIR}/cmake/StageDistribution.cmake"
  WORKING_DIRECTORY "${STAGED}" VERBATIM)
if(RELOADMAKER_BACKEND_BUNDLE)
  if(NOT EXISTS "${RELOADMAKER_BACKEND_BUNDLE}/dependency-manifest.json")
    message(FATAL_ERROR "Stage the validated backend bundle before building")
  endif()
  add_custom_command(TARGET ReloadMakerV2 POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -DSTAGE_SOURCE=${RELOADMAKER_BACKEND_BUNDLE} -DSTAGE_DESTINATION=${DIST} -P "${CMAKE_SOURCE_DIR}/cmake/StageDistribution.cmake"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/backends/adapters" "${DIST}/backends/adapters"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/backends/provenance.json" "${DIST}/backends/provenance.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/mcp" "${DIST}/mcp"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/skills" "${DIST}/skills"
    VERBATIM)
  file(GLOB product_tools "${CMAKE_SOURCE_DIR}/tools/product_*.py")
  add_custom_command(TARGET ReloadMakerV2 POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${product_tools} "${DIST}/tools/"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/RefreshDependencyManifest.py" "${DIST}" VERBATIM)
endif()
