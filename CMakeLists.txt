cmake_minimum_required(VERSION 4.2)
project(ReloadMakerV2 LANGUAGES NONE)
set(NIFSKOPE_SOURCE_DIR "F:/fo4dev/devtools/nifskope-upgraded/nifskope-cli" CACHE PATH "Read-only native engine source")
set(QT_BIN "C:/msys64/ucrt64/bin" CACHE PATH "Installed Qt/MinGW kit")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(STAGED "${CMAKE_BINARY_DIR}/native-engine")
set(DIST "${CMAKE_BINARY_DIR}/dist/ReloadMakerV2")
set(RELOADMAKER_BACKEND_BUNDLE "" CACHE PATH "Locally staged portable native/Python backend bundle")
set(RELOADMAKER_CONTENT_VALIDATOR "" CACHE FILEPATH "Validated DIAMOND content checker for portable export")
file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/local/tmp")
# The staged qmake project lists every application source, so adding or removing a
# source file, or changing the staging script, must re-run this configure step.
file(GLOB_RECURSE staged_application_files CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/prepare_build.py")
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
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/assets" "${DIST}/assets"
    VERBATIM)
  if(RELOADMAKER_CONTENT_VALIDATOR)
    add_custom_command(TARGET ReloadMakerV2 POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${DIST}/backends/diamond"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${RELOADMAKER_CONTENT_VALIDATOR}" "${DIST}/backends/diamond/DiamondProfileCheck.exe" VERBATIM)
  endif()
  file(GLOB product_tools CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/product_*.py")
  add_custom_command(TARGET ReloadMakerV2 POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${product_tools} "${DIST}/tools/"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/RefreshDependencyManifest.py" "${DIST}" VERBATIM)
endif()

# Behavioral suites. Each test builds its qmake suite before running it, so the
# tests share one compiler lock to stay within the two-process build cap.
enable_testing()
set(RELOADMAKER_GLOCK_PACKAGE "F:/fo4dev/personal_devtools/reload_maker/example_mods/glock" CACHE PATH
    "Read-only original Glock package used as an integration fixture")
set(simulation_fixtures "${CMAKE_SOURCE_DIR}/tests/fixtures/simulation")
set(reloadmaker_suites HkxReader Resources Pose SceneBinding Configuration Library Installation Authoring HandPose
    PartGrip GripViewport Mechanism ReloadRecipe Simulation Ammunition ActionCatalog Sound GrabLocations ScopeProfile
    Collision WeaponModel Checklist Detector)
foreach(suite IN LISTS reloadmaker_suites)
  set(suite_arguments "")
  if(suite STREQUAL "Simulation")
    set(suite_arguments "${simulation_fixtures}/glock-simulation.project.json"
        "${simulation_fixtures}/ru556-reload-recipe.json" "${simulation_fixtures}/m1a-reload-recipe.json")
  elseif(suite STREQUAL "Ammunition")
    set(suite_arguments "${RELOADMAKER_GLOCK_PACKAGE}/Glock19x.esp")
  elseif(suite STREQUAL "WeaponModel" OR suite STREQUAL "Checklist" OR suite STREQUAL "Detector")
    set(suite_arguments "${CMAKE_SOURCE_DIR}/assets/presets")
  endif()
  add_test(NAME ${suite}
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tools/Invoke-Tests.ps1"
            -Suite ${suite} -QtBin "${QT_BIN}" -NifSkopeSource "${NIFSKOPE_SOURCE_DIR}" ${suite_arguments}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(${suite} PROPERTIES RESOURCE_LOCK reloadmaker-compiler TIMEOUT 1800)
endforeach()
# The editor test harness never removes library work that existed before a run.
add_test(NAME EditorHarnessRestore
  COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/editor_session_restore_test.py")
# No weapon is recognized by name in application source (rework 14, item 26).
add_test(NAME WeaponNames
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/check_weapon_names.py" "${CMAKE_SOURCE_DIR}/src")
