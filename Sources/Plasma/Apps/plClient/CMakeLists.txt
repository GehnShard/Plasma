set(external_SCRIPTS
    external/create_resource_dat.py
    external/makeres.py
    external/render_svg.py
    external/scalergba.py
)

set(external_SOURCES
    external/Cursor_Base.svg
    external/Linking_Book.svg
    external/Loading_Text_rasterfont.svg
    external/Voice_Chat.svg
)
find_program(
    PNGCRUSH_EXECUTABLE pngcrush
    DOC "Path to pngcrush"
)

# Can we generate the resource.dat file?
python_test_modules(
    MODULES cairosvg PIL
    REQUIREMENTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/external/requirements.txt"
)

cmake_dependent_option(PLASMA_BUILD_RESOURCE_DAT "Do we want to build resource.dat?" ON "cairosvg_FOUND AND PIL_FOUND" OFF)
cmake_dependent_option(RESOURCE_OPTIMIZE "Optimize the images in resource.dat" ON "PLASMA_BUILD_RESOURCE_DAT AND PNGCRUSH_EXECUTABLE" OFF)
cmake_dependent_option(RESOURCE_BRUTE "Allow pngcrush brute-force optimization" OFF "PLASMA_BUILD_RESOURCE_DAT AND PNGCRUSH_EXECUTABLE" OFF)

if(PLASMA_BUILD_RESOURCE_DAT)
    set(external_DAT "${CMAKE_CURRENT_BINARY_DIR}/resource.dat")
    if(RESOURCE_OPTIMIZE)
        string(APPEND OPTIMIZE_ARGUMENT "--pngcrush \"${PNGCRUSH_EXECUTABLE}\" ")
    endif()
    if(RESOURCE_BRUTE)
        string(APPEND OPTIMIZE_ARGUMENT "--brute ")
    endif()

    add_custom_command(
        OUTPUT ${external_DAT}
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/external/makeres.py"
                --render --package
                ${OPTIMIZE_ARGUMENT}
                -i "${CMAKE_CURRENT_SOURCE_DIR}/external"
                -w "${CMAKE_CURRENT_BINARY_DIR}"
                -o "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS ${external_SCRIPTS} ${external_SOURCES}
    )

    install(
        FILES "${external_DAT}"
        DESTINATION client
    )
endif()

set(plClient_HEADERS
    plClient.h
    plClientCreatable.h
    plClientLoader.h
    plClientUpdateFormat.h
)

set(plClient_SOURCES
    pfAllCreatables.cpp
    plAllCreatables.cpp
    plClient.cpp
    plClientLoader.cpp
    pnAllCreatables.cpp
    winmain.cpp
)

set(plClient_TEXT
    ${Plasma_SOURCE_DIR}/Docs/ReleaseNotes/ReleaseNotes.txt
)

set(plClient_RESOURCES
    res/plClient.rc
    res/resource.h
    res/banner.bmp
    res/gehn.ico
)

plasma_executable(plClient WIN32 CLIENT
    SOURCES
        ${plClient_SOURCES} ${plClient_HEADERS}
        ${plClient_TEXT} ${plClient_RESOURCES}
)

if(PLASMA_BUILD_RESOURCE_DAT)
    target_sources(plClient PRIVATE ${external_SCRIPTS} ${external_SOURCES} ${external_DAT})
endif()

target_link_libraries(
    plClient
    PRIVATE
        CoreLib

        # For the "all creatables"
        pnNucleusInc
        plPubUtilInc
        pfFeatureInc

        # Everything else used in this target.
        pnDispatch
        pnFactory
        pnKeyedObject
        pnMessage
        pnSceneObject
        plAgeLoader
        plAnimation
        plAudio
        plAvatar
        plClientResMgr
        plDrawable
        plFile
        plGImage
        plGLight
        plInputCore
        plMessage
        plModifier
        plNetClient
        plNetCommon
        plNetGameLib
        plPhysX
        plPipeline
        plProgressMgr
        plResMgr
        plScene
        plSDL
        plStatGather
        plStatusLog
        plUnifiedTime
        pfAnimation
        pfAudio
        pfCharacter
        pfConsole
        pfConsoleCore
        pfCrashHandler
        pfGameGUIMgr
        pfJournalBook
        pfLocalizationMgr
        pfMoviePlayer
        pfPasswordStore
        pfPatcher
        pfPython
        CURL::libcurl
)

if(PLASMA_EXTERNAL_RELEASE)
    set_target_properties(plClient PROPERTIES OUTPUT_NAME "UruExplorer")
endif(PLASMA_EXTERNAL_RELEASE)

install(
    FILES $<TARGET_PDB_FILE:plClient>
    DESTINATION client
    OPTIONAL
)

if(PLASMA_BUILD_RESOURCE_DAT)
    source_group("Client Resource Scripts" FILES ${external_SCRIPTS})
    source_group("Client Resource Images" FILES ${external_SOURCES})
endif()
source_group("Source Files" FILES ${plClient_SOURCES})
source_group("Header Files" FILES ${plClient_HEADERS})
source_group("Text" FILES ${plClient_TEXT})
source_group("Windows Resource Files" FILES ${plClient_RESOURCES})
