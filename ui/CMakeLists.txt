# The panel UI as an ESP-IDF component; ui/sim builds the same sources for the browser.
idf_component_register(SRCS "src/ui.c" "src/canvas.c" "fonts/inter.c" "fonts/icons.c" "fonts/pause.c" INCLUDE_DIRS "src" "fonts")
