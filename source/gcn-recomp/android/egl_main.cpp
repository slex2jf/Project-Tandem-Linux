#include <cstdlib>
#include <jni.h>
// TANDEM_K043_LAYOUT
// TANDEM_K042_LAYOUT
// TANDEM_K041_LAYOUT
// TANDEM_K040_LAYOUT
// TANDEM_K039_LAYOUT
// TANDEM_K038_LAYOUT
// TANDEM_K037_LAYOUT
// TANDEM_K035_LAYOUT
bool audio_open();
void audio_pause();
void audio_resume();
void audio_close();
// NativeActivity EGL/OpenGL ES frontend for the original GX renderer.
#include <android/log.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <string>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <android/input.h>
#include <android/native_activity.h>
#include <android/window.h>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <cerrno>
#include <unistd.h>
#include <fstream>
#include <filesystem>
#include "../../src/eos_config.h"
#include "runtime.h"
#include "platform.h"
#include "hw/pad.h"
#include "input_script.h"
#include "gx/render.h"
#include "gx/render_gl.h"
#include "gx/gl.h"

static ANativeWindow* g_touch_window=nullptr;

uint32_t boot_load(const char*);
extern "C" void tandem_iso_selected(int fd, const char* display_name);
extern "C" void tandem_iso_cancelled();
namespace {
EGLDisplay display = EGL_NO_DISPLAY;
EGLSurface surface = EGL_NO_SURFACE;
EGLContext context = EGL_NO_CONTEXT;
EGLConfig config = nullptr;
ANativeWindow* egl_window = nullptr;
bool ready = false;
bool renderer_initialized = false;
bool lifecycle_paused = false;
bool focus_lost = false;
bool game_paused = false;
unsigned long frames = 0;
std::mutex iso_mutex;
std::condition_variable iso_cv;
int iso_fd = -1;
std::string iso_display_name;
bool iso_result_ready = false;
std::mutex eos_config_mutex;
std::condition_variable eos_config_cv;
int eos_config_fd = -1;
bool eos_config_result_ready = false;
void* gl_proc(const char* name) { return reinterpret_cast<void*>(eglGetProcAddress(name)); }
// A NativeActivity can destroy its window surface during rotation, focus changes,
// or the SAF picker while the EGL context is still perfectly usable.  Destroying
// the context here invalidates every GL object owned by render_gl.cpp, while the
// old renderer_initialized flag used to make those stale names look live on the
// next surface.  Keep the context/display until the Activity itself is destroyed.
void stop(bool destroy_context = false) {
    ready = false;
    if (display != EGL_NO_DISPLAY && destroy_context) {
        eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display,surface);
        surface = EGL_NO_SURFACE;
        egl_window = nullptr;
        {
            if (context != EGL_NO_CONTEXT) eglDestroyContext(display,context);
            eglTerminate(display);
            display=EGL_NO_DISPLAY;
            context=EGL_NO_CONTEXT;
            config=nullptr;
            renderer_initialized=false;
        }
    }
}

void update_lifecycle_pause() {
    const bool should_pause = lifecycle_paused || focus_lost;
    if (should_pause == game_paused) return;
    game_paused = should_pause;
    if (game_paused) {
        audio_pause();
        __android_log_print(ANDROID_LOG_INFO, "TANDEM-LIFECYCLE",
                            "Jogo pausado: audio e render suspensos");
    } else {
        audio_resume();
        __android_log_print(ANDROID_LOG_INFO, "TANDEM-LIFECYCLE",
                            "Jogo retomado: audio e render restaurados");
    }
}

static bool start_iso_picker(ANativeActivity* activity) {
    if (!activity || !activity->vm || !activity->clazz) return false;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
        attached = true;
    }
    jclass cls = env->GetObjectClass(activity->clazz);
    jmethodID open = env->GetMethodID(cls, "openIsoPicker", "()V");
    bool ok = open != nullptr && !env->ExceptionCheck();
    if (ok) env->CallVoidMethod(activity->clazz, open);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); ok = false; }
    env->DeleteLocalRef(cls);
    if (attached) activity->vm->DetachCurrentThread();
    return ok;
}

