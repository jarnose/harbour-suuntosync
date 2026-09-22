cmake_minimum_required(VERSION 3.19)
project(harbour-suuntosync CXX C)

# NOTE: no "Bluetooth" component - Sailfish OS 5.1's SDK target has no
# pkgconfig(Qt5Bluetooth) at all (confirmed by a real build attempt: zypper
# found no provider, by name or capability). QtConnectivity/QtBluetooth isn't
# part of the Sailfish OS Qt5 distribution; BLE work here goes through BlueZ
# 5's own D-Bus API (org.bluez.*) via Qt5::DBus instead (added below, Phase 5) -
# see ~/.claude/plans/agile-hopping-harp.md.
find_package(Qt5 COMPONENTS Core Qml Gui Quick Sql Concurrent Network DBus LinguistTools REQUIRED)

include(FindPkgConfig)
pkg_search_module(SAILFISH sailfishapp REQUIRED)
pkg_search_module(SAILFISHSECRETS sailfishsecrets REQUIRED)

set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Sources are added back in as their own phases land (see
# ~/.claude/plans/agile-hopping-harp.md) so each build stays small and
# attributable to one phase if it breaks. Phase 2a: TokenVault (Sailfish
# Secrets) + CloudAccountStore (SQLite metadata), no network yet. Phase 5:
# BlueZ D-Bus device discovery/pairing. Phase 6: mdswirecodec (already
# separately unit-tested via tests/test_mdswirecodec.cpp, plain g++) +
# mdswhiteboardclient wiring it up to real GATT characteristics. Phase 6
# continued: sbemcontainer (already separately unit-tested via
# tests/test_sbemcontainer.cpp, plain g++) decodes the actual
# /Logbook/byId/<id>/Data payload once reassembled - Heatshrink
# decompression (vendored, ISC-licensed, src/ble/heatshrink/) followed by
# an SBEM0103 TLV walk. See docs/logbook-data-format.md. Phase 6 continued
# again: logbookdecoder (tests/test_logbookdecoder.cpp, plain g++) turns
# the decoded chunks into workout summary fields - GPS/HR/activity/cadence
# confirmed against Jarno's own FIT exports, plus altitude and derived
# ascent/descent (found once the APK yielded the watch's own field map -
# see the doc). Phase 6 continued
# once more: MdsWhiteboardClient::fetchLogbookData() and
# AppController::testLogbookFetch() wire logbookdecoder up to a real BLE
# fetch - marked experimental throughout (see fetchLogbookData()'s and
# Mds::encodeStreamStartTrigger()'s doc comments) since it replaces the
# real captured trigger sequence's elaborate handle-walk with a much
# simpler hypothesis, since confirmed on real hardware, and wired into
# WorkoutStore. Phase 6
# continued yet again: logentriesdecoder (tests/test_logentriesdecoder.cpp,
# plain g++) + Mds::encodeEntriesFetchTrigger() +
# MdsWhiteboardClient::fetchLogEntries() - a *different* shortcut for
# /Entries specifically (a TYPE=0x0D handle-fetch, not a TYPE=0x10 stream
# trigger), re-derived from decompiling libmds.so's own protocol_v9
# structure-deserializer code after the /Data-style shortcut was confirmed
# not to work for /Entries on real hardware. See
# docs/logbook-data-format.md's "Third decompilation pass" section;
# confirmed working on real hardware. And finally summarydecoder
# (tests/test_summarydecoder.cpp) + Mds::encodePagedReadRequest() +
# MdsWhiteboardClient::fetchSummary(): /Summary is a third transport again,
# fixed-size pages rather than a stream, carrying the watch's own exact
# totals (ascent, descent, energy, moving time) that /Data can only
# approximate.
add_executable(harbour-suuntosync
    src/harbour-suuntosync.cpp
    src/controller/appcontroller.cpp
    src/secrets/tokenvault.cpp
    src/cloud/cloudaccountstore.cpp
    src/ble/bluezmanagedobjects.cpp
    src/ble/bluezadapter.cpp
    src/ble/pairedwatchstore.cpp
    src/ble/devicelistmodel.cpp
    src/ble/mdswirecodec.cpp
    src/ble/mdswhiteboardclient.cpp
    src/ble/sbemcontainer.cpp
    src/ble/logbookdecoder.cpp
    src/ble/logentriesdecoder.cpp
    src/ble/summarydecoder.cpp
    src/ble/heatshrink/heatshrink_decoder.c
    src/cloud/suuntoauth.cpp
    src/cloud/suuntocloudclient.cpp
    src/store/workoutstore.cpp
    src/model/workoutlistmodel.cpp
)

target_compile_definitions(harbour-suuntosync PRIVATE
    $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:QT_QML_DEBUG>
)

target_include_directories(harbour-suuntosync PRIVATE
    $<BUILD_INTERFACE:
    ${SAILFISH_INCLUDE_DIRS}
    ${SAILFISHSECRETS_INCLUDE_DIRS}
>)

target_link_libraries(harbour-suuntosync
    Qt5::Quick
    Qt5::Sql
    Qt5::Concurrent
    Qt5::Network
    Qt5::DBus
    ${SAILFISH_LDFLAGS}
    ${SAILFISHSECRETS_LDFLAGS}
)

install(TARGETS harbour-suuntosync
    RUNTIME DESTINATION bin
)
install(DIRECTORY qml
    DESTINATION share/harbour-suuntosync
)
install(FILES harbour-suuntosync.desktop
    DESTINATION share/applications
)
install(FILES icons/86x86/harbour-suuntosync.png
    DESTINATION share/icons/hicolor/86x86/apps
)
install(FILES icons/108x108/harbour-suuntosync.png
    DESTINATION share/icons/hicolor/108x108/apps
)
install(FILES icons/128x128/harbour-suuntosync.png
    DESTINATION share/icons/hicolor/128x128/apps
)
install(FILES icons/172x172/harbour-suuntosync.png
    DESTINATION share/icons/hicolor/172x172/apps
)

# Source strings (qsTr()/tr()/QCoreApplication::translate()) are English;
# translations/harbour-suuntosync.ts tracks them as the extraction base (kept
# up to date by manually running lupdate, not as part of this build) and each
# translations/harbour-suuntosync-<lang>.ts is a translated copy of it.
# qt5_add_translation only compiles .ts -> .qm (lrelease) - it does not run
# lupdate - so a normal build never touches the committed .ts files.
file(GLOB TS_FILES "${CMAKE_SOURCE_DIR}/translations/*.ts")
file(GLOB LANG_TS_FILES "${CMAKE_SOURCE_DIR}/translations/harbour-suuntosync-*.ts")
qt5_add_translation(QM_FILES ${LANG_TS_FILES})
add_custom_target(translations ALL DEPENDS ${QM_FILES})
add_dependencies(harbour-suuntosync translations)

install(FILES ${QM_FILES}
    DESTINATION share/harbour-suuntosync/translations
)

# Get the other files reachable from the project tree in Qt Creator
add_custom_target(distfiles
    SOURCES
        harbour-suuntosync.desktop
        qml/harbour-suuntosync.qml
        qml/cover/CoverPage.qml
        qml/pages/MainPage.qml
        qml/pages/ActivityTypes.js
        qml/pages/PairingPage.qml
        qml/pages/LoginPage.qml
        qml/pages/WorkoutDetailPage.qml
        rpm/harbour-suuntosync.spec
        ${TS_FILES})

# Tell Qt Creator where the application executable(s) would be located on the device.
file(WRITE "${CMAKE_BINARY_DIR}/QtCreatorDeployment.txt"
    "${CMAKE_INSTALL_PREFIX}
${CMAKE_BINARY_DIR}/harbour-suuntosync:bin
")
