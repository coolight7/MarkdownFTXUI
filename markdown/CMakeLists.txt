add_library(markdown-ui
    src/editor.cpp
    src/viewer.cpp
    src/parser_cmark.cpp
    src/dom_builder.cpp
    src/highlight.cpp
    src/incremental.cpp
    src/state_diagram.cpp
)

target_include_directories(markdown-ui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_definitions(markdown-ui PUBLIC CMARK_GFM_STATIC_DEFINE)

find_package(ftxui REQUIRED)

target_link_libraries(markdown-ui
    PUBLIC
        ftxui::screen
        ftxui::dom
        ftxui::component
)
if(WIN32)
    target_link_libraries(markdown-ui
        PUBLIC
            # 静态库链接顺序: extensions 依赖 cmark-gfm 核心符号, 必须在前,
            # 否则链接器扫描完 gfm 后无法回扫解析 extensions 的未定义符号。
            cmark-gfm-extensions_static
            cmark-gfm_static
    )
else()
    target_link_libraries(markdown-ui
        PUBLIC
            # 静态库链接顺序: extensions 依赖 cmark-gfm 核心符号, 必须在前,
            # 否则链接器扫描完 gfm 后无法回扫解析 extensions 的未定义符号。
            cmark-gfm-extensions
            cmark-gfm
    )
endif()

# --- Install targets ---
include(GNUInstallDirs)

install(TARGETS markdown-ui
    EXPORT markdown-ui-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/markdown
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT markdown-ui-targets
    FILE markdown-ui-config.cmake
    NAMESPACE markdown::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/markdown-ui
)