static bool start_persisted_iso(ANativeActivity* activity) {
    if (!activity || !activity->vm || !activity->clazz) return false;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
        attached = true;
    }
    jclass cls = env->GetObjectClass(activity->clazz);
    jmethodID open = cls ? env->GetMethodID(cls, "openPersistedIso", "()Z") : nullptr;
    bool ok = open != nullptr && !env->ExceptionCheck();
    if (ok) ok = env->CallBooleanMethod(activity->clazz, open) == JNI_TRUE;
    if (env->ExceptionCheck()) { env->ExceptionClear(); ok = false; }
    if (cls) env->DeleteLocalRef(cls);
    if (attached) activity->vm->DetachCurrentThread();
    return ok;
}

static bool start_eos_config_picker(ANativeActivity* activity) {
    if (!activity || !activity->vm || !activity->clazz) return false;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
        attached = true;
    }
    jclass cls = env->GetObjectClass(activity->clazz);
    jmethodID open = env->GetMethodID(cls, "openEosConfigPicker", "()V");
    bool ok = open != nullptr && !env->ExceptionCheck();
    if (ok) env->CallVoidMethod(activity->clazz, open);
    if (env->ExceptionCheck()) { env->ExceptionClear(); ok = false; }
    env->DeleteLocalRef(cls);
    if (attached) activity->vm->DetachCurrentThread();
    return ok;
}

extern "C" void tandem_iso_selected(int fd, const char* display_name) {
    std::lock_guard<std::mutex> lock(iso_mutex);
    if (iso_fd >= 0) close(iso_fd);
    iso_fd = fd;
    iso_display_name = display_name ? display_name : "selected.iso";
    iso_result_ready = true;
    iso_cv.notify_one();
}

extern "C" void tandem_iso_cancelled() {
    std::lock_guard<std::mutex> lock(iso_mutex);
    iso_fd = -1;
    iso_display_name.clear();
    iso_result_ready = true;
    iso_cv.notify_one();
}

extern "C" JNIEXPORT void JNICALL
Java_com_ethannicholas_mkdd_TandemActivity_nativeIsoSelected(
    JNIEnv* env, jclass, jint fd, jstring display_name) {
    const char* name = display_name ? env->GetStringUTFChars(display_name, nullptr) : nullptr;
    tandem_iso_selected(fd, name);
    if (name) env->ReleaseStringUTFChars(display_name, name);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ethannicholas_mkdd_TandemActivity_nativeIsoCancelled(JNIEnv*, jclass) {
    tandem_iso_cancelled();
}

extern "C" JNIEXPORT void JNICALL
Java_com_ethannicholas_mkdd_TandemActivity_nativeEosConfigSelected(
    JNIEnv*, jclass, jint fd) {
    std::lock_guard<std::mutex> lock(eos_config_mutex);
    if (eos_config_fd >= 0) close(eos_config_fd);
    eos_config_fd = fd;
    eos_config_result_ready = true;
    eos_config_cv.notify_one();
}

extern "C" JNIEXPORT void JNICALL
Java_com_ethannicholas_mkdd_TandemActivity_nativeEosConfigCancelled(JNIEnv*) {
    std::lock_guard<std::mutex> lock(eos_config_mutex);
    if (eos_config_fd >= 0) close(eos_config_fd);
    eos_config_fd = -1;
    eos_config_result_ready = true;
    eos_config_cv.notify_one();
}

static bool store_eos_config(int fd, const char* root) {
    if (fd < 0 || !root) return false;
    const std::string path = std::string(root) + "/eos.json";
    const std::string temporary = path + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) { close(fd); return false; }
    char block[4096]; size_t total = 0;
    for (;;) {
        const ssize_t count = read(fd, block, sizeof(block));
        if (count < 0) { output.close(); unlink(temporary.c_str()); close(fd); return false; }
        if (!count) break;
        total += static_cast<size_t>(count);
        if (total > 16384) { output.close(); unlink(temporary.c_str()); close(fd); return false; }
        output.write(block, count);
        if (!output) { output.close(); unlink(temporary.c_str()); close(fd); return false; }
    }
    close(fd); output.close();
    if (!output || total == 0) { unlink(temporary.c_str()); return false; }
    try {
        mkdd::eos::Config::load(temporary);
        if (rename(temporary.c_str(), path.c_str()) != 0) { unlink(temporary.c_str()); return false; }
        return true;
    } catch (...) {
        unlink(temporary.c_str());
        return false;
    }
}

