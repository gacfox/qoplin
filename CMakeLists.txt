cmake_minimum_required(VERSION 3.21)

# Qoplin uses vcpkg explicitly. Do not ask Qt Creator to discover and bootstrap
# another instance through its package-manager auto-setup.
set(QT_CREATOR_SKIP_VCPKG_SETUP ON)
if(WIN32 AND NOT DEFINED VCPKG_INSTALLED_DIR)
    set(VCPKG_INSTALLED_DIR "${CMAKE_SOURCE_DIR}/build/vcpkg_installed"
        CACHE PATH "Shared Qoplin vcpkg installation")
endif()

# Presets provide the toolchain explicitly. This fallback also makes a plain
# Qt Creator MinGW kit work when it configures a fresh build directory.
if(WIN32 AND NOT CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(_qoplin_vcpkg_root "$ENV{VCPKG_ROOT}")
    elseif(EXISTS "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(_qoplin_vcpkg_root "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/vcpkg")
    endif()
    if(_qoplin_vcpkg_root AND EXISTS "${_qoplin_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "${_qoplin_vcpkg_root}/scripts/buildsystems/vcpkg.cmake"
            CACHE FILEPATH "vcpkg toolchain used by Qoplin")
        if(NOT DEFINED VCPKG_TARGET_TRIPLET)
            set(VCPKG_TARGET_TRIPLET "x64-mingw-static" CACHE STRING "Qoplin vcpkg target triplet")
        endif()
        if(NOT DEFINED VCPKG_HOST_TRIPLET)
            set(VCPKG_HOST_TRIPLET "x64-mingw-static" CACHE STRING "Qoplin vcpkg host triplet")
        endif()
        message(STATUS "Qoplin: using vcpkg at ${_qoplin_vcpkg_root}")
    endif()
endif()

project(Qoplin VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network Test)
find_package(unofficial-sodium CONFIG REQUIRED)

add_library(qoplin_core STATIC
    src/storage/AppSettings.cpp
    src/storage/AppSettings.h
    src/storage/NotebookStore.cpp
    src/storage/NotebookStore.h
    src/models/NoteNode.h
    src/models/NotebookInfo.h
    src/sync/CryptoService.cpp
    src/sync/CryptoService.h
    src/sync/RemoteCatalogWorker.cpp
    src/sync/RemoteCatalogWorker.h
    src/sync/RemoteImportWorker.cpp
    src/sync/RemoteImportWorker.h
    src/sync/WebDavClient.cpp
    src/sync/WebDavClient.h
    src/sync/SyncWorker.cpp
    src/sync/SyncWorker.h
)
target_include_directories(qoplin_core PUBLIC src)
target_link_libraries(qoplin_core PUBLIC Qt6::Widgets Qt6::Network unofficial-sodium::sodium)

qt_add_executable(qoplin WIN32
    resources/qoplin.qrc
    resources/windows/qoplin.rc
    src/app/main.cpp
    src/app/ApplicationController.cpp
    src/app/ApplicationController.h
    src/ui/StartWindow.cpp
    src/ui/StartWindow.h
    src/ui/StartWindow.ui
    src/ui/MainWindow.cpp
    src/ui/MainWindowSync.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.ui
    src/ui/NotebookDialog.cpp
    src/ui/NotebookDialog.h
    src/ui/NotebookDialog.ui
    src/ui/RemoteImportDialog.cpp
    src/ui/RemoteImportDialog.h
    src/ui/RemoteImportDialog.ui
    src/ui/SettingsDialog.cpp
    src/ui/SettingsDialog.h
    src/ui/SettingsDialog.ui
    src/ui/SyncDialog.cpp
    src/ui/SyncDialog.h
    src/ui/SyncDialog.ui
)
target_include_directories(qoplin PRIVATE src)
target_link_libraries(qoplin PRIVATE qoplin_core Qt6::Widgets Qt6::Network)
set_target_properties(qoplin PROPERTIES OUTPUT_NAME Qoplin)

if(MINGW)
    target_compile_options(qoplin_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(qoplin PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/StorageTest.cpp")
    qt_add_executable(storage_test tests/StorageTest.cpp)
    target_link_libraries(storage_test PRIVATE qoplin_core Qt6::Test)
    add_test(NAME storage_test COMMAND storage_test)
endif()
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/SyncTest.cpp")
    qt_add_executable(sync_test tests/SyncTest.cpp tests/SyncTest.h)
    target_link_libraries(sync_test PRIVATE qoplin_core Qt6::Test)
    add_test(NAME sync_test COMMAND sync_test)
endif()
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/RemoteImportTest.cpp")
    qt_add_executable(remote_import_test tests/RemoteImportTest.cpp)
    target_link_libraries(remote_import_test PRIVATE qoplin_core Qt6::Test)
    add_test(NAME remote_import_test COMMAND remote_import_test)
endif()
if(BUILD_TESTING)
    qt_add_executable(sync_integration_test tests/SyncIntegrationTest.cpp)
    target_link_libraries(sync_integration_test PRIVATE qoplin_core Qt6::Test)
    add_test(NAME sync_integration_test COMMAND sync_integration_test)

    qt_add_executable(ui_smoke_test
        resources/qoplin.qrc
        tests/UiSmokeTest.cpp
        src/ui/MainWindow.cpp
        src/ui/MainWindowSync.cpp
        src/ui/MainWindow.h
        src/ui/MainWindow.ui
        src/ui/RemoteImportDialog.cpp
        src/ui/RemoteImportDialog.h
        src/ui/RemoteImportDialog.ui
        src/ui/SettingsDialog.cpp
        src/ui/SettingsDialog.h
        src/ui/SettingsDialog.ui
        src/ui/SyncDialog.cpp
        src/ui/SyncDialog.h
        src/ui/SyncDialog.ui
    )
    target_include_directories(ui_smoke_test PRIVATE src)
    target_link_libraries(ui_smoke_test PRIVATE qoplin_core Qt6::Test Qt6::Widgets)
    add_test(NAME ui_smoke_test COMMAND ui_smoke_test)
    set_tests_properties(ui_smoke_test PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Gui>/../plugins/platforms")
endif()

install(TARGETS qoplin RUNTIME DESTINATION .)
