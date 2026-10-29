// OpenXR API layer for Condor 3.
//
// Step 1: turns on XR_EXT_hand_tracking behind Condor's back and logs hand joints,
// expressed in the reference space of Condor's own projection layer (= cockpit space).

#include <windows.h>
#include <d3d11.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"

namespace {

constexpr const char* kLayerName = "XR_APILAYER_CONDOR3VRCOCKPIT_hands";
constexpr XrDuration kLogPeriod = 1'000'000'000; // 1 s, in ns

// The layer is implicit, so the loader puts it in every 32-bit OpenXR app.
// It only does anything when the host process is Condor.
bool gActive = false;
bool gHandTrackingEnabled = false;

XrInstance gInstance = XR_NULL_HANDLE;
XrSession gSession = XR_NULL_HANDLE;
XrHandTrackerEXT gHands[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
XrTime gLastLogTime = 0;
bool gLoggedFirstFrame = false;

PFN_xrGetInstanceProcAddr nextGetInstanceProcAddr = nullptr;
PFN_xrDestroyInstance nextDestroyInstance = nullptr;
PFN_xrGetSystemProperties nextGetSystemProperties = nullptr;
PFN_xrCreateSession nextCreateSession = nullptr;
PFN_xrDestroySession nextDestroySession = nullptr;
PFN_xrEndFrame nextEndFrame = nullptr;
PFN_xrCreateHandTrackerEXT nextCreateHandTrackerEXT = nullptr;
PFN_xrDestroyHandTrackerEXT nextDestroyHandTrackerEXT = nullptr;
PFN_xrLocateHandJointsEXT nextLocateHandJointsEXT = nullptr;

template <typename T>
void Resolve(const char* name, T& function) {
    if (XR_FAILED(nextGetInstanceProcAddr(gInstance, name, reinterpret_cast<PFN_xrVoidFunction*>(&function)))) {
        function = nullptr;
    }
}

bool IsCondorProcess() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* exe = wcsrchr(path, L'\\');
    exe = exe ? exe + 1 : path;
    return _wcsicmp(exe, L"Condor.exe") == 0;
}

bool RuntimeSupportsExtension(const char* extension) {
    PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
    if (XR_FAILED(nextGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                                          reinterpret_cast<PFN_xrVoidFunction*>(&enumerate)))) {
        return false;
    }
    uint32_t count = 0;
    if (XR_FAILED(enumerate(nullptr, 0, &count, nullptr))) {
        return false;
    }
    std::vector<XrExtensionProperties> properties(count, {XR_TYPE_EXTENSION_PROPERTIES});
    if (XR_FAILED(enumerate(nullptr, count, &count, properties.data()))) {
        return false;
    }
    return std::any_of(properties.begin(), properties.end(),
                       [&](const XrExtensionProperties& p) { return strcmp(p.extensionName, extension) == 0; });
}

float Distance(const XrVector3f& a, const XrVector3f& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void DestroyHandTrackers() {
    for (XrHandTrackerEXT& hand : gHands) {
        if (hand != XR_NULL_HANDLE) {
            nextDestroyHandTrackerEXT(hand);
            hand = XR_NULL_HANDLE;
        }
    }
}

void CreateHandTrackers(const XrSessionCreateInfo* createInfo) {
    XrSystemHandTrackingPropertiesEXT handProperties{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES, &handProperties};
    if (XR_SUCCEEDED(nextGetSystemProperties(gInstance, createInfo->systemId, &systemProperties))) {
        Log("System: %s, supportsHandTracking=%d", systemProperties.systemName, handProperties.supportsHandTracking);
        if (!handProperties.supportsHandTracking) {
            Log("Hand tracking unavailable. On Quest Link, enable 'Developer runtime features' in the Meta Horizon Link app.");
            return;
        }
    }
    const XrHandEXT sides[2] = {XR_HAND_LEFT_EXT, XR_HAND_RIGHT_EXT};
    for (int i = 0; i < 2; ++i) {
        XrHandTrackerCreateInfoEXT info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        info.hand = sides[i];
        info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        const XrResult result = nextCreateHandTrackerEXT(gSession, &info, &gHands[i]);
        Log("xrCreateHandTrackerEXT(%s) -> %d", i == 0 ? "left" : "right", result);
    }
}

void LogFirstFrame(const XrFrameEndInfo* frameEndInfo) {
    Log("First frame: %u layer(s), blend mode %d", frameEndInfo->layerCount, frameEndInfo->environmentBlendMode);
    for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* layer = frameEndInfo->layers[i];
        Log("  layer %u: type %d, flags 0x%llx", i, layer->type, static_cast<unsigned long long>(layer->layerFlags));
    }
}