static bool iso_fd_has_gamecube_header(int fd) {
    char header[6]{};
    if (fd < 0 || pread(fd, header, sizeof(header), 0) != sizeof(header))
        return false;
    // This recompilation targets Mario Kart: Double Dash!! GM4E01.
    return memcmp(header, "GM4E01", sizeof(header)) == 0;
}

bool start(ANativeWindow* window) {
    if (!window || ready) return false;
    if (display == EGL_NO_DISPLAY) {
        display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if(display==EGL_NO_DISPLAY || !eglInitialize(display,nullptr,nullptr)) return false;
        if(!eglBindAPI(EGL_OPENGL_ES_API)) return false;
        const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,
            EGL_DEPTH_SIZE,24,EGL_NONE};
        EGLint count=0;
        if(!eglChooseConfig(display,attrs,&config,1,&count) || count<1) return false;
    }
    EGLint visual=0; eglGetConfigAttrib(display,config,EGL_NATIVE_VISUAL_ID,&visual);
    // TANDEM_K050_LOW_RES
    ANativeWindow_setBuffersGeometry(window,1386,640,visual);
    if (surface != EGL_NO_SURFACE && egl_window != window) {
        // The old native window really went away.  The EGL context remains
        // alive, so deleting only this surface preserves all K192 GL objects.
        eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        eglDestroySurface(display,surface);
        surface = EGL_NO_SURFACE;
        egl_window = nullptr;
    }
    if (surface == EGL_NO_SURFACE) {
        surface=eglCreateWindowSurface(display,config,window,nullptr);
        egl_window = window;
    }
    if (context == EGL_NO_CONTEXT) {
        const EGLint ctx[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
        context=eglCreateContext(display,config,EGL_NO_CONTEXT,ctx);
    }
    if(surface==EGL_NO_SURFACE || context==EGL_NO_CONTEXT ||
       !eglMakeCurrent(display,surface,surface,context)) return false;
    const int version=gl_load_with(gl_proc);
    __android_log_print(ANDROID_LOG_INFO,"TANDEM-GL","GLAD version=%d EGL=%p",version,display);
    if(version<32) return false;
    EGLint w=0,h=0; eglQuerySurface(display,surface,EGL_WIDTH,&w);
    eglQuerySurface(display,surface,EGL_HEIGHT,&h);
    gx::render_set_window_size(w,h);
    if (!renderer_initialized) {
        gx::render_init(1);
        renderer_initialized=true;
    }
    ready=true;
    return true;
}
void command(android_app* app,int32_t cmd) {
    if(cmd==APP_CMD_INIT_WINDOW && app->window) {
        g_touch_window=app->window;
        if(!start(app->window)) __android_log_print(ANDROID_LOG_ERROR,"TANDEM-GL","EGL init failed: 0x%x",eglGetError());
    } else if(cmd==APP_CMD_TERM_WINDOW) stop(false);
    else if(cmd==APP_CMD_DESTROY) stop(true);
    else if(cmd==APP_CMD_PAUSE) {
        lifecycle_paused = true;
        update_lifecycle_pause();
    } else if(cmd==APP_CMD_RESUME) {
        lifecycle_paused = false;
        update_lifecycle_pause();
    } else if(cmd==APP_CMD_LOST_FOCUS) {
        focus_lost = true;
        update_lifecycle_pause();
    } else if(cmd==APP_CMD_GAINED_FOCUS) {
        focus_lost = false;
        update_lifecycle_pause();
    }
    else if(cmd==APP_CMD_WINDOW_RESIZED && ready) {
        EGLint w=0,h=0; eglQuerySurface(display,surface,EGL_WIDTH,&w);
        eglQuerySurface(display,surface,EGL_HEIGHT,&h);
        gx::render_set_window_size(w,h);
    }
}

// TANDEM_K020_TOUCH
struct TouchPoint {
    int id = -1;
    float x = 0;
    float y = 0;
};

TouchPoint touches[16];

