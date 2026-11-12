#include <android/log.h>
#include <android_native_app_glue.h>
#include <chrono>
#include <cstdlib>
#include <string>
#include "vulkan_renderer.h"
#include "runtime.h"
#include "platform.h"
#include "hw/pad.h"
#include "input_script.h"

uint32_t boot_load(const char*);
static android_app* g_app;
static bool g_ready;
static bool g_shutdown_done;
static void lifecycle_ready(bool ready,const char* reason) {
    if(g_ready!=ready) __android_log_print(ANDROID_LOG_INFO,"TANDEM-LIFECYCLE","ready %d -> %d reason=%s",int(g_ready),int(ready),reason);
    g_ready=ready;
}
static void on_cmd(android_app* app,int32_t cmd) {
    __android_log_print(ANDROID_LOG_INFO,"TANDEM-LIFECYCLE","cmd=%d window=%p pending=%p ready=%d",int(cmd),app->window,app->pendingWindow,int(g_ready));
    if(cmd==APP_CMD_INIT_WINDOW && app->window) {
        if(g_ready) { gx::vk::shutdown(); lifecycle_ready(false,"reinit-existing-window"); }
        const uint32_t w=ANativeWindow_getWidth(app->window), h=ANativeWindow_getHeight(app->window);
        __android_log_print(ANDROID_LOG_INFO,"TANDEM-LIFECYCLE","INIT_WINDOW window=%p size=%ux%u",app->window,w,h);
        const bool ok=gx::vk::init(app->window,w,h);
        g_shutdown_done=false;
        lifecycle_ready(ok,ok?"vk-init-ok":"vk-init-failed");
    } else if(cmd==APP_CMD_TERM_WINDOW) {
        __android_log_print(ANDROID_LOG_INFO,"TANDEM-LIFECYCLE","TERM_WINDOW window=%p",app->window);
        if(!g_shutdown_done) { gx::vk::shutdown(); g_shutdown_done=true; }
        lifecycle_ready(false,"term-window");
    } else if(cmd==APP_CMD_WINDOW_RESIZED && app->window) {
        __android_log_print(ANDROID_LOG_INFO,"TANDEM-LIFECYCLE","WINDOW_RESIZED size=%ux%u",ANativeWindow_getWidth(app->window),ANativeWindow_getHeight(app->window));
        gx::vk::resize(app->window,ANativeWindow_getWidth(app->window),ANativeWindow_getHeight(app->window));
    } else if(cmd==APP_CMD_DESTROY) {
        if(!g_shutdown_done) { gx::vk::shutdown(); g_shutdown_done=true; }
        lifecycle_ready(false,"destroy");
    }
}
static int32_t on_input(android_app*,AInputEvent*) { return 0; }

void android_main(android_app* app) {
    app_dummy(); g_app=app; app->onAppCmd=on_cmd; app->onInputEvent=on_input;
    g_shutdown_done=false;
    __android_log_print(ANDROID_LOG_INFO,"TANDEM-LIFECYCLE","android_main start window=%p",app->window);
    while(!app->window && !app->destroyRequested) { int ident; int events; android_poll_source* source=nullptr; ident=ALooper_pollOnce(100,nullptr,&events,(void**)&source); if(ident>=0 && source)source->process(app,source); }
    const char* root=app->activity->internalDataPath; std::string iso=std::string(root)+"/game.iso"; g_save_dir=root;
    __android_log_print(ANDROID_LOG_INFO,"TANDEM-ISO","internalDataPath=%s iso=%s",root,iso.c_str());
    mem_init(); timing_init(); input_script_init(); configure_game_boot(); uint32_t entry=boot_load(iso.c_str()); threads_start_boot(entry);
    while(!app->destroyRequested) {
        int ident; int events; android_poll_source* source=nullptr; const int timeout=g_ready?0:16; while((ident=ALooper_pollOnce(timeout,nullptr,&events,(void**)&source))>=0) { if(source)source->process(app,source); }
        PadState p{}; p.connected=true; pad_set_state(0,p);
        
        static unsigned long loops=0, batches=0, draws=0, empty=0;
        ++loops;
        if(g_ready && app->window) {
            if(auto b=gx::take_batch(8)) {
                ++batches;
                if(b->verts.empty() || b->indices.empty()) ++empty;
                if(gx::vk::draw(*b)) ++draws;
            }
        }
        if(loops%600==0) {
            __android_log_print(ANDROID_LOG_INFO,"TANDEM-GX",
                "loops=%lu batches=%lu draws=%lu empty=%lu ready=%d",
                loops,batches,draws,empty,int(g_ready));
        }

    }
    if(!g_shutdown_done) gx::vk::shutdown();
    threads_request_quit();
}
