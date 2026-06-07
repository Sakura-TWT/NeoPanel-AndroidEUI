if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "INPUT, OUTPUT and SYMBOL are required")
endif()

string(REPLACE "\"" "" INPUT "${INPUT}")
string(REPLACE "\"" "" OUTPUT "${OUTPUT}")

file(READ "${INPUT}" HEX_BYTES HEX)
string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," BYTE_LIST "${HEX_BYTES}")
string(REGEX REPLACE "((0x[0-9a-fA-F][0-9a-fA-F],){16})" "\\1\n" BYTE_LIST "${BYTE_LIST}")

file(WRITE "${OUTPUT}" "#include <cstddef>\n#include <cstdint>\n\n")
file(APPEND "${OUTPUT}" "namespace neopanel::assets {\n")
file(APPEND "${OUTPUT}" "extern const std::uint8_t ${SYMBOL}[] = {\n${BYTE_LIST}\n};\n")
file(APPEND "${OUTPUT}" "extern const std::size_t ${SYMBOL}Size = sizeof(${SYMBOL});\n")
file(APPEND "${OUTPUT}" "} // namespace neopanel::assets\n")