void update_touch(AInputEvent* event) {
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return;

    int action = AMotionEvent_getAction(event);
    int masked = action & AMOTION_EVENT_ACTION_MASK;
    int index = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

    if (masked == AMOTION_EVENT_ACTION_CANCEL) {
        for (auto& t : touches) t.id = -1;
        return;
    }

    if (masked == AMOTION_EVENT_ACTION_UP ||
        masked == AMOTION_EVENT_ACTION_POINTER_UP) {
        if (index < (int)AMotionEvent_getPointerCount(event)) {
            int id = AMotionEvent_getPointerId(event, index);
            for (auto& t : touches)
                if (t.id == id) t.id = -1;
        }
    }

    if (masked == AMOTION_EVENT_ACTION_DOWN ||
        masked == AMOTION_EVENT_ACTION_POINTER_DOWN ||
        masked == AMOTION_EVENT_ACTION_MOVE) {
        size_t count = AMotionEvent_getPointerCount(event);

        for (size_t i = 0; i < count; ++i) {
            if ((masked == AMOTION_EVENT_ACTION_DOWN ||
                 masked == AMOTION_EVENT_ACTION_POINTER_DOWN) &&
                (int)i != index) continue;

            int id = AMotionEvent_getPointerId(event, i);
            TouchPoint* slot = nullptr;

            for (auto& t : touches)
                if (t.id == id) { slot = &t; break; }

            if (!slot)
                for (auto& t : touches)
                    if (t.id == -1) { slot = &t; break; }

            if (slot) {
                slot->id = id;
                slot->x = AMotionEvent_getX(event, i);
                slot->y = AMotionEvent_getY(event, i);
            }
        }
    }
}

int32_t input(android_app*, AInputEvent* event) {
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
        return 0;
    update_touch(event);
    return 1;
}



// TANDEM_K023_COMPLETE
struct HudButton {
    float x,y,r;
    uint16_t mask;
    const char* label;
    float touch_scale = 1.5f;
};

static constexpr HudButton hud_buttons[] = {
    {0.8600f,0.7600f,0.0845f,PAD_A,"A"},
    {0.7827f,0.7600f,0.0650f,PAD_B,"B"},
    {0.8550f,0.4500f,0.065f,PAD_X,"X"},
    {0.82f,0.12f,0.035f,PAD_Z,"Z"},
    {0.09f,0.12f,0.045f,PAD_L,"L"},
    {0.20f,0.12f,0.040f,PAD_Y,"Y",1.0f},
    {0.91f,0.12f,0.045f,PAD_R,"R"},
    {0.50f,0.87f,0.040f,PAD_START,"START"}
};


static GLuint hud_program=0,hud_vao=0,hud_vbo=0;
static GLint hud_color=-1;
static bool hud_failed=false;

static GLuint hud_shader(GLenum type,const char* src) {
    GLuint sh=glCreateShader(type);
    glShaderSource(sh,1,&src,nullptr);
    glCompileShader(sh);
    GLint ok=0;
    glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
    if(!ok) {
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static bool hud_init() {
    const char* vs=R"(#version 300 es
        layout(location=0) in vec2 pos;
        void main(){gl_Position=vec4(pos,0.0,1.0);}
    )";
    const char* fs=R"(#version 300 es
        precision mediump float;
        uniform vec4 color;
        out vec4 frag;
        void main(){frag=color;}
    )";

    GLuint v=hud_shader(GL_VERTEX_SHADER,vs);
    GLuint f=hud_shader(GL_FRAGMENT_SHADER,fs);
    if(!v || !f) {
        if(v) glDeleteShader(v);
        if(f) glDeleteShader(f);
        return false;
    }

    hud_program=glCreateProgram();
    glAttachShader(hud_program,v);
    glAttachShader(hud_program,f);
    glLinkProgram(hud_program);
    glDeleteShader(v);
    glDeleteShader(f);

    GLint ok=0;
    glGetProgramiv(hud_program,GL_LINK_STATUS,&ok);
    if(!ok) {
        glDeleteProgram(hud_program);
        hud_program=0;
        return false;
    }

    hud_color=glGetUniformLocation(hud_program,"color");

    glGenVertexArrays(1,&hud_vao);
    glGenBuffers(1,&hud_vbo);
    glBindVertexArray(hud_vao);
    glBindBuffer(GL_ARRAY_BUFFER,hud_vbo);
    glBufferData(GL_ARRAY_BUFFER,8192,nullptr,GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);
    glBindVertexArray(0);
    return true;
}



static void hud_emit(const float* v,int count,GLenum mode) {
    glBufferSubData(GL_ARRAY_BUFFER,0,count*2*sizeof(float),v);
    glDrawArrays(mode,0,count);
}