void LogHands(const XrFrameEndInfo* frameEndInfo) {
    const XrCompositionLayerProjection* projection = nullptr;
    for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
        if (frameEndInfo->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            projection = reinterpret_cast<const XrCompositionLayerProjection*>(frameEndInfo->layers[i]);
            break;
        }
    }
    if (!projection || projection->viewCount == 0) {
        return;
    }

    // Eye position, to see where the pilot's head sits in Condor's space.
    const XrVector3f& eye = projection->views[0].pose.position;
    Log("head  (%+.3f %+.3f %+.3f)", eye.x, eye.y, eye.z);

    for (int i = 0; i < 2; ++i) {
        if (gHands[i] == XR_NULL_HANDLE) {
            continue;
        }
        XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
        XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
        locations.jointLocations = joints;
        XrHandJointsLocateInfoEXT locateInfo{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        locateInfo.baseSpace = projection->space;
        locateInfo.time = frameEndInfo->displayTime;
        const XrResult result = nextLocateHandJointsEXT(gHands[i], &locateInfo, &locations);
        const char* side = i == 0 ? "left " : "right";
        if (XR_FAILED(result) || !locations.isActive) {
            Log("%s not tracked (%d)", side, result);
            continue;
        }
        const XrVector3f& palm = joints[XR_HAND_JOINT_PALM_EXT].pose.position;
        const float pinch = Distance(joints[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position,
                                     joints[XR_HAND_JOINT_INDEX_TIP_EXT].pose.position);
        Log("%s palm (%+.3f %+.3f %+.3f)  pinch %.3f m", side, palm.x, palm.y, palm.z, pinch);
    }
}

XrResult XRAPI_CALL HookDestroyInstance(XrInstance instance) {
    Log("xrDestroyInstance");
    gInstance = XR_NULL_HANDLE;
    return nextDestroyInstance(instance);
}

XrResult XRAPI_CALL HookCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session) {
    const XrResult result = nextCreateSession(instance, createInfo, session);
    Log("xrCreateSession -> %d", result);
    if (XR_FAILED(result)) {
        return result;
    }
    for (auto* entry = static_cast<const XrBaseInStructure*>(createInfo->next); entry; entry = entry->next) {
        Log("  graphics binding type %d%s", entry->type,
            entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR ? " (D3D11)" : "");
    }
    gSession = *session;
    gLoggedFirstFrame = false;
    if (gHandTrackingEnabled) {
        CreateHandTrackers(createInfo);
    }
    return result;
}

XrResult XRAPI_CALL HookDestroySession(XrSession session) {
    Log("xrDestroySession");
    if (session == gSession) {
        DestroyHandTrackers();
        gSession = XR_NULL_HANDLE;
    }
    return nextDestroySession(session);
}

XrResult XRAPI_CALL HookEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
    if (session == gSession && frameEndInfo) {
        if (!gLoggedFirstFrame) {
            LogFirstFrame(frameEndInfo);
            gLoggedFirstFrame = true;
        }
        if (frameEndInfo->displayTime - gLastLogTime >= kLogPeriod) {
            gLastLogTime = frameEndInfo->displayTime;
            LogHands(frameEndInfo);
        }
    }
    return nextEndFrame(session, frameEndInfo);
}

XrResult XRAPI_CALL LayerGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);

struct Hook {
    const char* name;
    PFN_xrVoidFunction function;
};

