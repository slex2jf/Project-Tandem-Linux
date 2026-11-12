#include <android/native_activity.h>
#include <android/log.h>
#include <jni.h>
#include <string>
#include "eos_sdk.h"
#include "eos_common.h"
#include "Android/eos_Android.h"

extern "C" bool tandem_eos_native_init(ANativeActivity* activity) {
    if (!activity || !activity->internalDataPath)
        return false;

    EOS_InitializeOptions options{};
    options.ApiVersion = EOS_INITIALIZE_API_LATEST;
    options.ProductName = "Tandem MKDD";
    options.ProductVersion = "1.0";

    EOS_Android_InitializeOptions androidOptions{};
    androidOptions.ApiVersion =
        EOS_ANDROID_INITIALIZEOPTIONS_API_LATEST;
    androidOptions.OptionalInternalDirectory =
        activity->internalDataPath;
    androidOptions.OptionalExternalDirectory =
        activity->internalDataPath;

    options.SystemInitializeOptions = &androidOptions;

    EOS_EResult result = EOS_Initialize(&options);

    __android_log_print(
        result == EOS_EResult::EOS_Success || result == EOS_EResult::EOS_AlreadyConfigured
            ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
        "TANDEM-EOS",
        "EOS_Initialize result=%d", (int)result
    );

    return result == EOS_EResult::EOS_Success ||
           result == EOS_EResult::EOS_AlreadyConfigured;
}