static void hud_circle(float x,float y,float r,
                       float w,float h,bool filled) {
    constexpr int n=48;
    float v[(n+2)*2]{};
    int count=0;
    if(filled) {
        v[count++]=x*2-1;
        v[count++]=1-y*2;
    }
    for(int i=0;i<=n;i++) {
        float a=6.28318530718f*i/n;
        float px=x+std::cos(a)*r*std::min(w,h)/w;
        float py=y+std::sin(a)*r*std::min(w,h)/h;
        v[count++]=px*2-1;
        v[count++]=1-py*2;
    }
    hud_emit(v,count/2,filled?GL_TRIANGLE_FAN:GL_LINE_STRIP);
}

static void hud_line(float x1,float y1,float x2,float y2,
                     float w,float h) {
    float v[]={
        x1*2-1,1-y1*2,
        x2*2-1,1-y2*2
    };
    hud_emit(v,2,GL_LINES);
}

static const char* hud_glyph(char c) {
    switch(c) {
        case 'A': return "011101000110001111111000110001";
        case 'B': return "111101000110001111101000111110";
        case 'X': return "10001100010101000100010101000110001";
        case 'Z': return "111110000100010001000100011111";
        case 'L': return "100001000010000100001000011111";
        case 'Y': return "10001100010101000100001000010000100";
        case 'R': return "111101000110001111101010010001";
        case 'S': return "011111000010000011100000111110";
        case 'T': return "111110010000100001000010000100";
        default: return nullptr;
    }
}

static void hud_text(const char* text,float cx,float cy,
                     float scale,float w,float h) {
    int len=0;
    while(text[len]) ++len;
    float unit=scale*std::min(w,h);
    float total=len*6*unit;
    float x0=cx*w-total*0.5f;
    float y0=cy*h-3.5f*unit;

    for(int k=0;k<len;k++) {
        const char* glyph=hud_glyph(text[k]);
        if(!glyph) continue;

        for(int row=0;row<7;row++)
            for(int col=0;col<5;col++)
                if(glyph[row*5+col]=='1') {
                    float x=x0+(k*6+col)*unit;
                    float y=y0+row*unit;
                    float v[]={
                        x/w*2-1,1-y/h*2,
                        (x+unit)/w*2-1,1-y/h*2,
                        x/w*2-1,1-(y+unit)/h*2,
                        (x+unit)/w*2-1,1-y/h*2,
                        (x+unit)/w*2-1,1-(y+unit)/h*2,
                        x/w*2-1,1-(y+unit)/h*2
                    };
                    hud_emit(v,6,GL_TRIANGLES);
                }
    }
}

// TANDEM_K033_LAYOUT
// TANDEM_K031_MOVING_STICK
static float hud_stick_x = 0.18f;
static float hud_stick_y = 0.76f;

