#include <android/native_activity.h>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include "../../src/eos_service.h"
#include "../../src/eos_local_http.h"
#include "../../src/rooms_native.h"

#define EOSLOG(...) __android_log_print(ANDROID_LOG_INFO,"TANDEM-EOS",__VA_ARGS__)
#define EOSERR(...) __android_log_print(ANDROID_LOG_ERROR,"TANDEM-EOS",__VA_ARGS__)

extern "C" void tandem_eos_service_start(ANativeActivity* activity) {
    static std::atomic<bool> started{false};
    if (!activity || !activity->internalDataPath) {
        EOSERR("Diretorio interno indisponivel");
        return;
    }
    if (started.exchange(true)) return;
    const std::string root = activity->internalDataPath;
    const std::string config = root + "/eos.json";
    const std::string ready = root + "/eos.ready";
    std::thread([config,ready]() {
        try {
            // Do not start a local room server without a real EOS
            // configuration.  A listening port is not an online backend.
            if (!std::filesystem::is_regular_file(config)) {
                EOSERR("EOS config ausente: %s; EOS nao iniciado", config.c_str());
                return;
            }

            // Upstream is loopback-only; it is a bridge to the native EOS
            // runtime, not a replacement for EOS.
            mkdd::native_rooms::Rooms rooms;
            mkdd::eos::LocalHttp upstream(28122,false);
            EOSLOG("EOS upstream loopback ativo na porta %u",upstream.port());
            std::thread([config,ready]() {
                try {
                    EOSLOG("Inicializando EOS nativo com upstream 28122");
                    mkdd::eos::serve(config,28122,ready,0,false);
                } catch (const std::exception& e) {
                    EOSERR("EOS nativo falhou: %s",e.what());
                }
            }).detach();
            for (;;) {
                upstream.poll([&rooms](const mkdd::online::Json& request) {
                    return rooms.request(request);
                });
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } catch (const std::exception& e) {
            EOSERR("K137: upstream falhou: %s",e.what());
        }
    }).detach();
}
