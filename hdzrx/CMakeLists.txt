idf_component_register(SRCS "phy.c" "codec.c" "fft.c" INCLUDE_DIRS ".")
# Plain complex multiplies: without this every one goes through libgcc's NaN-checking __mulsc3.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fcx-limited-range)