static void hud_draw() {
    if(!ready || hud_failed) return;

    EGLint w=0,h=0;
    eglQuerySurface(display,surface,EGL_WIDTH,&w);
    eglQuerySurface(display,surface,EGL_HEIGHT,&h);
    if(w<=0 || h<=0) return;

    GLint program=0,vao=0,array_buffer=0;
    GLint previous_fbo=0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&previous_fbo);
    GLint viewport[4]{},scissor_box[4]{};
    GLint blend_src_rgb=0,blend_dst_rgb=0;
    GLint blend_src_alpha=0,blend_dst_alpha=0;
    GLint blend_eq_rgb=0,blend_eq_alpha=0;
    GLint active_texture=0;
    GLboolean color_mask[4]{};
    GLboolean depth_mask=GL_TRUE;
    GLboolean blend=glIsEnabled(GL_BLEND);
    GLboolean depth=glIsEnabled(GL_DEPTH_TEST);
    GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST);
    GLboolean cull=glIsEnabled(GL_CULL_FACE);
    GLboolean stencil=glIsEnabled(GL_STENCIL_TEST);

    glGetIntegerv(GL_CURRENT_PROGRAM,&program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&array_buffer);
    glGetIntegerv(GL_VIEWPORT,viewport);
    glGetIntegerv(GL_SCISSOR_BOX,scissor_box);
    glGetIntegerv(GL_BLEND_SRC_RGB,&blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB,&blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&blend_src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA,&blend_dst_alpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB,&blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA,&blend_eq_alpha);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&active_texture);
    glGetBooleanv(GL_COLOR_WRITEMASK,color_mask);
    glGetBooleanv(GL_DEPTH_WRITEMASK,&depth_mask);

    if(!hud_program && !hud_init()) {
        hud_failed=true;
        glBindBuffer(GL_ARRAY_BUFFER,array_buffer);
        glBindVertexArray(vao);
        glUseProgram(program);
        return;
    }

    // TANDEM_K026_FIX
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    glViewport(0,0,w,h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(hud_program);
    glBindVertexArray(hud_vao);
    glBindBuffer(GL_ARRAY_BUFFER,hud_vbo);

    for(const auto& b:hud_buttons) {
        glUniform4f(hud_color,.55f,.55f,.55f,.23f);
        hud_circle(b.x,b.y,b.r,w,h,true);
        glUniform4f(hud_color,.92f,.92f,.92f,.72f);
        hud_circle(b.x,b.y,b.r,w,h,false);
        hud_text(b.label,b.x,b.y,.0023f,w,h);
    }

    glUniform4f(hud_color,.55f,.55f,.55f,.20f);
    hud_circle(.18f,.76f,.128f,w,h,true);
    glUniform4f(hud_color,.92f,.92f,.92f,.70f);
    hud_circle(.18f,.76f,.128f,w,h,false);

    glUniform4f(hud_color,.75f,.75f,.75f,.42f);
    hud_circle(hud_stick_x,hud_stick_y,.0525f,w,h,true);

    glBindFramebuffer(GL_FRAMEBUFFER,previous_fbo);
    glBindBuffer(GL_ARRAY_BUFFER,array_buffer);
    glBindVertexArray(vao);
    glUseProgram(program);
    glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
    glScissor(scissor_box[0],scissor_box[1],scissor_box[2],scissor_box[3]);
    glBlendFuncSeparate(blend_src_rgb,blend_dst_rgb,
                        blend_src_alpha,blend_dst_alpha);
    glBlendEquationSeparate(blend_eq_rgb,blend_eq_alpha);
    glDepthMask(depth_mask);
    glColorMask(color_mask[0],color_mask[1],
                color_mask[2],color_mask[3]);
    glActiveTexture(active_texture);

    if(blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if(depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if(scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if(cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if(stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
}

PadState touch_pad() {
    PadState pad{};
    pad.connected = true;

    if (!surface || surface == EGL_NO_SURFACE) return pad;

    EGLint width = 0, height = 0;
    if (g_touch_window) {
        width = ANativeWindow_getWidth(g_touch_window);
        height = ANativeWindow_getHeight(g_touch_window);
    }
    if (width <= 0 || height <= 0) return pad;

    float w = float(width);
    float h = float(height);

    hud_stick_x = 0.18f;
    hud_stick_y = 0.76f;

    for (const auto& t : touches) {
        if (t.id < 0) continue;

        float x = t.x / w;
        float y = t.y / h;

        // Controles alinhados ao layout K021.
        if (x < 0.40f && y > 0.50f) {
            const float visual_radius = 0.075f * std::min(w,h);
            float vx = t.x - 0.18f*w;
            float vy = t.y - 0.76f*h;
            float distance = std::sqrt(vx*vx + vy*vy);

            if (distance > visual_radius && distance > 0.0f) {
                vx *= visual_radius / distance;
                vy *= visual_radius / distance;
            }

            hud_stick_x = (0.18f*w + vx)/w;
            hud_stick_y = (0.76f*h + vy)/h;

            float dx = (x - 0.18f) / 0.085f;
            float dy = (0.76f - y) / 0.085f;

            dx = std::clamp(dx, -1.0f, 1.0f);
            dy = std::clamp(dy, -1.0f, 1.0f);

            if (std::fabs(dx) < 0.12f) dx = 0;
            if (std::fabs(dy) < 0.12f) dy = 0;

            pad.stick_x = uint8_t(128 + dx * 127);
            pad.stick_y = uint8_t(128 + dy * 127);
        }

        for (const auto& b : hud_buttons) {
            float dx = (x - b.x) * w;
            float dy = (y - b.y) * h;
            float r = b.r * std::min(w, h) * b.touch_scale;

            if (dx * dx + dy * dy <= r * r)
                pad.buttons |= b.mask;
        }

    }

    return pad;
}

}

// TANDEM_K044_IMMERSIVE
static void tandem_enable_immersive(ANativeActivity* activity) {
    if (!activity || !activity->vm || !activity->clazz)
        return;

    JNIEnv* env = nullptr;
    bool attached = false;

    if (activity->vm->GetEnv(
            reinterpret_cast<void**>(&env),
            JNI_VERSION_1_6) != JNI_OK) {
        if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
            return;
        attached = true;
    }

    jclass activityClass = env->GetObjectClass(activity->clazz);
    jmethodID getWindow = env->GetMethodID(
        activityClass,
        "getWindow",
        "()Landroid/view/Window;"
    );

    if (getWindow) {
        jobject window = env->CallObjectMethod(
            activity->clazz, getWindow
        );

        if (window) {
            jclass windowClass = env->GetObjectClass(window);
            jmethodID getDecorView = env->GetMethodID(
                windowClass,
                "getDecorView",
                "()Landroid/view/View;"
            );

            if (getDecorView) {
                jobject decor = env->CallObjectMethod(
                    window, getDecorView
                );

                if (decor) {
                    jclass viewClass = env->GetObjectClass(decor);
                    jmethodID setFlags = env->GetMethodID(
                        viewClass,
                        "setSystemUiVisibility",
                        "(I)V"
                    );

                    if (setFlags) {
                        // FULLSCREEN | HIDE_NAVIGATION |
                        // IMMERSIVE_STICKY | LAYOUT_FLAGS
                        env->CallVoidMethod(
                            decor, setFlags, 0x1706
                        );
                    }

                    env->DeleteLocalRef(viewClass);
                    env->DeleteLocalRef(decor);
                }
            }

            env->DeleteLocalRef(windowClass);
            env->DeleteLocalRef(window);
        }
    }

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }

    env->DeleteLocalRef(activityClass);

    if (attached)
        activity->vm->DetachCurrentThread();
}

extern "C" bool tandem_eos_java_init(ANativeActivity* activity);
extern "C" bool tandem_eos_native_init(ANativeActivity* activity);
extern "C" void tandem_eos_service_start(ANativeActivity* activity);

void android_main(android_app* app) {
    // TANDEM_K057C_ONLINE
    setenv("MKDD_ONLINE_MENU", "1", 1);

    tandem_enable_immersive(app->activity);

    app_dummy();
    // TANDEM_FULLSCREEN
    ANativeActivity_setWindowFlags(
        app->activity,
        AWINDOW_FLAG_FULLSCREEN,
        0
    );

    app->onAppCmd=command; app->onInputEvent=input;
    while(!app->window && !app->destroyRequested) {
        int events; android_poll_source* source=nullptr;
        int id=ALooper_pollOnce(100,nullptr,&events,(void**)&source);
        if(id>=0 && source) source->process(app,source);
    }
    const char* root=app->activity->internalDataPath;
    const std::string eos_config_path = std::string(root) + "/eos.json";
    if (std::filesystem::is_regular_file(eos_config_path)) {
        try {
            (void)mkdd::eos::Config::load(eos_config_path);
            __android_log_print(ANDROID_LOG_INFO, "TANDEM-EOS",
                                "eos.json privado valido; seletor EOS omitido");
        } catch (const std::exception& error) {
            __android_log_print(ANDROID_LOG_WARN, "TANDEM-EOS",
                                "eos.json privado invalido; multiplayer desativado (%s)",
                                error.what());
        }
    } else {
        __android_log_print(ANDROID_LOG_INFO, "TANDEM-EOS",
                            "eos.json privado ausente; multiplayer desativado");
    }
    // EOS configuration is optional for offline play.  The existing EOS picker
    // remains available as an explicit recovery/configuration entry point; it
    // is never mandatory during the normal boot path.
    // The official EOS Android SDK requires its Java bridge before native
    // EOS_Initialize.  Its dependencies are packaged in K198; no credential
    // is passed through this call.
    tandem_eos_java_init(app->activity);
    // Native EOS Connect/Lobby/P2P is initialized by the service.
    tandem_eos_service_start(app->activity);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(iso_mutex);
            if (iso_fd >= 0) close(iso_fd);
            iso_fd = -1;
            iso_display_name.clear();
            iso_result_ready = false;
        }
        bool persisted_started = start_persisted_iso(app->activity);
        if (persisted_started) {
            __android_log_print(ANDROID_LOG_INFO,"TANDEM-ISO",
                                "Tentando ISO persistida via permissao SAF");
        } else {
            __android_log_print(ANDROID_LOG_INFO,"TANDEM-ISO",
                                "Solicitando ISO pelo Android Storage Access Framework");
            if (!start_iso_picker(app->activity)) {
                __android_log_print(ANDROID_LOG_ERROR,"TANDEM-ISO","Falha ao iniciar ACTION_OPEN_DOCUMENT");
                return;
            }
        }
        // K084: manter o looper Android ativo enquanto o SAF responde.
        while (!app->destroyRequested) {
            {
                std::lock_guard<std::mutex> lock(iso_mutex);
                if (iso_result_ready) break;
            }
            int events = 0;
            android_poll_source* source = nullptr;
            int id = ALooper_pollOnce(16, nullptr, &events,
                                     (void**)&source);
            if (id >= 0 && source) source->process(app, source);
        }
        if (app->destroyRequested) {
            __android_log_print(ANDROID_LOG_WARN, "TANDEM-ISO",
                                "Activity encerrada durante selecao");
            return;
        }
        std::unique_lock<std::mutex> iso_lock(iso_mutex);
        if (iso_fd < 0) {
            __android_log_print(ANDROID_LOG_ERROR,"TANDEM-ISO","Selecao cancelada ou descritor invalido");
            return;
        }
        if (!iso_fd_has_gamecube_header(iso_fd)) {
            __android_log_print(ANDROID_LOG_ERROR,"TANDEM-ISO",
                                "Arquivo selecionado nao e uma ISO GM4E01; selecione a imagem do jogo");
            close(iso_fd);
            iso_fd = -1;
            iso_display_name.clear();
            iso_lock.unlock();
            continue;
        }
        const std::string iso = "/proc/self/fd/" + std::to_string(iso_fd);
        __android_log_print(ANDROID_LOG_INFO,"TANDEM-ISO","ISO SAF selecionada: %s fd=%d (sem copia)", iso_display_name.c_str(), iso_fd);
        iso_lock.unlock();
        g_save_dir=root;
        mem_init(); timing_init(); input_script_init(); configure_game_boot();
        uint32_t entry=boot_load(iso.c_str()); threads_start_boot(entry);
        // TANDEM_K004_AUDIO_START
        if (!audio_open()) __android_log_print(ANDROID_LOG_ERROR,"TANDEM-AUDIO","Falha ao iniciar audio");
        break;
    }
    while(!app->destroyRequested) {
        int events; android_poll_source* source=nullptr;
        int id=ALooper_pollOnce(ready?0:16,nullptr,&events,(void**)&source);
        if(id>=0 && source) source->process(app,source);
        
        if(ready && !game_paused) {
            EGLint sw=0,sh=0;
            static EGLint last_w=0,last_h=0;
            eglQuerySurface(display,surface,EGL_WIDTH,&sw);
            eglQuerySurface(display,surface,EGL_HEIGHT,&sh);
            if(sw>0 && sh>0 && (sw!=last_w || sh!=last_h)) {
                gx::render_set_window_size(sw,sh);
                glViewport(0,0,sw,sh);
                last_w=sw;
                last_h=sh;
                __android_log_print(ANDROID_LOG_INFO,"TANDEM-K026",
                    "Surface %dx%d",sw,sh);
            }
        }

        PadState pad = touch_pad(); pad_set_state(0,pad);
        if (game_paused) {
            // The guest threads keep their timing state while backgrounded.  Drain
            // batches produced during that interval instead of replaying stale or
            // partial frames when focus returns; the first resumed batch is then a
            // complete current frame while the audio callback remains gated.
            (void)gx::take_batch(0);
        } else if(ready) {
            if(auto batch=gx::take_batch(8)) {
                const bool presented=gx::render_execute(*batch);
                if(presented) {
                    hud_draw();
                    if(!eglSwapBuffers(display,surface)) {
                        __android_log_print(ANDROID_LOG_ERROR,"TANDEM-GL","swap failed 0x%x",eglGetError());
                        stop();
                    }
                    if(++frames%120==0) __android_log_print(ANDROID_LOG_INFO,"TANDEM-GL","presented=%lu",frames);
                }
            }
        }
    }
    audio_close(); stop(true); threads_request_quit();
}
