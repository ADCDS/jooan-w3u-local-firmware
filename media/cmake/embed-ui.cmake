# SPDX-License-Identifier: MIT
# Turn media/ui/*.{html,css,js,svg} into a C table the HTTP server serves
# from memory. Deterministic output (sorted names, no timestamps or paths).
# Usage: cmake -DUI_DIR=<dir> -DOUT=<header> -P embed-ui.cmake
file(GLOB names RELATIVE "${UI_DIR}" "${UI_DIR}/*.html" "${UI_DIR}/*.css" "${UI_DIR}/*.js" "${UI_DIR}/*.svg")
list(SORT names)
set(arrays "")
set(table "")
set(index 0)
foreach(name IN LISTS names)
  if(NOT name MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$")
    message(FATAL_ERROR "UI file name not allowed: ${name}")
  endif()
  file(READ "${UI_DIR}/${name}" hex HEX)
  string(LENGTH "${hex}" digits)
  math(EXPR size "${digits} / 2")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  string(APPEND arrays "static const unsigned char ui_asset_${index}[${size}] = {${bytes}};\n")
  if(name MATCHES "\\.html$")
    set(type "text/html; charset=utf-8")
  elseif(name MATCHES "\\.css$")
    set(type "text/css; charset=utf-8")
  elseif(name MATCHES "\\.js$")
    set(type "text/javascript; charset=utf-8")
  else()
    set(type "image/svg+xml")
  endif()
  string(APPEND table "    {\"${name}\", \"${type}\", ui_asset_${index}, ${size}},\n")
  math(EXPR index "${index} + 1")
endforeach()
file(WRITE "${OUT}.tmp" "/* Generated from media/ui by cmake/embed-ui.cmake; do not edit. */\n"
  "${arrays}"
  "typedef struct { const char *name, *type; const unsigned char *data; size_t size; } ui_asset;\n"
  "static const ui_asset ui_assets[] = {\n${table}    {0, 0, 0, 0}\n};\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
