cmake_minimum_required(VERSION 3.20)
project(DSGemuEngine VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD   99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED   ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =======================================================
#  チップコア
#  FmEngineApi に依存しないので単体でも組み込める
# =======================================================
set(CORE_SOURCES
    core/ym2163.c
)

set(ENGINE_SOURCES
    src/DSGemuEngine.cpp
)

# =======================================================
#  共有ライブラリ
#  出力ファイル名: DSGemuEngine.dll / libDSGemuEngine.so / libDSGemuEngine.dylib
#  FMEngineTest から使う場合は -e オプションでパスを指定する。
# =======================================================
add_library(DSGemuEngine SHARED ${ENGINE_SOURCES} ${CORE_SOURCES})

target_include_directories(DSGemuEngine PRIVATE
    src         # FmEngineApi.h
    core        # ym2163.h
)

target_compile_definitions(DSGemuEngine PRIVATE FMENGINE_EXPORTS)

# -------------------------------------------------------
#  チップコア単体のスタティックライブラリ
#  他プロジェクトへ組み込む場合はこちらを使う
# -------------------------------------------------------
add_library(ym2163 STATIC ${CORE_SOURCES})
target_include_directories(ym2163 PUBLIC core)

# -------------------------------------------------------
#  プラットフォーム別設定
# -------------------------------------------------------
if(MSVC)
    target_compile_options(DSGemuEngine PRIVATE /W3 /utf-8)
    target_compile_options(ym2163       PRIVATE /W3 /utf-8)
    set_target_properties(DSGemuEngine PROPERTIES
        OUTPUT_NAME              "DSGemuEngine"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        PDB_OUTPUT_DIRECTORY     "${CMAKE_BINARY_DIR}/bin"
    )
else()
    target_compile_options(DSGemuEngine PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fvisibility=hidden   # 非公開シンボルを隠す
    )
    target_compile_options(ym2163 PRIVATE -Wall -Wextra)
    set_target_properties(ym2163 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    find_package(Threads REQUIRED)
    target_link_libraries(DSGemuEngine PRIVATE Threads::Threads)
    set_target_properties(DSGemuEngine PROPERTIES
        OUTPUT_NAME              "DSGemuEngine"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
endif()

# -------------------------------------------------------
#  インストール (任意)
# -------------------------------------------------------
install(TARGETS DSGemuEngine ym2163
    RUNTIME DESTINATION bin   # Windows DLL
    LIBRARY DESTINATION lib   # Linux/macOS .so/.dylib
    ARCHIVE DESTINATION lib
)
install(FILES src/FmEngineApi.h core/ym2163.h DESTINATION include)
