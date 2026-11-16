# render/ — the pure drawing layer (M7): a display-independent DrawList
# built from a sim snapshot, camera and view options, a software rasterizer
# and a PNG encoder (stb_image_write). No raylib here; sim/ gets no new
# dependencies (it is only read).
add_library(render STATIC
  src/battle_view.cpp
  src/raster.cpp
  src/png_writer.cpp
  src/ui/ui.cpp
  src/ui/demo.cpp
  src/ui/screens.cpp
  src/ui/shell.cpp)
add_library(render::render ALIAS render)
target_include_directories(render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(render PUBLIC cxx_std_20)
target_link_libraries(render PUBLIC sim game_core stb_headers nlohmann_json::nlohmann_json)
