#include "quest_discord.hpp"

#include "config.hpp"
#include "main.hpp"
#include "scotland2/shared/modloader.h"

#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <exception>
#include <limits>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
    constexpr jlong kApplicationId = 1028340906740420711LL;
    constexpr const char* kRpcVersion = "1";
    constexpr const char* kHelperSource = "/sdcard/ModData/com.beatgames.beatsaber/Mods/DiscordRichPresence/discord-rpc-helper.jar";

    JavaVM* g_vm = nullptr;
    // These references are captured from the Java-created Unity thread. A
    // std::thread attached through JNI normally has no application class loader,
    // so deriving them on the presence worker makes recovery impossible after an
    // early bridge-load failure.
    jobject g_appClassLoader = nullptr;
    jobject g_activityContext = nullptr;
    jobject g_bridge = nullptr;
    jmethodID g_connect = nullptr;
    jmethodID g_sendFrame = nullptr;
    jmethodID g_disconnect = nullptr;
    jmethodID g_getState = nullptr;
    jmethodID g_getLastError = nullptr;
    std::mutex g_jniMutex;
    std::atomic<uint64_t> g_nonce{1};
    std::string g_nativeError;
    std::string g_lastLoggedConnectionError;
    std::string g_lastLoggedProtocolError;

    struct PresenceState {
        std::string phase = "MainMenu";
        nlohmann::json song = nlohmann::json::object();
        nlohmann::json stats = nlohmann::json::object();
        int playerCount = 0;
        int maxPlayerCount = 0;
        std::string lobbyCode;
        std::string partyId;
        std::time_t songStart = 0;
        std::time_t songEnd = 0;
        int songLength = 0;
        std::time_t pauseStart = 0;
    };

    PresenceState g_state;
    std::mutex g_stateMutex;

    bool CheckAndClearException(JNIEnv* env, const char* operation) {
        // JNI calls report Java failures through a pending exception. A pending
        // exception poisons later JNI calls, so clear it at the exact boundary
        // and turn it into a native status instead of continuing with null IDs.
        if (!env) {
            g_nativeError = std::string(operation) + " failed because JNI is unavailable";
            return true;
        }
        if (!env->ExceptionCheck()) return false;

        // ExceptionDescribe writes only to logcat. Capture Throwable.toString()
        // manually so the actionable Java class/message also reaches Paper's
        // per-mod log file. This routine cannot use the LocalRef helper declared
        // below because it is itself the primitive used by those helpers.
        jthrowable exception = env->ExceptionOccurred();
        env->ExceptionClear();
        std::string javaMessage;
        if (exception) {
            jclass throwableClass = env->GetObjectClass(exception);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (throwableClass) {
                jmethodID toString = env->GetMethodID(
                    throwableClass, "toString", "()Ljava/lang/String;");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (toString) {
                    jstring text = static_cast<jstring>(
                        env->CallObjectMethod(exception, toString));
                    if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                    } else if (text) {
                        const char* chars = env->GetStringUTFChars(text, nullptr);
                        if (env->ExceptionCheck()) {
                            env->ExceptionClear();
                        } else if (chars) {
                            javaMessage.assign(chars);
                            env->ReleaseStringUTFChars(text, chars);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        env->DeleteLocalRef(text);
                    }
                }
                env->DeleteLocalRef(throwableClass);
            }
            env->DeleteLocalRef(exception);
        }

        g_nativeError = std::string(operation) + " threw " +
            (javaMessage.empty() ? "an unavailable Java exception" : javaMessage);
        logger.error("Quest Discord JNI: {}", g_nativeError);
        return true;
    }

    // JNI local references have a per-thread limit. Presence updates run for
    // the entire game session, so release every temporary deterministically
    // instead of eventually overflowing the local-reference table.
    template<class T>
    class LocalRef {
    public:
        LocalRef(JNIEnv* env, T value) : env(env), value(value) {}
        ~LocalRef() {
            if (env && value) env->DeleteLocalRef(value);
        }
        LocalRef(const LocalRef&) = delete;
        LocalRef& operator=(const LocalRef&) = delete;
        T get() const { return value; }
        explicit operator bool() const { return value != nullptr; }

    private:
        JNIEnv* env;
        T value;
    };

    // Validate both the returned JNI handle and its exception state before any
    // subsequent JNI call attempts to use that handle.
    template<class T>
    bool RequireJniValue(JNIEnv* env, T value, const char* operation) {
        if (CheckAndClearException(env, operation)) return false;
        if (value) return true;
        g_nativeError = std::string(operation) + " returned null";
        logger.error("Quest Discord JNI: {}", g_nativeError);
        return false;
    }

    JNIEnv* GetEnv(bool& attached) {
        attached = false;
        if (!g_vm) {
            g_vm = modloader_jvm;
            if (!g_vm) {
                g_nativeError = "Scotland2 did not expose the Android Java VM";
                return nullptr;
            }
        }

        JNIEnv* env = nullptr;
        const auto result = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (result == JNI_EDETACHED) {
            if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
                g_nativeError = "Could not attach the mod thread to the Java VM";
                return nullptr;
            }
            attached = true;
        } else if (result != JNI_OK) {
            g_nativeError = "Could not obtain a JNI environment";
            return nullptr;
        }
        return env;
    }

    // Own a native thread's temporary JVM attachment for the entire JNI call
    // scope. This object must be created before any LocalRef objects so C++'s
    // reverse destruction order releases every local reference first and only
    // then detaches the thread. Manually detaching before a LocalRef destructor
    // runs makes DeleteLocalRef use an invalid JNIEnv and Android's CheckJNI
    // deliberately aborts the whole Beat Saber process.
    class ScopedJniEnv {
    public:
        ScopedJniEnv() : env(GetEnv(attached)) {}
        ~ScopedJniEnv() {
            if (attached && g_vm) g_vm->DetachCurrentThread();
        }

        ScopedJniEnv(const ScopedJniEnv&) = delete;
        ScopedJniEnv& operator=(const ScopedJniEnv&) = delete;

        JNIEnv* get() const { return env; }
        explicit operator bool() const { return env != nullptr; }

    private:
        bool attached = false;
        JNIEnv* env = nullptr;
    };

    bool ReadJString(JNIEnv* env, jstring value, const char* operation,
                     std::string& result, bool allowNull = false) {
        result.clear();
        if (!value) {
            if (allowNull) return true;
            g_nativeError = std::string(operation) + " returned no string";
            logger.error("Quest Discord JNI: {}", g_nativeError);
            return false;
        }
        const char* chars = env->GetStringUTFChars(value, nullptr);
        if (CheckAndClearException(env, operation)) return false;
        if (!chars) {
            g_nativeError = std::string(operation) + " returned unreadable UTF-8";
            logger.error("Quest Discord JNI: {}", g_nativeError);
            return false;
        }
        result.assign(chars);
        env->ReleaseStringUTFChars(value, chars);
        return !CheckAndClearException(env, operation);
    }

    bool CopyHelperToCodeCache(JNIEnv* env, jobject context, std::string& destination) {
        LocalRef<jclass> contextClass(env, env->GetObjectClass(context));
        if (!RequireJniValue(env, contextClass.get(), "Context class lookup")) return false;
        jmethodID getCodeCacheDir = env->GetMethodID(
            contextClass.get(), "getCodeCacheDir", "()Ljava/io/File;");
        if (!RequireJniValue(env, getCodeCacheDir, "Context.getCodeCacheDir lookup")) return false;

        LocalRef<jobject> cacheDir(env, env->CallObjectMethod(context, getCodeCacheDir));
        if (!RequireJniValue(env, cacheDir.get(), "Context.getCodeCacheDir")) return false;

        LocalRef<jclass> fileClass(env, env->FindClass("java/io/File"));
        if (!RequireJniValue(env, fileClass.get(), "java.io.File lookup")) return false;
        jmethodID getAbsolutePath = env->GetMethodID(
            fileClass.get(), "getAbsolutePath", "()Ljava/lang/String;");
        if (!RequireJniValue(env, getAbsolutePath, "File.getAbsolutePath lookup")) return false;

        LocalRef<jstring> cachePathString(
            env, static_cast<jstring>(env->CallObjectMethod(cacheDir.get(), getAbsolutePath)));
        if (!RequireJniValue(env, cachePathString.get(), "File.getAbsolutePath")) return false;
        std::string cachePath;
        if (!ReadJString(env, cachePathString.get(), "Read code-cache path", cachePath)) return false;
        destination = cachePath + "/discord-rpc-helper.jar";

        std::ifstream input(kHelperSource, std::ios::binary);
        if (!input) {
            g_nativeError = std::string("Quest Discord helper is missing: ") + kHelperSource;
            return false;
        }

        // Existing helper copies are read-only by design. Temporarily restore
        // owner write access before truncating; ENOENT is expected on first run.
        if (chmod(destination.c_str(), 0600) != 0 && errno != ENOENT) {
            g_nativeError = "Could not make the existing Quest Discord helper writable";
            return false;
        }
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output) {
            g_nativeError = "Could not create the private Quest Discord helper copy";
            return false;
        }
        if (chmod(destination.c_str(), 0444) != 0) {
            output.close();
            std::remove(destination.c_str());
            g_nativeError = "Could not make the Quest Discord helper read-only";
            return false;
        }
        // Android 14+ requires dynamically loaded code to be read-only before
        // its contents are written. The already-open descriptor remains writable.
        output << input.rdbuf();
        output.flush();
        if (!output.good()) {
            output.close();
            std::remove(destination.c_str());
            g_nativeError = "Could not finish writing the Quest Discord helper copy";
            return false;
        }
        output.close();
        return true;
    }

    // Resolve UnityPlayer through the cached application loader. Unlike
    // FindClass on a native-attached worker, ClassLoader.loadClass consistently
    // sees classes packaged with Beat Saber.
    bool CacheActivityContextLocked(JNIEnv* env) {
        if (g_activityContext) return true;
        if (!g_appClassLoader) {
            g_nativeError = "Beat Saber's Java class loader was not cached on the Unity thread";
            return false;
        }

        LocalRef<jclass> classLoaderClass(env, env->FindClass("java/lang/ClassLoader"));
        if (!RequireJniValue(env, classLoaderClass.get(), "java.lang.ClassLoader lookup")) return false;
        jmethodID loadClass = env->GetMethodID(
            classLoaderClass.get(), "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        if (!RequireJniValue(env, loadClass, "ClassLoader.loadClass lookup")) return false;

        LocalRef<jstring> unityPlayerName(
            env, env->NewStringUTF("com.unity3d.player.UnityPlayer"));
        if (!RequireJniValue(env, unityPlayerName.get(), "Create UnityPlayer class name")) return false;
        LocalRef<jclass> unityPlayerClass(
            env, static_cast<jclass>(env->CallObjectMethod(
                g_appClassLoader, loadClass, unityPlayerName.get())));
        if (!RequireJniValue(env, unityPlayerClass.get(), "Load UnityPlayer")) return false;
        jfieldID currentActivity = env->GetStaticFieldID(
            unityPlayerClass.get(), "currentActivity", "Landroid/app/Activity;");
        if (!RequireJniValue(env, currentActivity, "UnityPlayer.currentActivity lookup")) return false;
        LocalRef<jobject> context(
            env, env->GetStaticObjectField(unityPlayerClass.get(), currentActivity));
        if (!RequireJniValue(env, context.get(), "UnityPlayer.currentActivity")) {
            g_nativeError = "Beat Saber activity is not available yet";
            return false;
        }

        g_activityContext = env->NewGlobalRef(context.get());
        if (!RequireJniValue(env, g_activityContext, "Cache Beat Saber activity context")) {
            g_activityContext = nullptr;
            return false;
        }
        return true;
    }

    // This must be called from Initialize(), which is entered from Unity/BSML.
    // Cache the application loader once; later native worker retries can then
    // load or rebuild the DEX bridge without depending on their boot-only loader.
    bool CacheApplicationContextLocked(JNIEnv* env) {
        if (!g_appClassLoader) {
            LocalRef<jclass> threadClass(env, env->FindClass("java/lang/Thread"));
            if (!RequireJniValue(env, threadClass.get(), "java.lang.Thread lookup")) return false;
            jmethodID currentThread = env->GetStaticMethodID(
                threadClass.get(), "currentThread", "()Ljava/lang/Thread;");
            if (!RequireJniValue(env, currentThread, "Thread.currentThread lookup")) return false;
            jmethodID getContextClassLoader = env->GetMethodID(
                threadClass.get(), "getContextClassLoader", "()Ljava/lang/ClassLoader;");
            if (!RequireJniValue(env, getContextClassLoader, "Thread.getContextClassLoader lookup")) return false;

            LocalRef<jobject> thread(
                env, env->CallStaticObjectMethod(threadClass.get(), currentThread));
            if (!RequireJniValue(env, thread.get(), "Thread.currentThread")) return false;
            LocalRef<jobject> parentLoader(
                env, env->CallObjectMethod(thread.get(), getContextClassLoader));
            if (!RequireJniValue(env, parentLoader.get(), "Thread.getContextClassLoader")) {
                g_nativeError = "Beat Saber's Java class loader is not available on the Unity thread";
                return false;
            }

            g_appClassLoader = env->NewGlobalRef(parentLoader.get());
            if (!RequireJniValue(env, g_appClassLoader, "Cache Beat Saber class loader")) {
                g_appClassLoader = nullptr;
                return false;
            }
        }
        return CacheActivityContextLocked(env);
    }

    bool LoadBridgeLocked(JNIEnv* env) {
        if (g_bridge) return true;
        if (!g_appClassLoader) {
            g_nativeError = "Quest Discord bridge has no cached app class loader; use Reconnect from the mod menu";
            return false;
        }
        // currentActivity can be temporarily null during startup. Once the app
        // loader is cached, a worker retry can safely resolve it later.
        if (!CacheActivityContextLocked(env)) return false;

        // Resolve and validate each class/method independently. Calling through
        // a null jclass or jmethodID is a fatal native error, not a recoverable
        // Java exception.
        LocalRef<jclass> classLoaderClass(env, env->FindClass("java/lang/ClassLoader"));
        if (!RequireJniValue(env, classLoaderClass.get(), "java.lang.ClassLoader lookup")) return false;
        jmethodID loadClass = env->GetMethodID(
            classLoaderClass.get(), "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        if (!RequireJniValue(env, loadClass, "ClassLoader.loadClass lookup")) return false;

        std::string dexPath;
        if (!CopyHelperToCodeCache(env, g_activityContext, dexPath)) return false;

        LocalRef<jclass> dexLoaderClass(env, env->FindClass("dalvik/system/DexClassLoader"));
        if (!RequireJniValue(env, dexLoaderClass.get(), "DexClassLoader lookup")) return false;
        jmethodID dexLoaderCtor = env->GetMethodID(
            dexLoaderClass.get(),
            "<init>",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/ClassLoader;)V");
        if (!RequireJniValue(env, dexLoaderCtor, "DexClassLoader constructor lookup")) return false;

        LocalRef<jstring> dexPathString(env, env->NewStringUTF(dexPath.c_str()));
        if (!RequireJniValue(env, dexPathString.get(), "Create helper DEX path")) return false;
        const auto slash = dexPath.find_last_of('/');
        if (slash == std::string::npos) {
            g_nativeError = "Quest Discord helper path has no parent directory";
            return false;
        }
        const std::string cachePath = dexPath.substr(0, slash);
        LocalRef<jstring> cachePathString(env, env->NewStringUTF(cachePath.c_str()));
        if (!RequireJniValue(env, cachePathString.get(), "Create helper cache path")) return false;
        LocalRef<jobject> dexLoader(
            env, env->NewObject(dexLoaderClass.get(), dexLoaderCtor,
                dexPathString.get(), cachePathString.get(), nullptr, g_appClassLoader));
        if (!RequireJniValue(env, dexLoader.get(), "DexClassLoader creation")) return false;

        LocalRef<jstring> bridgeName(
            env, env->NewStringUTF("com.rainzdev.discordrichpresencequest.DiscordRpcBridge"));
        if (!RequireJniValue(env, bridgeName.get(), "Create DiscordRpcBridge class name")) return false;
        LocalRef<jclass> bridgeClass(
            env, static_cast<jclass>(env->CallObjectMethod(
                dexLoader.get(), loadClass, bridgeName.get())));
        if (!RequireJniValue(env, bridgeClass.get(), "Load DiscordRpcBridge")) return false;

        jmethodID constructor = env->GetMethodID(
            bridgeClass.get(), "<init>", "(Landroid/content/Context;)V");
        if (!RequireJniValue(env, constructor, "DiscordRpcBridge constructor lookup")) return false;
        LocalRef<jobject> localBridge(
            env, env->NewObject(bridgeClass.get(), constructor, g_activityContext));
        if (!RequireJniValue(env, localBridge.get(), "Create DiscordRpcBridge")) return false;

        g_bridge = env->NewGlobalRef(localBridge.get());
        if (!RequireJniValue(env, g_bridge, "Create DiscordRpcBridge global reference")) return false;
        g_connect = env->GetMethodID(bridgeClass.get(), "connect", "(JLjava/lang/String;)Z");
        if (!RequireJniValue(env, g_connect, "DiscordRpcBridge.connect lookup")) goto method_failure;
        g_sendFrame = env->GetMethodID(bridgeClass.get(), "sendFrame", "(Ljava/lang/String;)Z");
        if (!RequireJniValue(env, g_sendFrame, "DiscordRpcBridge.sendFrame lookup")) goto method_failure;
        g_disconnect = env->GetMethodID(bridgeClass.get(), "disconnect", "()V");
        if (!RequireJniValue(env, g_disconnect, "DiscordRpcBridge.disconnect lookup")) goto method_failure;
        g_getState = env->GetMethodID(bridgeClass.get(), "getState", "()Ljava/lang/String;");
        if (!RequireJniValue(env, g_getState, "DiscordRpcBridge.getState lookup")) goto method_failure;
        g_getLastError = env->GetMethodID(bridgeClass.get(), "getLastError", "()Ljava/lang/String;");
        if (!RequireJniValue(env, g_getLastError, "DiscordRpcBridge.getLastError lookup")) goto method_failure;
        return true;

    method_failure:
        // A partially initialized bridge must never be reused on the next
        // reconnect attempt; release it and reset every cached method ID.
        env->DeleteGlobalRef(g_bridge);
        g_bridge = nullptr;
        g_connect = nullptr;
        g_sendFrame = nullptr;
        g_disconnect = nullptr;
        g_getState = nullptr;
        g_getLastError = nullptr;
        return false;
    }

    bool ReadBridgeLastErrorLocked(JNIEnv* env, std::string& message) {
        message.clear();
        if (!g_bridge || !g_getLastError) return true;
        LocalRef<jstring> error(
            env, static_cast<jstring>(env->CallObjectMethod(g_bridge, g_getLastError)));
        if (CheckAndClearException(env, "DiscordRpcBridge.getLastError")) return false;
        return ReadJString(env, error.get(), "Read Discord bridge error", message, true);
    }

    // Presence refreshes may retry every 5-15 seconds. Keep the retry behavior
    // unchanged for prompt recovery, but write a repeated connection reason only
    // once until the reason changes or a connection succeeds.
    void LogConnectionFailureLocked(const char* operation) {
        const std::string reason = g_nativeError.empty()
            ? "Discord RPC connection failed without a reported reason"
            : g_nativeError;
        if (reason == g_lastLoggedConnectionError) return;
        g_lastLoggedConnectionError = reason;
        logger.warn("{}: {}", operation, reason);
    }

    void LogProtocolDiagnosticLocked(JNIEnv* env) {
        std::string message;
        if (!ReadBridgeLastErrorLocked(env, message)) return;
        if (message.empty()) {
            g_lastLoggedProtocolError.clear();
            return;
        }
        if (message == g_lastLoggedProtocolError) return;
        g_lastLoggedProtocolError = message;
        logger.warn("Discord RPC protocol reply: {}", message);
    }

    bool EnsureConnectedLocked(JNIEnv* env) {
        if (!LoadBridgeLocked(env)) return false;
        LocalRef<jstring> state(
            env, static_cast<jstring>(env->CallObjectMethod(g_bridge, g_getState)));
        if (!RequireJniValue(env, state.get(), "DiscordRpcBridge.getState")) return false;
        std::string stateText;
        if (!ReadJString(env, state.get(), "Read Discord bridge state", stateText)) return false;
        if (stateText == "READY" || stateText == "CONNECTED" || stateText == "BINDING") {
            // Discord replies arrive asynchronously after sendFrame has already
            // returned. Poll the helper's last diagnostic on the next update so
            // rejected activities appear in the Paper log without making a
            // non-fatal protocol reply tear down a healthy binding.
            LogProtocolDiagnosticLocked(env);
            g_nativeError.clear();
            g_lastLoggedConnectionError.clear();
            return true;
        }

        LocalRef<jstring> version(env, env->NewStringUTF(kRpcVersion));
        if (!RequireJniValue(env, version.get(), "Create Discord RPC version")) return false;
        const bool started = env->CallBooleanMethod(
            g_bridge, g_connect, kApplicationId, version.get());
        if (CheckAndClearException(env, "DiscordRpcBridge.connect")) return false;
        if (!started) {
            // Read the helper's explicit failure only after verifying that the
            // connect JNI call left no pending exception.
            LocalRef<jstring> error(
                env, static_cast<jstring>(env->CallObjectMethod(g_bridge, g_getLastError)));
            if (CheckAndClearException(env, "DiscordRpcBridge.getLastError")) return false;
            std::string message;
            if (ReadJString(env, error.get(), "Read Discord connection error", message, true) &&
                !message.empty()) {
                g_nativeError = std::move(message);
            } else if (g_nativeError.empty()) {
                g_nativeError = "Discord RPC connection was rejected";
            }
            return false;
        }
        g_nativeError.clear();
        g_lastLoggedConnectionError.clear();
        return true;
    }

    bool SendFrame(const nlohmann::json& frame) {
        std::lock_guard<std::mutex> lock(g_jniMutex);
        // Keep the attachment guard alive until after every function-local
        // LocalRef has been destroyed, including on all early-return paths.
        ScopedJniEnv jni;
        JNIEnv* env = jni.get();
        if (!env) {
            LogConnectionFailureLocked("SendFrame could not access JNI");
            return false;
        }
        if (!EnsureConnectedLocked(env)) {
            LogConnectionFailureLocked("SendFrame could not connect to Quest Discord");
            return false;
        }
        // Replace invalid UTF-8 rather than allowing json::dump to throw through
        // a Unity hook because of malformed third-party song metadata.
        const std::string serialized = frame.dump(
            -1, ' ', false, nlohmann::json::error_handler_t::replace);
        LocalRef<jstring> json(env, env->NewStringUTF(serialized.c_str()));
        if (!RequireJniValue(env, json.get(), "Create Discord activity frame")) {
            return false;
        }
        const bool sent = env->CallBooleanMethod(g_bridge, g_sendFrame, json.get());
        const bool failed = CheckAndClearException(env, "DiscordRpcBridge.sendFrame") || !sent;
        if (failed && g_bridge) {
            LocalRef<jstring> error(
                env, static_cast<jstring>(env->CallObjectMethod(g_bridge, g_getLastError)));
            if (!CheckAndClearException(env, "DiscordRpcBridge.getLastError")) {
                std::string message;
                if (ReadJString(env, error.get(), "Read Discord send error", message, true) &&
                    !message.empty()) {
                    g_nativeError = std::move(message);
                }
            }
            if (g_nativeError.empty()) g_nativeError = "Discord RPC rejected the activity frame";
            LogConnectionFailureLocked("DiscordRpcBridge.sendFrame failed");
        } else {
            g_lastLoggedConnectionError.clear();
        }
        return !failed;
    }

    // Serialize Quest IPC on one owned worker. Presence is state, not an event
    // log, so replacing an unsent frame with the newest frame prevents backlog
    // while keeping synchronous Binder work off UnityMain.
    class FrameDispatcher {
    public:
        FrameDispatcher() : worker([this] {
            Run();
        }) {}

        ~FrameDispatcher() {
            // Use explicit stop/join because this Android libc++ target omits
            // std::jthread even though the project otherwise uses C++20.
            {
                std::lock_guard<std::mutex> lock(frameMutex);
                stopping = true;
            }
            frameCv.notify_all();
            if (worker.joinable()) worker.join();
        }

        void Enqueue(nlohmann::json frame) {
            {
                std::lock_guard<std::mutex> lock(frameMutex);
                pendingFrame = std::move(frame);
            }
            frameCv.notify_one();
        }

        void CancelPendingAndWaitForIdle() {
            // Removing pendingFrame alone is not enough: the worker may already
            // have moved that frame into its local variable and be waiting for
            // the JNI mutex. Wait until such an in-flight send finishes before
            // Shutdown clears/disconnects, otherwise the old Quest activity can
            // be published after the user has selected Desktop Companion mode.
            std::unique_lock<std::mutex> lock(frameMutex);
            pendingFrame.reset();
            frameCv.wait(lock, [this] { return !sending; });
        }

    private:
        void Run() noexcept {
            while (true) {
                std::optional<nlohmann::json> frame;
                {
                    std::unique_lock<std::mutex> lock(frameMutex);
                    frameCv.wait(lock, [this] {
                        return stopping || pendingFrame.has_value();
                    });
                    if (stopping && !pendingFrame.has_value()) return;
                    frame = std::move(pendingFrame);
                    pendingFrame.reset();
                    // Publish this state before releasing frameMutex so mode
                    // shutdown cannot mistake a dequeued frame for an idle
                    // dispatcher and race ahead of the actual JNI send.
                    sending = true;
                }

                // SendFrame converts expected JNI/Java failures to false. Keep
                // a final boundary for allocation/serialization failures so no
                // exception can escape the thread entry and call std::terminate.
                try {
                    // SendFrame logs a distinct actionable failure at the JNI
                    // boundary. Do not add a second generic line here on every
                    // refresh; it would hide the useful reason in log noise.
                    if (frame.has_value()) SendFrame(*frame);
                } catch (const std::exception& error) {
                    logger.error("FrameDispatcher::Run dropped an activity safely: {}", error.what());
                } catch (...) {
                    logger.error("FrameDispatcher::Run dropped an activity after a non-standard exception");
                }

                {
                    std::lock_guard<std::mutex> lock(frameMutex);
                    sending = false;
                }
                // Wake a mode-switch callback that is waiting to clear Discord
                // only after no worker frame can still follow that clear.
                frameCv.notify_all();
            }
        }

        std::mutex frameMutex;
        std::condition_variable frameCv;
        std::optional<nlohmann::json> pendingFrame;
        bool sending = false;
        bool stopping = false;
        std::thread worker;
    };

    // Unity-thread-only observer used by Shutdown to avoid constructing the
    // function-local dispatcher merely to cancel an empty queue. The worker
    // never reads or writes this pointer.
    FrameDispatcher* g_frameDispatcher = nullptr;

    // The function-local lifetime guarantees construction only when Quest mode
    // actually sends presence and joins the worker before namespace statics die.
    FrameDispatcher& GetFrameDispatcher() {
        static FrameDispatcher dispatcher;
        g_frameDispatcher = &dispatcher;
        return dispatcher;
    }

    std::string JsonString(const nlohmann::json& object, const char* key) {
        // Presence text fields have a strict string contract. Reject malformed
        // values without operator[] insertion or a throwing conversion.
        if (!object.is_object()) return {};
        const auto value = object.find(key);
        if (value == object.end() || !value->is_string()) return {};
        return value->get_ref<const std::string&>();
    }

    int JsonInt(const nlohmann::json& object, const char* key, int fallback = 0) {
        // Parse and clamp explicitly. std::stoi and json::get conversions throw
        // on malformed/oversized input, which previously relied on a blanket
        // catch inside a frequently called gameplay path.
        if (!object.is_object()) return fallback;
        const auto value = object.find(key);
        if (value == object.end() || value->is_null()) return fallback;
        // nlohmann reports unsigned values as integers too, so test unsigned
        // first to avoid converting a large uint64_t through signed int64_t.
        if (value->is_number_unsigned()) {
            const uint64_t parsed = value->get<uint64_t>();
            return parsed > static_cast<uint64_t>(std::numeric_limits<int>::max())
                ? std::numeric_limits<int>::max()
                : static_cast<int>(parsed);
        }
        if (value->is_number_integer()) {
            const int64_t parsed = value->get<int64_t>();
            return static_cast<int>(std::clamp<int64_t>(
                parsed, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
        }
        if (value->is_number_float()) {
            const double parsed = value->get<double>();
            if (!std::isfinite(parsed)) return fallback;
            return static_cast<int>(std::clamp(
                parsed, static_cast<double>(std::numeric_limits<int>::min()),
                static_cast<double>(std::numeric_limits<int>::max())));
        }
        if (value->is_string()) {
            const auto& text = value->get_ref<const std::string&>();
            int parsed = fallback;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (result.ec == std::errc{} && result.ptr == text.data() + text.size()) return parsed;
        }
        return fallback;
    }

    double JsonDouble(const nlohmann::json& object, const char* key, double fallback = 0.0) {
        // strtod exposes malformed/range failures through return state, avoiding
        // exceptions in the ten-second stat-update path.
        if (!object.is_object()) return fallback;
        const auto value = object.find(key);
        if (value == object.end() || value->is_null()) return fallback;
        if (value->is_number()) {
            const double parsed = value->get<double>();
            return std::isfinite(parsed) ? parsed : fallback;
        }
        if (value->is_string()) {
            const auto& text = value->get_ref<const std::string&>();
            if (text.empty()) return fallback;
            char* end = nullptr;
            errno = 0;
            const double parsed = std::strtod(text.c_str(), &end);
            if (errno == 0 && end == text.c_str() + text.size() && std::isfinite(parsed)) return parsed;
        }
        return fallback;
    }

    std::string Join(const std::vector<std::string>& parts, const std::string& separator) {
        std::ostringstream result;
        bool first = true;
        for (const auto& part : parts) {
            if (part.empty()) continue;
            if (!first) result << separator;
            result << part;
            first = false;
        }
        return result.str();
    }

    std::string JoinMappers(const nlohmann::json& song) {
        if (!song.contains("mappers") || !song["mappers"].is_array()) return {};
        std::set<std::string> unique;
        for (const auto& mapper : song["mappers"]) {
            if (mapper.is_string() && !mapper.get<std::string>().empty()) unique.insert(mapper.get<std::string>());
        }
        return Join(std::vector<std::string>(unique.begin(), unique.end()), ", ");
    }

    std::string SongDetails(const PresenceState& state) {
        std::vector<std::string> songParts;
        if (getConfig().QuestShowSongAuthor.GetValue()) songParts.push_back(JsonString(state.song, "author"));
        if (getConfig().QuestShowSongTitle.GetValue()) songParts.push_back(JsonString(state.song, "title"));
        std::string details = Join(songParts, " - ");
        if (getConfig().QuestShowMappers.GetValue()) {
            const std::string mappers = JoinMappers(state.song);
            if (!mappers.empty()) {
                if (!details.empty()) details += " | ";
                details += "Mapped by " + mappers;
            }
        }
        return details;
    }

    std::string GameplayState(const PresenceState& state) {
        std::vector<std::string> parts;
        const bool multiplayer = state.phase == "MultiplayerPlaying";
        const bool spectating = state.phase == "Spectating";
        if (getConfig().QuestShowStatus.GetValue() && multiplayer) parts.emplace_back("Status: Playing");
        if (getConfig().QuestShowStatus.GetValue() && spectating) parts.emplace_back("Status: Spectating");
        if (getConfig().QuestShowDifficulty.GetValue()) parts.push_back(JsonString(state.song, "difficulty"));
        if (getConfig().QuestShowStatus.GetValue() && multiplayer) parts.emplace_back("Multiplayer");
        if (multiplayer || spectating) return Join(parts, " | ");
        if (getConfig().QuestShowScore.GetValue()) parts.push_back("🎯 " + std::to_string(JsonInt(state.stats, "score")));
        if (getConfig().QuestShowCombo.GetValue()) {
            // Combo is supplied only by the Quest-mode stat sampler. Spell out
            // the label and omit an "x" suffix: Beat Saber uses 1x/2x/4x/8x for
            // its score multiplier, so displaying "8x" here incorrectly makes
            // the successful-cut streak look like that separate value.
            parts.push_back("🔥 Combo: " + std::to_string(JsonInt(state.stats, "combo")));
        }
        if (getConfig().QuestShowMisses.GetValue()) parts.push_back("❌ " + std::to_string(JsonInt(state.stats, "notesMissed")));
        if (getConfig().QuestShowBadCuts.GetValue()) parts.push_back("💥 " + std::to_string(JsonInt(state.stats, "notesBadCut")));
        if (getConfig().QuestShowBombs.GetValue()) parts.push_back("💣 " + std::to_string(JsonInt(state.stats, "bombsHit")));
        return Join(parts, " | ");
    }

    nlohmann::json BuildActivity(const PresenceState& state) {
        nlohmann::json activity;
        activity["name"] = "Beat Saber";
        activity["type"] = 0;

        std::string stateText;
        std::string details;
        if (state.phase == "MainMenu") {
            if (getConfig().QuestShowStatus.GetValue()) stateText = "Status: Main Menu";
        } else if (state.phase == "LevelSelect") {
            if (getConfig().QuestShowStatus.GetValue()) stateText = "Status: Level Selection Menu";
        } else if (state.phase == "Playing" || state.phase == "MultiplayerPlaying" || state.phase == "Spectating") {
            stateText = GameplayState(state);
            details = SongDetails(state);
        } else if (state.phase == "Paused") {
            if (getConfig().QuestShowStatus.GetValue()) stateText = "Level Paused";
            details = SongDetails(state);
        } else if (state.phase == "Cleared" || state.phase == "Failed") {
            std::vector<std::string> parts;
            if (getConfig().QuestShowStatus.GetValue()) {
                parts.push_back(state.phase == "Cleared" ? "Status: Cleared" : "Status: Failed");
            }
            if (getConfig().QuestShowDifficulty.GetValue()) parts.push_back(JsonString(state.song, "difficulty"));
            stateText = Join(parts, " | ");
            details = SongDetails(state);
        } else if (state.phase == "Lobby") {
            if (getConfig().QuestShowStatus.GetValue()) details = "Status: Multiplayer Lobby";
            if (getConfig().QuestShowMultiplayer.GetValue()) {
                stateText = std::to_string(state.playerCount) + " players waiting...";
            }
        }

        if (!stateText.empty()) activity["state"] = stateText;
        if (!details.empty()) activity["details"] = details;

        const bool timedPhase = state.phase == "Playing" || state.phase == "MultiplayerPlaying" || state.phase == "Spectating";
        if (timedPhase && getConfig().QuestShowTimer.GetValue() && state.songStart > 0 && state.songEnd > state.songStart) {
            activity["timestamps"] = {{"start", state.songStart}, {"end", state.songEnd}};
        }

        nlohmann::json assets;
        const bool coverPhase = state.phase == "Playing" || state.phase == "MultiplayerPlaying" ||
            state.phase == "Spectating" || state.phase == "Paused" || state.phase == "Failed";
        if (coverPhase && getConfig().QuestShowCoverArt.GetValue()) {
            const std::string cover = JsonString(state.song, "coverURL");
            if (!cover.empty() && cover != "null") assets["large_image"] = cover;
        }
        if (getConfig().QuestShowPlatform.GetValue()) {
            assets["small_image"] = "quest";
            assets["small_text"] = "Meta Quest";
        }
        if (!assets.empty()) activity["assets"] = assets;

        if (state.phase == "Lobby" && getConfig().QuestShowMultiplayer.GetValue()) {
            const bool hasValidParty = !state.partyId.empty() && state.maxPlayerCount > 0;
            if (hasValidParty) {
                activity["party"] = {
                    {"id", state.partyId},
                    {"size", {state.playerCount, state.maxPlayerCount}}
                };
                // Discord validates join secrets as part of a party. Emitting a
                // secret by itself causes SET_ACTIVITY to be rejected, so keep
                // both fields governed by the same validity condition.
                if (!state.lobbyCode.empty()) {
                    activity["secrets"] = {{"join", "bsrpc://BeatTogether/" + state.lobbyCode}};
                }
            }
        }
        return activity;
    }

    void SendCurrentLocked() {
        nlohmann::json frame = {
            {"cmd", "SET_ACTIVITY"},
            {"args", {
                {"pid", getpid()},
                {"activity", BuildActivity(g_state)}
            }},
            {"nonce", std::to_string(g_nonce.fetch_add(1))}
        };
        // Enqueue while holding only the short-lived state lock; the dispatcher
        // performs JNI and Binder work after this gameplay callback returns.
        try {
            GetFrameDispatcher().Enqueue(std::move(frame));
            logger.debug("Quest Discord activity queued: {}", g_state.phase);
        } catch (const std::exception& error) {
            // Native thread creation can fail on a memory-constrained Quest.
            // Drop this optional update rather than unwinding through a hook.
            logger.error("SendCurrentLocked could not queue the Quest Discord activity: {}", error.what());
        } catch (...) {
            logger.error("SendCurrentLocked could not queue the Quest Discord activity due to a non-standard exception");
        }
    }

    void StartSong(PresenceState& state, const nlohmann::json& event, const std::string& phase) {
        state.phase = phase;
        state.song = event;
        state.stats = nlohmann::json::object();
        state.songLength = std::max(0, JsonInt(event, "duration"));
        state.songStart = std::time(nullptr);
        state.songEnd = state.songStart + state.songLength;
        state.pauseStart = 0;
    }
}

namespace QuestDiscord {
    bool Initialize() {
        std::lock_guard<std::mutex> lock(g_jniMutex);
        // Use the same lifetime rule on the Unity path. It is normally already
        // attached, but the guard also keeps future call sites safe if that ever
        // changes.
        ScopedJniEnv jni;
        JNIEnv* env = jni.get();
        // Initialize is called from a Unity/BSML callback. Capture its app class
        // loader before any native worker is asked to load the helper DEX.
        const bool result = env && CacheApplicationContextLocked(env) && EnsureConnectedLocked(env);
        if (result) logger.info("Quest Discord RPC binding started");
        else logger.error("Quest Discord RPC initialization failed: {}", g_nativeError);
        return result;
    }

    void Shutdown() {
        // Clear the activity while the bridge is still connected. This path is
        // intentionally synchronous because immediately disconnecting first
        // would race and discard the final clear frame.
        // Do not construct a worker just to shut down a bridge that never sent
        // an activity (for example, when initialization failed at startup).
        if (g_frameDispatcher) g_frameDispatcher->CancelPendingAndWaitForIdle();
        bool hasLoadedBridge = false;
        {
            std::lock_guard<std::mutex> lock(g_jniMutex);
            hasLoadedBridge = g_bridge != nullptr;
        }
        if (hasLoadedBridge) {
            nlohmann::json clearFrame = {
                {"cmd", "SET_ACTIVITY"},
                {"args", {{"pid", getpid()}, {"activity", nullptr}}},
                {"nonce", std::to_string(g_nonce.fetch_add(1))}
            };
            SendFrame(clearFrame);
        }

        std::lock_guard<std::mutex> lock(g_jniMutex);
        if (!g_bridge) return;
        ScopedJniEnv jni;
        JNIEnv* env = jni.get();
        if (env) {
            if (g_disconnect) {
                env->CallVoidMethod(g_bridge, g_disconnect);
                CheckAndClearException(env, "DiscordRpcBridge.disconnect");
            }

            // Release the global reference and method cache so reconnecting
            // always starts from a complete, internally consistent bridge.
            env->DeleteGlobalRef(g_bridge);
            g_bridge = nullptr;
            g_connect = nullptr;
            g_sendFrame = nullptr;
            g_disconnect = nullptr;
            g_getState = nullptr;
            g_getLastError = nullptr;
            // Keep g_appClassLoader/g_activityContext for the Beat Saber process
            // lifetime. They are tiny global references and are required for a
            // later Quest-mode reconnect from the native dispatcher; deleting
            // them here would recreate the class-loader failure fixed above.
        }
        logger.info("Quest Discord RPC disconnected");
    }

    void HandleEvent(const nlohmann::json& event) {
        const std::string type = JsonString(event, "type");
        if (type.empty() || type == "HeartbeatReceiver") return;

        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (type == "MainMenuInitialized") {
            g_state.phase = "MainMenu";
        } else if (type == "LevelSelectionMenuInitialized") {
            g_state.phase = "LevelSelect";
        } else if (type == "BeatmapInitialized") {
            StartSong(g_state, event, "Playing");
        } else if (type == "BeatmapCoverResolved") {
            // A custom song is published immediately, before BeatSaver returns.
            // Apply only the late cover URL so the network response cannot reset
            // elapsed time, live stats, or a Paused state.
            if (event.contains("coverURL") && !event["coverURL"].is_null()) {
                g_state.song["coverURL"] = event["coverURL"];
            }
        } else if (type == "MultiplayerBeatmapInitialized") {
            StartSong(g_state, event, "MultiplayerPlaying");
            g_state.partyId.clear();
        } else if (type == "SpectateInitialized") {
            StartSong(g_state, event, "Spectating");
        } else if (type == "BeatmapStatUpdate") {
            if (g_state.phase != "Playing" && g_state.phase != "MultiplayerPlaying" && g_state.phase != "Spectating") return;
            g_state.stats = event;
            const double currentTime = JsonDouble(event, "currentTime", -1.0);
            if (currentTime >= 0.0 && g_state.songLength > 0) {
                g_state.songStart = std::time(nullptr) - static_cast<std::time_t>(currentTime);
                g_state.songEnd = g_state.songStart + g_state.songLength;
            }
        } else if (type == "BeatmapPaused") {
            g_state.phase = "Paused";
            g_state.pauseStart = std::time(nullptr);
        } else if (type == "BeatmapResumed") {
            if (g_state.pauseStart > 0) {
                const std::time_t pausedFor = std::time(nullptr) - g_state.pauseStart;
                g_state.songStart += pausedFor;
                g_state.songEnd += pausedFor;
            }
            g_state.pauseStart = 0;
            g_state.phase = "Playing";
        } else if (type == "BeatmapRestarted") {
            g_state.phase = "Playing";
            g_state.stats = nlohmann::json::object();
            g_state.songStart = std::time(nullptr);
            g_state.songLength = std::max(g_state.songLength, JsonInt(event, "duration"));
            g_state.songEnd = g_state.songStart + g_state.songLength;
            g_state.pauseStart = 0;
        } else if (type == "BeatmapCleared") {
            for (const char* key : {"title", "author", "duration", "mappers", "difficulty", "coverURL"}) {
                if (event.contains(key) && !event[key].is_null()) g_state.song[key] = event[key];
            }
            g_state.phase = "Cleared";
        } else if (type == "BeatmapFailed") {
            g_state.phase = "Failed";
        } else if (type == "LobbyPlayerOnConnect" || type == "LobbyPlayerOnDisconnect" ||
                   type == "LobbyLocalPlayerOnConnect") {
            // Treat the local player's first connection as a real lobby event;
            // otherwise solo waiting lobbies never acquire party presence.
            g_state.phase = "Lobby";
            g_state.playerCount = JsonInt(event, "playerCount");
            g_state.maxPlayerCount = JsonInt(event, "maxPlayerCount");
            g_state.lobbyCode = JsonString(event, "lobbyCode");
            if (g_state.partyId.empty()) g_state.partyId = "party_" + std::to_string(std::time(nullptr));
        } else if (type == "LobbyLocalPlayerOnDisconnect") {
            // Local disconnect ends the lobby even if no remote-player callback
            // follows, so discard join secrets and party identifiers promptly.
            g_state.phase = "MainMenu";
            g_state.playerCount = 0;
            g_state.maxPlayerCount = 0;
            g_state.lobbyCode.clear();
            g_state.partyId.clear();
        } else if (type == "MultiplayerBeatmapFinished") {
            // Multiplayer results previously left Discord showing a running song
            // forever because this event was silently ignored.
            g_state.phase = "Lobby";
        } else {
            return;
        }
        SendCurrentLocked();
    }

    void Refresh() {
        if (!getConfig().UseQuestDiscord.GetValue()) return;
        std::lock_guard<std::mutex> lock(g_stateMutex);
        SendCurrentLocked();
    }

    std::string GetConnectionStatus() {
        std::lock_guard<std::mutex> lock(g_jniMutex);
        if (!g_bridge) return g_nativeError.empty() ? "Not connected" : g_nativeError;
        // The status path owns jstring LocalRefs too. Keeping attachment cleanup
        // in this guard prevents the same detach-before-destruction crash if a
        // future caller queries status from a native-created thread.
        ScopedJniEnv jni;
        JNIEnv* env = jni.get();
        if (!env) return g_nativeError;
        // Check each Java call before decoding its result. Reading a jstring
        // while a Java exception is pending can crash inside the JNI runtime.
        LocalRef<jstring> state(
            env, static_cast<jstring>(env->CallObjectMethod(g_bridge, g_getState)));
        if (CheckAndClearException(env, "DiscordRpcBridge.getState")) {
            return g_nativeError;
        }
        std::string result;
        if (!ReadJString(env, state.get(), "Read Discord bridge state", result)) {
            return g_nativeError;
        }
        if (result == "ERROR" || result == "CLOSED") {
            LocalRef<jstring> error(
                env, static_cast<jstring>(env->CallObjectMethod(g_bridge, g_getLastError)));
            if (!CheckAndClearException(env, "DiscordRpcBridge.getLastError")) {
                std::string message;
                if (ReadJString(env, error.get(), "Read Discord status error", message, true) &&
                    !message.empty()) {
                    result += ": " + message;
                }
            }
        }
        return result;
    }

    bool IsHelperAvailable() {
        // access() is a read-only packaging diagnostic used by the startup log;
        // it does not load the DEX or initiate a Discord service binding.
        return access(kHelperSource, R_OK) == 0;
    }
}
