# Link-A-only build for the rpu_app snapshot.
#
# Compiled sources:
#   - apu_rpmsg_platform.c    — Link A OpenAMP slave transport
#   - tessai_agent_endpoint.c — "tessai-agent" rpmsg endpoint handler
#                               table (fw_echo, ECHO_REGION, DAPP,
#                               local-R5_1 COPY_IN_OUT, fault ops)
#   - metal_init_once.c       — libmetal one-shot init guard
zephyr_library()

# ─── Link A transport (APU tessai-agent <-> RPURuntime slave) ────────────────
if(CONFIG_TESSAI_APU_RPMSG_SLAVE)
    zephyr_library_sources(
        src/apu_rpmsg_platform.c
        src/tessai_agent_endpoint.c
    )
    # NOTE: the matching
    #   zephyr_linker_section(NAME .resource_table GROUP RSC_TABLE NOINPUT)
    #   zephyr_linker_section_configure(SECTION .resource_table KEEP INPUT ".resource_table*")
    # calls MUST live in the application's top-level CMakeLists.txt
    # (after find_package(Zephyr)), not here in the module. Module-
    # scope zephyr_linker_section calls do not propagate to the
    # linker generator in Zephyr 3.7 — resource_table.c.obj's
    # .resource_table section gets emitted but the linker
    # orphan-places it, losing the RSC_TABLE memory-region placement.
    # See tests/tessai_agent_test/CMakeLists.txt for the canonical
    # call site; mirror it in any other RPURuntime test that turns
    # CONFIG_TESSAI_APU_RPMSG_SLAVE on.
endif()

# metal_init_once gates on CONFIG_OPENAMP (auto-selected by
# TESSAI_APU_RPMSG_SLAVE in Kconfig). Kept gated rather than
# unconditional so a hypothetical future Link-A-off build doesn't
# pull in <metal/sys.h> for nothing.
if(CONFIG_OPENAMP)
    zephyr_library_sources(src/metal_init_once.c)
endif()

zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
