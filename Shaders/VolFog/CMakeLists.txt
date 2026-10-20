# Not in the original client (World of Legends): HLSL shaders of the native volumetric fog (wolVolFog), brief B2.
# Chain (design 2026-10-19-fase3-niebla-volumetrica-design.md, section 3): fxc -> Tools/VolFog/mkbls.py (GXSH .bls) ->
# Tools/VolFog/mkmpq.py -> bin/Win/Data/patch-Z.MPQ. Only the target VolFogShaders is defined here; WowClient depends on it.

set(VOLFOG_OUT ${CMAKE_BINARY_DIR}/volfog)
set(VOLFOG_STAGE ${VOLFOG_OUT}/stage)
set(VOLFOG_MPQ ${CMAKE_SOURCE_DIR}/bin/Win/Data/patch-Z.MPQ)

# fxc.exe: newest Windows SDK that has it (override with -DVOLFOG_FXC=<path>).
set(VOLFOG_FXC "" CACHE FILEPATH "HLSL compiler (fxc.exe) used for the volumetric fog shaders")
if (NOT VOLFOG_FXC OR NOT EXISTS "${VOLFOG_FXC}")
    file(GLOB VOLFOG_FXC_CANDIDATES "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x64/fxc.exe"
                                    "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
    list(REMOVE_DUPLICATES VOLFOG_FXC_CANDIDATES)
    list(SORT VOLFOG_FXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
    if (VOLFOG_FXC_CANDIDATES)
        list(GET VOLFOG_FXC_CANDIDATES 0 VOLFOG_FXC_FOUND)
        set(VOLFOG_FXC "${VOLFOG_FXC_FOUND}" CACHE FILEPATH "HLSL compiler (fxc.exe) used for the volumetric fog shaders" FORCE)
    endif ()
endif ()
if (NOT VOLFOG_FXC OR NOT EXISTS "${VOLFOG_FXC}")
    message(FATAL_ERROR
        "WOWDECOMP_VOLFOG: fxc.exe not found. Install the Windows 10/11 SDK (it provides "
        "'C:/Program Files (x86)/Windows Kits/10/bin/<version>/x64/fxc.exe'), pass -DVOLFOG_FXC=<path to fxc.exe>, "
        "or configure with -DWOWDECOMP_VOLFOG=OFF to skip the volumetric fog shaders.")
endif ()

# Python 3 (standard library only) runs mkbls.py and mkmpq.py (override with -DVOLFOG_PYTHON=<python.exe>).
set(VOLFOG_PYTHON "" CACHE FILEPATH "Python 3 interpreter used by Tools/VolFog")
if (NOT VOLFOG_PYTHON OR NOT EXISTS "${VOLFOG_PYTHON}")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if (Python3_Interpreter_FOUND)
        set(VOLFOG_PYTHON "${Python3_EXECUTABLE}" CACHE FILEPATH "Python 3 interpreter used by Tools/VolFog" FORCE)
    endif ()
endif ()
if (NOT VOLFOG_PYTHON OR NOT EXISTS "${VOLFOG_PYTHON}")
    message(FATAL_ERROR
        "WOWDECOMP_VOLFOG: Python 3 not found (needed by Tools/VolFog/mkbls.py and mkmpq.py). Install it, pass "
        "-DVOLFOG_PYTHON=<path to python.exe>, or configure with -DWOWDECOMP_VOLFOG=OFF.")
endif ()

file(GLOB VOLFOG_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hlsli")
set(VOLFOG_MKBLS ${CMAKE_SOURCE_DIR}/Tools/VolFog/mkbls.py)
set(VOLFOG_MKMPQ ${CMAKE_SOURCE_DIR}/Tools/VolFog/mkmpq.py)
set(VOLFOG_BLS_FILES "")

# volfog_shader(<Name> <ps|vs> <source.hlsl> [PERMUTATIONS <defs> ...])
# One fxc run per permutation: each <defs> is a comma separated list of preprocessor definitions (e.g.
# "STEPS=16,LOCAL_LIGHTS=0"); without PERMUTATIONS there is one permutation with no definitions. The .bls holds the
# permutations in the order given and is staged as Shaders/Pixel/ps_3_0/<Name>.bls or Shaders/Vertex/vs_3_0/<Name>.bls.
function(volfog_shader NAME KIND SOURCE)
    cmake_parse_arguments(ARG "" "" "PERMUTATIONS" ${ARGN})
    if (KIND STREQUAL "ps")
        set(PROFILE ps_3_0)
        set(BLS_DIR Shaders/Pixel/ps_3_0)
    elseif (KIND STREQUAL "vs")
        set(PROFILE vs_3_0)
        set(BLS_DIR Shaders/Vertex/vs_3_0)
    else ()
        message(FATAL_ERROR "volfog_shader(${NAME}): kind must be ps or vs")
    endif ()
    if (NOT ARG_PERMUTATIONS)
        set(ARG_PERMUTATIONS "-")
    endif ()
    list(LENGTH ARG_PERMUTATIONS PERM_COUNT)

    set(PSOS "")
    set(INDEX 0)
    foreach (PERM IN LISTS ARG_PERMUTATIONS)
        set(DEFINE_ARGS "")
        if (NOT PERM STREQUAL "-")
            string(REPLACE "," ";" DEFINES "${PERM}")
            foreach (DEFINE IN LISTS DEFINES)
                list(APPEND DEFINE_ARGS /D ${DEFINE})
            endforeach ()
        endif ()
        set(PSO ${VOLFOG_OUT}/${NAME}_${INDEX}.pso)
        set(ASM ${VOLFOG_OUT}/${NAME}_${INDEX}.asm)
        add_custom_command(
            OUTPUT ${PSO} ${ASM}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${VOLFOG_OUT}
            COMMAND "${VOLFOG_FXC}" /nologo /O3 /T ${PROFILE} /E main ${DEFINE_ARGS} /Fo ${PSO} /Fc ${ASM} ${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE} ${VOLFOG_HEADERS}
            COMMENT "fxc ${PROFILE} ${NAME} [${INDEX}] ${PERM}"
            VERBATIM)
        list(APPEND PSOS ${PSO})
        math(EXPR INDEX "${INDEX} + 1")
    endforeach ()

    set(BLS ${VOLFOG_STAGE}/${BLS_DIR}/${NAME}.bls)
    add_custom_command(
        OUTPUT ${BLS}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${VOLFOG_STAGE}/${BLS_DIR}
        COMMAND "${VOLFOG_PYTHON}" -I ${VOLFOG_MKBLS} ${BLS} ${PSOS}
        DEPENDS ${PSOS} ${VOLFOG_MKBLS}
        COMMENT "mkbls ${NAME}.bls (${PERM_COUNT} permutation(s))"
        VERBATIM)
    set(VOLFOG_BLS_FILES ${VOLFOG_BLS_FILES} ${BLS} PARENT_SCOPE)
endfunction ()

# B2: quad vertex shader (vf_fullscreen_vs.hlsl as it is) and the debug pixel shader (wolVolFogDebug 1..3).
# The remaining vf_*.hlsl are compiled from brief B7 on (design, section 3).
volfog_shader(WolVolFogQuad vs vf_fullscreen_vs.hlsl)
volfog_shader(WolVolFogDebug ps WolVolFogDebug.hlsl PERMUTATIONS "DEBUG_MODE=1" "DEBUG_MODE=2" "DEBUG_MODE=3" "DEBUG_MODE=4")

# B6: ray march and composite. Six permutations each, index = (quality - 1) * 2 + localLights: STEPS 16 / 24 / 32 (wolVolFog 1 / 2 / 3)
# times LOCAL_LIGHTS 0 / 1 (the macros of vf_integrate.hlsli). B6 draws only the LOCAL_LIGHTS=0 ones; the others are compiled for B8.
# SAMPLES_AUTHORED_NOISE keeps its default (0): the authored noise of the Classic layers is not used (design, section 2).
volfog_shader(WolVolFogMarch ps vf_march.hlsl PERMUTATIONS
    "STEPS=16,LOCAL_LIGHTS=0" "STEPS=16,LOCAL_LIGHTS=1"
    "STEPS=24,LOCAL_LIGHTS=0" "STEPS=24,LOCAL_LIGHTS=1"
    "STEPS=32,LOCAL_LIGHTS=0" "STEPS=32,LOCAL_LIGHTS=1")
volfog_shader(WolVolFogComposite ps vf_composite.hlsl PERMUTATIONS
    "STEPS=16,LOCAL_LIGHTS=0" "STEPS=16,LOCAL_LIGHTS=1"
    "STEPS=24,LOCAL_LIGHTS=0" "STEPS=24,LOCAL_LIGHTS=1"
    "STEPS=32,LOCAL_LIGHTS=0" "STEPS=32,LOCAL_LIGHTS=1")

# B7: temporal filter and the packed linear depth of the previous frame (one permutation each). vf_temporal.hlsl and vf_history_depth.hlsl
# are compiled as they are in coa-vfog; the history depth is written as RGB = (high byte, low byte, depth class) of the linear depth, so it
# needs an 8 bit target (ARGB8), not R32F.
volfog_shader(WolVolFogTemporal ps vf_temporal.hlsl)
volfog_shader(WolVolFogHistoryDepth ps vf_history_depth.hlsl)

# B9: light shafts (god rays). Mask of the bright sky around the sun or moon, radial blur (two passes of the same shader) and the final blend over
# the fogged image, one permutation each. vf_ray_mask.hlsl, vf_ray_blur.hlsl and vf_ray_composite.hlsl are compiled as they are in coa-vfog.
volfog_shader(WolVolFogRayMask ps vf_ray_mask.hlsl)
volfog_shader(WolVolFogRayBlur ps vf_ray_blur.hlsl)
volfog_shader(WolVolFogRayComposite ps vf_ray_composite.hlsl)

add_custom_command(
    OUTPUT ${VOLFOG_MPQ}
    COMMAND "${VOLFOG_PYTHON}" -I ${VOLFOG_MKMPQ} ${VOLFOG_MPQ} ${VOLFOG_STAGE} ${VOLFOG_BLS_FILES}
    DEPENDS ${VOLFOG_BLS_FILES} ${VOLFOG_MKMPQ}
    COMMENT "mkmpq patch-Z.MPQ"
    VERBATIM)

add_custom_target(VolFogShaders ALL DEPENDS ${VOLFOG_MPQ})
