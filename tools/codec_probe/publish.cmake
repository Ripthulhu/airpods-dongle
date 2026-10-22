# SPDX-License-Identifier: GPL-3.0-only
# Keep SDK binary/map/disassembly generation, but publish UF2 only through the gate.
function(airpods_add_checked_outputs target)
    set(uf2 "${CMAKE_CURRENT_BINARY_DIR}/${target}.uf2")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${ARGN}
        COMMENT "Checking linked codec memory and PCM quality before publishing firmware"
        VERBATIM)
    set(PICO_NO_UF2 1)
    pico_add_extra_outputs(${target})
    # Make does not rebuild a missing POST_BUILD byproduct. This real output rule
    # recovers it; Ninja can reuse the receipt immediately after a successful link.
    add_custom_command(OUTPUT "${uf2}"
        COMMAND ${ARGN} --if-current
        DEPENDS ${target}
        VERBATIM)
    add_custom_target(${target}_uf2 ALL DEPENDS "${uf2}")
endfunction()
