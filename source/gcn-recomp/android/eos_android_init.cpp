#include <jni.h>
#include <android/native_activity.h>
#include <android/log.h>

extern "C" bool tandem_eos_java_init(ANativeActivity* activity) {
    if (!activity || !activity->vm || !activity->clazz)
        return false;

    JNIEnv* env = nullptr;
    bool attached = false;

    if (activity->vm->GetEnv(
            reinterpret_cast<void**>(&env),
            JNI_VERSION_1_6) != JNI_OK) {
        if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
            return false;
        attached = true;
    }

    bool ok = false;

    jclass cls = nullptr;

    // The Android EOS Java wrapper is optional for Tandem.  Connect, Lobby
    // and P2P are initialized by the native EOS SDK below, while the Java
    // wrapper additionally depends on AndroidX Custom Tabs classes that are
    // not part of the native game APK.  Keep this helper safe for callers
    // that still probe it, but never make native EOS depend on it.

    jclass activityClass = env->GetObjectClass(activity->clazz);
    jmethodID getClassLoader = env->GetMethodID(
        activityClass,
        "getClassLoader",
        "()Ljava/lang/ClassLoader;"
    );

    if (getClassLoader) {
        jobject loader = env->CallObjectMethod(
            activity->clazz, getClassLoader
        );

        if (loader && !env->ExceptionCheck()) {
            jclass loaderClass = env->GetObjectClass(loader);
            jmethodID loadClass = env->GetMethodID(
                loaderClass,
                "loadClass",
                "(Ljava/lang/String;)Ljava/lang/Class;"
            );

            if (loadClass) {
                jstring name = env->NewStringUTF(
                    "com.epicgames.mobile.eossdk.EOSSDK"
                );

                cls = static_cast<jclass>(
                    env->CallObjectMethod(loader, loadClass, name)
                );

                env->DeleteLocalRef(name);
            }

            env->DeleteLocalRef(loaderClass);
            env->DeleteLocalRef(loader);
        }
    }

    env->DeleteLocalRef(activityClass);

    if (cls) {
        jmethodID init = env->GetStaticMethodID(
            cls, "init", "(Landroid/app/Activity;)V");

        if (init) {
            env->CallStaticVoidMethod(cls, init, activity->clazz);
            ok = !env->ExceptionCheck();
        }

        env->DeleteLocalRef(cls);
    }

    if (env->ExceptionCheck()) {
        // Do not dump a Java stack trace: it is noisy and can contain
        // installation details.  Record only the exception class.
        jthrowable exception = env->ExceptionOccurred();
        env->ExceptionClear();
        jclass exceptionClass = exception ? env->GetObjectClass(exception) : nullptr;
        jclass classClass = exceptionClass ? env->GetObjectClass(exceptionClass) : nullptr;
        jmethodID getName = classClass ? env->GetMethodID(classClass, "getName", "()Ljava/lang/String;") : nullptr;
        const char* name = nullptr;
        jstring nameString = getName ? static_cast<jstring>(env->CallObjectMethod(exceptionClass, getName)) : nullptr;
        if (nameString) name = env->GetStringUTFChars(nameString, nullptr);
        __android_log_print(ANDROID_LOG_WARN, "TANDEM-EOS",
                            "EOS Java bridge unavailable (%s)", name ? name : "Java exception");
        if (name && nameString) env->ReleaseStringUTFChars(nameString, name);
        if (nameString) env->DeleteLocalRef(nameString);
        if (classClass) env->DeleteLocalRef(classClass);
        if (exceptionClass) env->DeleteLocalRef(exceptionClass);
        if (exception) env->DeleteLocalRef(exception);
        ok = false;
    }

    __android_log_print(
        ok ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
        "TANDEM-EOS",
        "EOS Java init: %s",
        ok ? "OK" : "FALHOU");

    if (attached)
        activity->vm->DetachCurrentThread();

    return ok;
}