const Hook kHooks[] = {
    {"xrGetInstanceProcAddr", reinterpret_cast<PFN_xrVoidFunction>(LayerGetInstanceProcAddr)},
    {"xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction>(HookDestroyInstance)},
    {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(HookCreateSession)},
    {"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(HookDestroySession)},
    {"xrEndFrame", reinterpret_cast<PFN_xrVoidFunction>(HookEndFrame)},
};

XrResult XRAPI_CALL LayerGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!nextGetInstanceProcAddr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (gActive) {
        for (const Hook& hook : kHooks) {
            if (strcmp(name, hook.name) == 0) {
                *function = hook.function;
                return XR_SUCCESS;
            }
        }
    }
    return nextGetInstanceProcAddr(instance, name, function);
}

XrResult XRAPI_CALL LayerCreateApiLayerInstance(const XrInstanceCreateInfo* createInfo,
                                                const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
    if (!layerInfo || layerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        !layerInfo->nextInfo || layerInfo->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        strcmp(layerInfo->nextInfo->layerName, kLayerName) != 0) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    nextGetInstanceProcAddr = layerInfo->nextInfo->nextGetInstanceProcAddr;

    XrApiLayerCreateInfo chainInfo = *layerInfo;
    chainInfo.nextInfo = layerInfo->nextInfo->next;

    gActive = IsCondorProcess();
    if (!gActive) {
        return layerInfo->nextInfo->nextCreateApiLayerInstance(createInfo, &chainInfo, instance);
    }

    Log("%s loaded in %s (engine %s)", kLayerName, createInfo->applicationInfo.applicationName,
        createInfo->applicationInfo.engineName);

    std::vector<const char*> extensions(createInfo->enabledExtensionNames,
                                        createInfo->enabledExtensionNames + createInfo->enabledExtensionCount);
    for (const char* extension : extensions) {
        Log("  app extension: %s", extension);
    }
    const bool appRequested = std::any_of(extensions.begin(), extensions.end(), [](const char* e) {
        return strcmp(e, XR_EXT_HAND_TRACKING_EXTENSION_NAME) == 0;
    });
    if (appRequested) {
        gHandTrackingEnabled = true;
    } else if (RuntimeSupportsExtension(XR_EXT_HAND_TRACKING_EXTENSION_NAME)) {
        extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        gHandTrackingEnabled = true;
    }
    Log("XR_EXT_hand_tracking %s", gHandTrackingEnabled ? "enabled" : "NOT supported by runtime");

    XrInstanceCreateInfo modifiedInfo = *createInfo;
    modifiedInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    modifiedInfo.enabledExtensionNames = extensions.data();

    const XrResult result = layerInfo->nextInfo->nextCreateApiLayerInstance(&modifiedInfo, &chainInfo, instance);
    Log("xrCreateInstance -> %d", result);
    if (XR_FAILED(result)) {
        gActive = false;
        return result;
    }

    gInstance = *instance;
    Resolve("xrDestroyInstance", nextDestroyInstance);
    Resolve("xrGetSystemProperties", nextGetSystemProperties);
    Resolve("xrCreateSession", nextCreateSession);
    Resolve("xrDestroySession", nextDestroySession);
    Resolve("xrEndFrame", nextEndFrame);
    if (gHandTrackingEnabled) {
        Resolve("xrCreateHandTrackerEXT", nextCreateHandTrackerEXT);
        Resolve("xrDestroyHandTrackerEXT", nextDestroyHandTrackerEXT);
        Resolve("xrLocateHandJointsEXT", nextLocateHandJointsEXT);
        gHandTrackingEnabled = nextCreateHandTrackerEXT && nextDestroyHandTrackerEXT && nextLocateHandJointsEXT;
    }
    return result;
}

} // namespace

extern "C" XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                                                  const char* /*layerName*/,
                                                                  XrNegotiateApiLayerRequest* request) {
    if (!loaderInfo || !request || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    // Condor's loader may be older than our headers; never claim more than it knows.
    request->layerApiVersion = std::min<XrVersion>(XR_CURRENT_API_VERSION, loaderInfo->maxApiVersion);
    request->getInstanceProcAddr = LayerGetInstanceProcAddr;
    request->createApiLayerInstance = LayerCreateApiLayerInstance;
    return XR_SUCCESS;
}
