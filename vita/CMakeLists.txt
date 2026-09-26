# Vita application target. Configured only under the VitaSDK toolchain
# (root CMakeLists adds this subdirectory when VITA is set).

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaIPTV")
# Placeholder 9-character title ID; a final unique one is an M5 task.
set(VITA_TITLEID  "VIPTV0001")
set(VITA_VERSION  "00.01")

add_executable(vitaiptv_vita
    src/main.c
    src/vi_http.c
    src/vi_vdec.c
)

target_link_libraries(vitaiptv_vita
    vicore          # portable core (parsers, demux, log)
    vita2d
    # vita2d dependencies
    freetype
    png
    jpeg
    z
    m
    # SCE stubs
    ScePvf_stub
    SceGxm_stub
    SceDisplay_stub
    SceSysmodule_stub
    SceCtrl_stub
    SceCommonDialog_stub
    SceAppMgr_stub
    SceKernelDmacMgr_stub
    # pipeline: networking, TLS, HTTP, hardware decode, file IO
    SceNet_stub
    SceNetCtl_stub
    SceHttp_stub
    SceSsl_stub
    SceVideodec_stub
    SceIofilemgr_stub
)

vita_create_self(eboot.bin vitaiptv_vita)
vita_create_vpk(vitaiptv.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)
