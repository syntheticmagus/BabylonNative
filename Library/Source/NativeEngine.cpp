#include "NativeEngine.h"

#include "RuntimeImpl.h"
#include "NapiBridge.h"
#include "ShaderCompiler.h"

#define XR_USE_GRAPHICS_API_D3D11
#define XR_DO(OPERATION) do { XrResult result = OPERATION; if (result != XrResult::XR_SUCCESS) return result; } while (false)
#define XR_CHECK(OPERATION) do { XrResult result = OPERATION; if (XR_FAILED(result)) throw std::exception{}; } while (false)
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#ifndef WIN32
#include <alloca.h>
#define alloca(size)   __builtin_alloca(size)
#endif
// TODO: this needs to be fixed in bgfx
namespace bgfx
{
    uint16_t attribToId(Attrib::Enum _attr);
}

#define BGFX_UNIFORM_FRAGMENTBIT UINT8_C(0x10) // Copy-pasta from bgfx_p.h
#define BGFX_UNIFORM_SAMPLERBIT  UINT8_C(0x20) // Copy-pasta from bgfx_p.h

#include <bimg/bimg.h>
#include <bimg/decode.h>
#include <bimg/encode.h>

#include <bx/math.h>
#include <bx/readerwriter.h>

#include <queue>
#include <regex>
#include <sstream>

namespace
{
    // Choppied and copied out of the OpenXR VS SDK
    struct OpenXR
    {
        // ------------------------------ Start CreateInstance ----------------------------
        std::vector<const char*> SelectExtensions() {
            // Fetch the list of extensions supported by the runtime.
            uint32_t extensionCount;
            XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));
            std::vector<XrExtensionProperties> extensionProperties(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
            XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensionProperties.data()));

            std::vector<const char*> enabledExtensions;

            // Add a specific extension to the list of extensions to be enabled, if it is supported.
            auto EnableExtentionIfSupported = [&](const char* extensionName) {
                for (uint32_t i = 0; i < extensionCount; i++) {
                    if (strcmp(extensionProperties[i].extensionName, extensionName) == 0) {
                        enabledExtensions.push_back(extensionName);
                        return true;
                    }
                }
                return false;
            };

            // D3D11 extension is required for this sample, so check if it's supported.
            assert(EnableExtentionIfSupported(XR_KHR_D3D11_ENABLE_EXTENSION_NAME));

            // Additional optional extensions for enhanced functionality. Track whether enabled in m_optionalExtensions.
            m_optionalExtensions.DepthExtensionSupported = EnableExtentionIfSupported(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
            m_optionalExtensions.UnboundedRefSpaceSupported = EnableExtentionIfSupported(XR_MSFT_UNBOUNDED_REFERENCE_SPACE_EXTENSION_NAME);
            m_optionalExtensions.SpatialAnchorSupported = EnableExtentionIfSupported(XR_MSFT_SPATIAL_ANCHOR_EXTENSION_NAME);

            return enabledExtensions;
        }
        void CreateInstance()
        {
            assert(m_instance == XR_NULL_HANDLE);

            // Build out the extensions to enable. Some extensions are required and some are optional.
            const std::vector<const char*> enabledExtensions = SelectExtensions();

            // Create the instance with desired extensions.
            XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
            createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
            createInfo.enabledExtensionNames = enabledExtensions.data();

            createInfo.applicationInfo = { "", 1, "OpenXR Sample", 1, XR_CURRENT_API_VERSION };
            strcpy_s(createInfo.applicationInfo.applicationName, m_applicationName.c_str());
            XR_CHECK(xrCreateInstance(&createInfo, &m_instance));
        }
        // ------------------------------ End CreateInstance ----------------------------
        // ------------------------------ Start InitializeSystem ------------------------------
        void InitializeSystem() {
            assert(m_instance != XR_NULL_HANDLE);
            assert(m_systemId == XR_NULL_SYSTEM_ID);

            XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
            systemInfo.formFactor = FORM_FACTOR;
            while (true) {
                XrResult result = xrGetSystem(m_instance, &systemInfo, &m_systemId);
                if (XR_SUCCEEDED(result)) {
                    break;
                }
                else if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
                    // DEBUG_PRINT("No headset detected.  Trying again in one second...");
                    using namespace std::chrono_literals;
                    std::this_thread::sleep_for(1s);
                }
                else {
                    // CHECK_XRRESULT(result, "xrGetSystem");
                    throw std::exception{}; // TODO
                }
            };

            // Choose an environment blend mode.
            {
                // Query the list of supported environment blend modes for the current system
                uint32_t count;
                XR_CHECK(xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, VIEW_CONFIGURATION_TYPE, 0, &count, nullptr));
                assert(count > 0); // A system must support at least one environment blend mode.

                std::vector<XrEnvironmentBlendMode> environmentBlendModes(count);
                XR_CHECK(xrEnumerateEnvironmentBlendModes(
                    m_instance, m_systemId, VIEW_CONFIGURATION_TYPE, count, &count, environmentBlendModes.data()));

                // This sample supports all modes, pick the system's preferred one.
                m_environmentBlendMode = environmentBlendModes[0];
            }

            // Choose a reasonable depth range can help improve hologram visual quality.
            // Use reversed Z (near > far) for more uniformed Z resolution.
            m_near = 20.f;
            m_far = 0.1f;
        }
        // ------------------------------ End InitializeSystem ------------------------------
        void InitializeSession() {
            assert(m_instance != XR_NULL_HANDLE);
            assert(m_systemId != XR_NULL_SYSTEM_ID);
            assert(m_session == XR_NULL_HANDLE);

            // Create the D3D11 device for the adapter associated with the system.
            XrGraphicsRequirementsD3D11KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
            XR_CHECK(xrGetD3D11GraphicsRequirementsKHR(m_instance, m_systemId, &graphicsRequirements));

            // Create a list of feature levels which are both supported by the OpenXR runtime and this application.
            std::vector<D3D_FEATURE_LEVEL> featureLevels = { D3D_FEATURE_LEVEL_12_1,
                                                            D3D_FEATURE_LEVEL_12_0,
                                                            D3D_FEATURE_LEVEL_11_1,
                                                            D3D_FEATURE_LEVEL_11_0,
                                                            D3D_FEATURE_LEVEL_10_1,
                                                            D3D_FEATURE_LEVEL_10_0 };
            featureLevels.erase(std::remove_if(featureLevels.begin(),
                featureLevels.end(),
                [&](D3D_FEATURE_LEVEL fl) { return fl < graphicsRequirements.minFeatureLevel; }),
                featureLevels.end());
            assert(featureLevels.size() != 0); // "Unsupported minimum feature level!"

            ID3D11Device* device = nullptr;//TODO m_graphicsPlugin->InitializeDevice(graphicsRequirements.adapterLuid, featureLevels);

            XrGraphicsBindingD3D11KHR graphicsBinding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
            graphicsBinding.device = device;

            XrSessionCreateInfo createInfo{ XR_TYPE_SESSION_CREATE_INFO };
            createInfo.next = &graphicsBinding;
            createInfo.systemId = m_systemId;
            XR_CHECK(xrCreateSession(m_instance, &createInfo, &m_session));

            CreateSpaces();
            CreateSwapchains();
        }

        void CreateSpaces() {
            assert(m_session != XR_NULL_HANDLE);

            // Create a space to place a cube in the world.
            {
                if (m_optionalExtensions.UnboundedRefSpaceSupported) {
                    // Unbounded reference space provides the best scene space for world-scale experiences.
                    m_sceneSpaceType = XR_REFERENCE_SPACE_TYPE_UNBOUNDED_MSFT;
                }
                else {
                    // If running on a platform that does not support world-scale experiences, fall back to local space.
                    m_sceneSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
                }

                XrReferenceSpaceCreateInfo spaceCreateInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
                spaceCreateInfo.referenceSpaceType = m_sceneSpaceType;
                // spaceCreateInfo.poseInReferenceSpace = xr::math::Pose::Identity();
                XR_CHECK(xrCreateReferenceSpace(m_session, &spaceCreateInfo, &m_sceneSpace));
            }
        }

        std::tuple<DXGI_FORMAT, DXGI_FORMAT> SelectSwapchainPixelFormats()
        {
            assert(m_session != XR_NULL_HANDLE);

            // Query runtime preferred swapchain formats.
            uint32_t swapchainFormatCount;
            XR_CHECK(xrEnumerateSwapchainFormats(m_session, 0, &swapchainFormatCount, nullptr));

            std::vector<int64_t> swapchainFormats(swapchainFormatCount);
            XR_CHECK(xrEnumerateSwapchainFormats(
                m_session, (uint32_t)swapchainFormats.size(), &swapchainFormatCount, swapchainFormats.data()));

            // Choose the first runtime preferred format that this app supports.
            auto SelectPixelFormat = [](const std::vector<int64_t>& runtimePreferredFormats,
                const std::vector<DXGI_FORMAT>& applicationSupportedFormats) {
                auto found = std::find_first_of(std::begin(runtimePreferredFormats),
                    std::end(runtimePreferredFormats),
                    std::begin(applicationSupportedFormats),
                    std::end(applicationSupportedFormats));
                if (found == std::end(runtimePreferredFormats)) {
                    throw std::exception{ "No runtime swapchain format is supported." };
                }
                return (DXGI_FORMAT)* found;
            };

            // DXGI_FORMAT colorSwapchainFormat = SelectPixelFormat(swapchainFormats, m_graphicsPlugin->SupportedColorFormats());
            // DXGI_FORMAT depthSwapchainFormat = SelectPixelFormat(swapchainFormats, m_graphicsPlugin->SupportedDepthFormats());

            // return { colorSwapchainFormat, depthSwapchainFormat };
        }

        void CreateSwapchains()
        {
            assert(m_session != XR_NULL_HANDLE);
            assert(m_renderResources == nullptr);

            m_renderResources = std::make_unique<RenderResources>();

            // Read graphics properties for preferred swapchain length and logging.
            XrSystemProperties systemProperties{ XR_TYPE_SYSTEM_PROPERTIES };
            XR_CHECK(xrGetSystemProperties(m_instance, m_systemId, &systemProperties));

            // Select color and depth swapchain pixel formats
            const auto [colorSwapchainFormat, depthSwapchainFormat] = SelectSwapchainPixelFormats();

            // Query and cache view configuration views.
            uint32_t viewCount;
            XR_CHECK(xrEnumerateViewConfigurationViews(m_instance, m_systemId, VIEW_CONFIGURATION_TYPE, 0, &viewCount, nullptr));
            assert(viewCount == STEREO_VIEW_COUNT);

            m_renderResources->ConfigViews.resize(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
            XR_CHECK(xrEnumerateViewConfigurationViews(
                m_instance, m_systemId, VIEW_CONFIGURATION_TYPE, viewCount, &viewCount, m_renderResources->ConfigViews.data()));

            // Using texture array for better performance, but requiring left/right views have identical sizes.
            const XrViewConfigurationView& view = m_renderResources->ConfigViews[0];
            assert(m_renderResources->ConfigViews[0].recommendedImageRectWidth ==
                m_renderResources->ConfigViews[1].recommendedImageRectWidth);
            assert(m_renderResources->ConfigViews[0].recommendedImageRectHeight ==
                m_renderResources->ConfigViews[1].recommendedImageRectHeight);
            assert(m_renderResources->ConfigViews[0].recommendedSwapchainSampleCount ==
                m_renderResources->ConfigViews[1].recommendedSwapchainSampleCount);

            // Create swapchains with texture array for color and depth images.
            // The texture array has the size of viewCount, and they are rendered in a single pass using VPRT.
            const uint32_t textureArraySize = viewCount;
            m_renderResources->ColorSwapchain =
                CreateSwapchainD3D11(m_session,
                    colorSwapchainFormat,
                    view.recommendedImageRectWidth,
                    view.recommendedImageRectHeight,
                    textureArraySize,
                    view.recommendedSwapchainSampleCount,
                    0,
                    XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);

            m_renderResources->DepthSwapchain =
                CreateSwapchainD3D11(m_session,
                    depthSwapchainFormat,
                    view.recommendedImageRectWidth,
                    view.recommendedImageRectHeight,
                    textureArraySize,
                    view.recommendedSwapchainSampleCount,
                    0,
                    XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);

            // Preallocate view buffers for xrLocateViews later inside frame loop.
            m_renderResources->Views.resize(viewCount, { XR_TYPE_VIEW });
        }

        struct SwapchainD3D11;
        SwapchainD3D11 CreateSwapchainD3D11(XrSession session,
            DXGI_FORMAT format,
            int32_t width,
            int32_t height,
            uint32_t arraySize,
            uint32_t sampleCount,
            XrSwapchainCreateFlags createFlags,
            XrSwapchainUsageFlags usageFlags) {
            SwapchainD3D11 swapchain;
            swapchain.Format = format;
            swapchain.Width = width;
            swapchain.Height = height;
            swapchain.ArraySize = arraySize;

            XrSwapchainCreateInfo swapchainCreateInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            swapchainCreateInfo.arraySize = arraySize;
            swapchainCreateInfo.format = format;
            swapchainCreateInfo.width = width;
            swapchainCreateInfo.height = height;
            swapchainCreateInfo.mipCount = 1;
            swapchainCreateInfo.faceCount = 1;
            swapchainCreateInfo.sampleCount = sampleCount;
            swapchainCreateInfo.createFlags = createFlags;
            swapchainCreateInfo.usageFlags = usageFlags;

            XR_CHECK(xrCreateSwapchain(session, &swapchainCreateInfo, &swapchain.Swapchain));

            uint32_t chainLength;
            XR_CHECK(xrEnumerateSwapchainImages(swapchain.Swapchain, 0, &chainLength, nullptr));

            swapchain.Images.resize(chainLength, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
            XR_CHECK(xrEnumerateSwapchainImages(swapchain.Swapchain,
                (uint32_t)swapchain.Images.size(),
                &chainLength,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.Images.data())));

            return swapchain;
        }

        // Return true if an event is available, otherwise return false.
        bool TryReadNextEvent(XrEventDataBuffer* buffer) const {
            // Reset buffer header for every xrPollEvent function call.
            *buffer = { XR_TYPE_EVENT_DATA_BUFFER };
            const XrResult xr = xrPollEvent(m_instance, buffer);
            if (xr == XR_EVENT_UNAVAILABLE) {
                return false;
            }
            else {
                return true;
            }
        }

        void ProcessEvents(bool* exitRenderLoop, bool* requestRestart) {
            *exitRenderLoop = *requestRestart = false;

            XrEventDataBuffer buffer{ XR_TYPE_EVENT_DATA_BUFFER };
            XrEventDataBaseHeader* header = reinterpret_cast<XrEventDataBaseHeader*>(&buffer);

            // Process all pending messages.
            while (TryReadNextEvent(&buffer)) {
                switch (header->type) {
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
                    *exitRenderLoop = true;
                    *requestRestart = false;
                    return;
                }
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    const auto stateEvent = *reinterpret_cast<const XrEventDataSessionStateChanged*>(header);
                    assert(m_session != XR_NULL_HANDLE && m_session == stateEvent.session);
                    m_sessionState = stateEvent.state;
                    switch (m_sessionState) {
                    case XR_SESSION_STATE_READY: {
                        assert(m_session != XR_NULL_HANDLE);
                        XrSessionBeginInfo sessionBeginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
                        sessionBeginInfo.primaryViewConfigurationType = VIEW_CONFIGURATION_TYPE;
                        XR_CHECK(xrBeginSession(m_session, &sessionBeginInfo));
                        m_sessionRunning = true;
                        break;
                    }
                    case XR_SESSION_STATE_STOPPING: {
                        m_sessionRunning = false;
                        XR_CHECK(xrEndSession(m_session));
                        break;
                    }
                    case XR_SESSION_STATE_EXITING: {
                        // Do not attempt to restart because user closed this session.
                        *exitRenderLoop = true;
                        *requestRestart = false;
                        break;
                    }
                    case XR_SESSION_STATE_LOSS_PENDING: {
                        // Poll for a new systemId
                        *exitRenderLoop = true;
                        *requestRestart = true;
                        break;
                    }
                    }
                    break;
                }
                case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                default: {
                    // DEBUG_PRINT("Ignoring event type %d", header->type);
                    break;
                }
                }
            }
        }
        // ------------------- VARIABLES ----------------------
        constexpr static XrFormFactor FORM_FACTOR{ XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
        constexpr static XrViewConfigurationType VIEW_CONFIGURATION_TYPE{ XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
        constexpr static uint32_t STEREO_VIEW_COUNT{ 2 }; // PRIMARY_STEREO view configuration always has 2 views

        const std::string m_applicationName;

        XrInstance m_instance;
        XrSystemId m_systemId{ XR_NULL_SYSTEM_ID };
        XrSession m_session;

        struct {
            bool DepthExtensionSupported{ false };
            bool UnboundedRefSpaceSupported{ false };
            bool SpatialAnchorSupported{ false };
        } m_optionalExtensions;

        XrSpace m_sceneSpace;
        XrReferenceSpaceType m_sceneSpaceType{};

        constexpr static uint32_t LeftSide = 0;
        constexpr static uint32_t RightSide = 1;

        XrEnvironmentBlendMode m_environmentBlendMode{};
        float m_near{};
        float m_far{};

        struct SwapchainD3D11 {
            XrSwapchain Swapchain;
            DXGI_FORMAT Format{ DXGI_FORMAT_UNKNOWN };
            int32_t Width{ 0 };
            int32_t Height{ 0 };
            uint32_t ArraySize{ 0 };
            std::vector<XrSwapchainImageD3D11KHR> Images;
        };

        struct RenderResources {
            std::vector<XrView> Views;
            std::vector<XrViewConfigurationView> ConfigViews;
            SwapchainD3D11 ColorSwapchain;
            SwapchainD3D11 DepthSwapchain;
            std::vector<XrCompositionLayerProjectionView> ProjectionLayerViews;
            std::vector<XrCompositionLayerDepthInfoKHR> DepthInfoViews;
        };

        std::unique_ptr<RenderResources> m_renderResources{};

        bool m_sessionRunning{ false };
        XrSessionState m_sessionState{ XR_SESSION_STATE_UNKNOWN };
    };

    XrResult CreateInstance(XrInstance& instance)
    {
        uint32_t extensionCount;
        XR_DO(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));
        std::vector<XrExtensionProperties> extensionProperties(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
        XR_DO(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensionProperties.data()));

        std::vector<const char*> enabledExtensions;

        // Add a specific extension to the list of extensions to be enabled, if it is supported.
        auto EnableExtentionIfSupported = [&](const char* extensionName)
        {
            for (uint32_t i = 0; i < extensionCount; i++)
            {
                if (strcmp(extensionProperties[i].extensionName, extensionName) == 0)
                {
                    enabledExtensions.push_back(extensionName);
                    return true;
                }
            }
            return false;
        };

        // D3D11 extension is required for this sample, so check if it's supported.
        bool d3d11Supported = EnableExtentionIfSupported(XR_KHR_D3D11_ENABLE_EXTENSION_NAME); // CHECK

        // Additional optional extensions for enhanced functionality. Track whether enabled in m_optionalExtensions.
        bool depthExtensionSupported = EnableExtentionIfSupported(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
        bool unboundedRefSpaceSupported = EnableExtentionIfSupported(XR_MSFT_UNBOUNDED_REFERENCE_SPACE_EXTENSION_NAME);
        bool spatialAnchorSupported = EnableExtentionIfSupported(XR_MSFT_SPATIAL_ANCHOR_EXTENSION_NAME);

        // Create the instance with desired extensions.
        XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
        createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
        createInfo.enabledExtensionNames = enabledExtensions.data();

        createInfo.applicationInfo = { "", 0, "Babylon Native", 410, XR_CURRENT_API_VERSION };
        strcpy_s(createInfo.applicationInfo.applicationName, "asdfadfaf");
        XR_DO(xrCreateInstance(&createInfo, &instance));

        return XrResult::XR_SUCCESS;
    }

    XrResult InitializeSystem(XrInstance& instance, XrSystemId& systemId)
    {
        XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

        while (true)
        {
            XrResult result = xrGetSystem(instance, &systemInfo, &systemId);
            if (SUCCEEDED(result))
            {
                break;
            }
            else if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
            {
                using namespace std::chrono_literals;
                std::this_thread::sleep_for(1s);
            }
            else
            {
                throw std::exception{ "Failed to initialize system." };
            }
        };

        // Choose an environment blend mode.
        XrEnvironmentBlendMode environmentBlendMode;
        {
            // Query the list of supported environment blend modes for the current system
            uint32_t count;
            XR_DO(xrEnumerateEnvironmentBlendModes(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr));
            assert(count > 0); // A system must support at least one environment blend mode.

            std::vector<XrEnvironmentBlendMode> environmentBlendModes(count);
            XR_DO(xrEnumerateEnvironmentBlendModes(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, environmentBlendModes.data()));

            // This sample supports all modes, pick the system's preferred one.
            environmentBlendMode = environmentBlendModes[0];
        }

        // Choose a reasonable depth range can help improve hologram visual quality.
        // Use reversed Z (near > far) for more uniformed Z resolution.
        // m_nearFar = { 20.f, 0.1f };
        float nearDist = 20.f;
        float farDist = 0.1f;
        
        return XrResult::XR_SUCCESS;
    }

    XrResult InitializeSession(XrInstance& instance, XrSystemId& systemId, ID3D11Device* device, XrSession& session)
    {
        // Create the D3D11 device for the adapter associated with the system.
        XrGraphicsRequirementsD3D11KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
        XR_DO(xrGetD3D11GraphicsRequirementsKHR(instance, systemId, &graphicsRequirements));

        XrGraphicsBindingD3D11KHR graphicsBinding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
        graphicsBinding.device = device;

        XrSessionCreateInfo createInfo{ XR_TYPE_SESSION_CREATE_INFO };
        createInfo.next = &graphicsBinding;
        createInfo.systemId = systemId;
        XR_DO(xrCreateSession(instance, &createInfo, &session));

        return XrResult::XR_SUCCESS;
    }

    XrResult CreateSwapChains(XrInstance& instance, XrSession& session, XrSwapchain& swapchain)
    {
        XrSwapchainCreateInfo createInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        createInfo.arraySize = 2;
        createInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        createInfo.width = 512;
        createInfo.height = 512;
        createInfo.mipCount = 1;
        createInfo.faceCount = 1;
        createInfo.sampleCount = 1;
        createInfo.createFlags = 0;
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

        XR_DO(xrCreateSwapchain(session, &createInfo, &swapchain));

        return XrResult::XR_SUCCESS;
    }

    void DoXrStuff(ID3D11Device* device)
    {
        XrInstance instance{};
        if (CreateInstance(instance) != XrResult::XR_SUCCESS)
        {
            throw std::exception{ "Failed to create XR instance!" };
        }

        XrSystemId systemId{};
        if (InitializeSystem(instance, systemId) != XrResult::XR_SUCCESS)
        {
            throw std::exception{ "Failed to initialize XR instance!" };
        }

        XrSession session{};
        if (InitializeSession(instance, systemId, device, session) != XrResult::XR_SUCCESS)
        {
            throw std::exception{ "Failed to initialize session!" };
        }

        XrSwapchain swapchain{};
        if (CreateSwapChains(instance, session, swapchain) != XrResult::XR_SUCCESS)
        {
            throw std::exception{ "Failed to create swapchains!" };
        }

        uint32_t swapchainLength{};
        xrEnumerateSwapchainImages(swapchain, 0, &swapchainLength, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> images{};
        images.resize(swapchainLength, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        xrEnumerateSwapchainImages(swapchain, static_cast<uint32_t>(images.size()), &swapchainLength, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

        // CreateSpaces();
        // CreateSwapchains();

        Sleep(1000);

        XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
        beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        xrBeginSession(session, &beginInfo);

        XrFrameBeginInfo frameBeginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
        xrBeginFrame(session, &frameBeginInfo);

        for (int viewId = 1; viewId <= 3; ++viewId)
        {
            auto frameBuffer = bgfx::createFrameBuffer(512, 512, bgfx::TextureFormat::RGBA8U, BGFX_TEXTURE_RT);
            auto texture = bgfx::getTexture(frameBuffer);
            bgfx::overrideInternal(texture, reinterpret_cast<uintptr_t>(images[viewId - 1].texture));

            bgfx::setViewFrameBuffer(viewId, frameBuffer);
            bgfx::setViewClear(viewId, BGFX_CLEAR_COLOR, 0x00FF00FF);
            bgfx::setViewRect(viewId, 0, 0, 512, 512);
            bgfx::touch(viewId);
        }
        bgfx::frame();

        XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;

        XrFrameEndInfo frameEndInfo{ XR_TYPE_FRAME_END_INFO };
        frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        frameEndInfo.layerCount = 0;
        xrEndFrame(session, &frameEndInfo);
    }
}

namespace babylon
{
    namespace
    {
        template<typename T>
        class RecycleSet
        {
        public:
            RecycleSet(T firstId)
                : m_nextId{ firstId }
            {}

            RecycleSet() : RecycleSet({ 0 })
            {}

            T Get()
            {
                if (m_queue.empty())
                {
                    return m_nextId++;
                }
                else
                {
                    T next = m_queue.back();
                    m_queue.pop();
                    return next;
                }
            }

            void Recycle(T id)
            {
                assert(id < m_nextId);
                m_queue.push(id);
            }

        private:
            T m_nextId{};
            std::queue<bgfx::ViewId> m_queue{};
        };

        struct UniformInfo final
        {
            uint8_t Stage{};
            // uninitilized bgfx resource is kInvalidHandle. 0 can be a valid handle.
            bgfx::UniformHandle Handle{bgfx::kInvalidHandle};
        };

        template<typename AppendageT>
        inline void AppendBytes(std::vector<uint8_t>& bytes, const AppendageT appendage)
        {
            auto ptr = reinterpret_cast<const uint8_t*>(&appendage);
            auto stride = static_cast<std::ptrdiff_t>(sizeof(AppendageT));
            bytes.insert(bytes.end(), ptr, ptr + stride);
        }

        template<typename AppendageT = std::string&>
        inline void AppendBytes(std::vector<uint8_t>& bytes, const std::string& string)
        {
            auto ptr = reinterpret_cast<const uint8_t*>(string.data());
            auto stride = static_cast<std::ptrdiff_t>(string.length());
            bytes.insert(bytes.end(), ptr, ptr + stride);
        }

        template<typename ElementT>
        inline void AppendBytes(std::vector<uint8_t>& bytes, const gsl::span<ElementT>& data)
        {
            auto ptr = reinterpret_cast<const uint8_t*>(data.data());
            auto stride = static_cast<std::ptrdiff_t>(data.size() * sizeof(ElementT));
            bytes.insert(bytes.end(), ptr, ptr + stride);
        }

        void FlipYInImageBytes(gsl::span<uint8_t> bytes, size_t rowCount, size_t rowPitch)
        {
            std::vector<uint8_t> buffer{};
            buffer.reserve(rowPitch);

            for (size_t row = 0; row < rowCount / 2; row++)
            {
                auto frontPtr = bytes.data() + (row * rowPitch);
                auto backPtr = bytes.data() + ((rowCount - row - 1) * rowPitch);

                std::memcpy(buffer.data(), frontPtr, rowPitch);
                std::memcpy(frontPtr, backPtr, rowPitch);
                std::memcpy(backPtr, buffer.data(), rowPitch);
            }
        }

        void AppendUniformBuffer(std::vector<uint8_t>& bytes, const spirv_cross::Compiler& compiler, const spirv_cross::Resource& uniformBuffer, bool isFragment)
        {
            const uint8_t fragmentBit = (isFragment ? BGFX_UNIFORM_FRAGMENTBIT : 0);

            const spirv_cross::SPIRType& type = compiler.get_type(uniformBuffer.base_type_id);
            for (uint32_t index = 0; index < type.member_types.size(); ++index)
            {
                auto name = compiler.get_member_name(uniformBuffer.base_type_id, index);
                auto offset = compiler.get_member_decoration(uniformBuffer.base_type_id, index, spv::DecorationOffset);
                auto memberType = compiler.get_type(type.member_types[index]);

                bgfx::UniformType::Enum bgfxType;
                uint16_t regCount;

                if (memberType.basetype != spirv_cross::SPIRType::Float)
                {
                    throw std::exception(); // Not supported
                }

                if (memberType.columns == 1 && 1 <= memberType.vecsize && memberType.vecsize <= 4)
                {
                    bgfxType = bgfx::UniformType::Vec4;
                    regCount = 1;
                }
                else if (memberType.columns == 4 && memberType.vecsize == 4)
                {
                    bgfxType = bgfx::UniformType::Mat4;
                    regCount = 4;
                }
                else
                {
                    throw std::exception();
                }

                for (const auto size : memberType.array)
                {
                    regCount *= size;
                }

                AppendBytes(bytes, static_cast<uint8_t>(name.size()));
                AppendBytes(bytes, name);
                AppendBytes(bytes, static_cast<uint8_t>(bgfxType | fragmentBit));
                AppendBytes(bytes, static_cast<uint8_t>(0)); // Value "num" not used by D3D11 pipeline.
                AppendBytes(bytes, static_cast<uint16_t>(offset));
                AppendBytes(bytes, static_cast<uint16_t>(regCount));
            }
        }

        void AppendSamplers(std::vector<uint8_t>& bytes, const spirv_cross::Compiler& compiler, const spirv_cross::SmallVector<spirv_cross::Resource>& samplers, bool isFragment, std::unordered_map<std::string, UniformInfo>& cache)
        {
            const uint8_t fragmentBit = (isFragment ? BGFX_UNIFORM_FRAGMENTBIT : 0);

            for (const spirv_cross::Resource& sampler : samplers)
            {
                AppendBytes(bytes, static_cast<uint8_t>(sampler.name.size()));
                AppendBytes(bytes, sampler.name);
                AppendBytes(bytes, static_cast<uint8_t>(bgfx::UniformType::Sampler | BGFX_UNIFORM_SAMPLERBIT));

                // These values (num, regIndex, regCount) are not used by D3D11 pipeline.
                AppendBytes(bytes, static_cast<uint8_t>(0));
                AppendBytes(bytes, static_cast<uint16_t>(0));
                AppendBytes(bytes, static_cast<uint16_t>(0));

                cache[sampler.name].Stage = compiler.get_decoration(sampler.id, spv::DecorationBinding);
            }
        }

        void CacheUniformHandles(bgfx::ShaderHandle shader, std::unordered_map<std::string, UniformInfo>& cache)
        {
            const auto MAX_UNIFORMS = 256;
            bgfx::UniformHandle uniforms[MAX_UNIFORMS];
            auto numUniforms = bgfx::getShaderUniforms(shader, uniforms, MAX_UNIFORMS);

            bgfx::UniformInfo info{};
            for (uint8_t idx = 0; idx < numUniforms; idx++)
            {
                bgfx::getUniformInfo(uniforms[idx], info);
                cache[info.name].Handle = uniforms[idx];
            }
        }

        enum class WebGLAttribType
        {
            BYTE = 5120,
            UNSIGNED_BYTE = 5121,
            SHORT = 5122,
            UNSIGNED_SHORT = 5123,
            INT = 5124,
            UNSIGNED_INT = 5125,
            FLOAT = 5126
        };

        bgfx::AttribType::Enum ConvertAttribType(WebGLAttribType type)
        {
            switch (type)
            {
            case WebGLAttribType::UNSIGNED_BYTE:    return bgfx::AttribType::Uint8;
            case WebGLAttribType::SHORT:            return bgfx::AttribType::Int16;
            case WebGLAttribType::FLOAT:            return bgfx::AttribType::Float;
            default: // avoid "warning: 4 enumeration values not handled"
                throw std::exception();
                break;
            }
        }

        // Must match constants.ts in Babylon.js.
        constexpr std::array<uint64_t, 11> ALPHA_MODE
        {
            // ALPHA_DISABLE
            0x0,

            // ALPHA_ADD: SRC ALPHA * SRC + DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ONE),

            // ALPHA_COMBINE: SRC ALPHA * SRC + (1 - SRC ALPHA) * DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE),

            // ALPHA_SUBTRACT: DEST - SRC * DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_INV_SRC_COLOR, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE),

            // ALPHA_MULTIPLY: SRC * DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_DST_COLOR, BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE),

            // ALPHA_MAXIMIZED: SRC ALPHA * SRC + (1 - SRC) * DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_COLOR, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE),

            // ALPHA_ONEONE: SRC + DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ONE),

            // ALPHA_PREMULTIPLIED: SRC + (1 - SRC ALPHA) * DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE),

            // ALPHA_PREMULTIPLIED_PORTERDUFF: SRC + (1 - SRC ALPHA) * DEST, (1 - SRC ALPHA) * DEST ALPHA
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA),

            // ALPHA_INTERPOLATE: CST * SRC + (1 - CST) * DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_FACTOR, BGFX_STATE_BLEND_INV_FACTOR, BGFX_STATE_BLEND_FACTOR, BGFX_STATE_BLEND_INV_FACTOR),

            // ALPHA_SCREENMODE: SRC + (1 - SRC) * DEST, SRC ALPHA + (1 - SRC ALPHA) * DEST ALPHA
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_COLOR, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA),
        };

        constexpr std::array<bgfx::TextureFormat::Enum, 2> TEXTURE_FORMAT
        {
            bgfx::TextureFormat::RGBA8,
            bgfx::TextureFormat::RGBA32F
        };
    }

    class NativeEngine::Impl final
    {
    public:
        Impl(void* nativeWindowPtr, RuntimeImpl& runtimeImpl);

        void Initialize(Napi::Env& env);
        void UpdateSize(float width, float height);
        void UpdateRenderTarget();
        void Suspend();

    private:
        using EngineDefiner = NativeEngineDefiner<NativeEngine::Impl>;
        friend EngineDefiner;

        struct VertexArray final
        {
            struct IndexBuffer
            {
                bgfx::IndexBufferHandle handle;
            };

            IndexBuffer indexBuffer;

            struct VertexBuffer
            {
                bgfx::VertexBufferHandle handle;
                uint32_t startVertex;
                bgfx::VertexDeclHandle declHandle;
            };

            std::vector<VertexBuffer> vertexBuffers;
        };

        enum BlendMode {}; // TODO DEBUG
        enum class Filter {}; // TODO DEBUG
        enum class AddressMode {}; // TODO DEBUG

        struct TextureData final
        {
            ~TextureData()
            {
                bgfx::destroy(Texture);

                for (auto image : Images)
                {
                    bimg::imageFree(image);
                }
            }

            std::vector<bimg::ImageContainer*> Images{};
            bgfx::TextureHandle Texture{ bgfx::kInvalidHandle };
        };

        class ViewClearState
        {
        public:
            ViewClearState(uint16_t viewId)
                : m_viewId{ viewId }
            {}

            bool Update(const Napi::CallbackInfo& info)
            {
                auto r = info[0].As<Napi::Number>().FloatValue();
                auto g = info[1].As<Napi::Number>().FloatValue();
                auto b = info[2].As<Napi::Number>().FloatValue();
                auto a = info[3].IsUndefined() ? 1.f : info[3].As<Napi::Number>().FloatValue();
                auto backBuffer = info[4].IsUndefined() ? true : info[4].As<Napi::Boolean>().Value();
                auto depth = info[5].IsUndefined() ? true : info[5].As<Napi::Boolean>().Value();
                auto stencil = info[6].IsUndefined() ? true : info[6].As<Napi::Boolean>().Value();

                bool needToUpdate = r != m_red
                    || g != m_green
                    || b != m_blue
                    || a != m_alpha
                    || backBuffer != m_backBuffer
                    || depth != m_depth
                    || stencil != m_stencil;
                if (needToUpdate)
                {
                    m_red = r;
                    m_green = g;
                    m_blue = b;
                    m_alpha = a;
                    m_backBuffer = backBuffer;
                    m_depth = depth;
                    m_stencil = stencil;

                    Update();
                }

                return needToUpdate;
            }

            void Update() const
            {
                // TODO: Backbuffer, depth, and stencil.
                bgfx::setViewClear(m_viewId, BGFX_CLEAR_COLOR | (m_depth ? BGFX_CLEAR_DEPTH : 0x0), Color());
                bgfx::touch(m_viewId);
            }

        private:
            const uint16_t m_viewId{};
            float m_red{ 68.f / 255.f };
            float m_green{ 51.f / 255.f };
            float m_blue{ 85.f / 255.f };
            float m_alpha{ 1.f };
            bool m_backBuffer{ true };
            bool m_depth{ true };
            bool m_stencil{ true };

            uint32_t Color() const
            {
                uint32_t color = 0x0;
                color += static_cast<uint8_t>(m_red * std::numeric_limits<uint8_t>::max());
                color = color << 8;
                color += static_cast<uint8_t>(m_green * std::numeric_limits<uint8_t>::max());
                color = color << 8;
                color += static_cast<uint8_t>(m_blue * std::numeric_limits<uint8_t>::max());
                color = color << 8;
                color += static_cast<uint8_t>(m_alpha * std::numeric_limits<uint8_t>::max());
                return color;
            }
        };

        struct FrameBufferData final
        {
            FrameBufferData(bgfx::FrameBufferHandle frameBuffer, RecycleSet<bgfx::ViewId>& viewIdSet, uint16_t width, uint16_t height)
                : FrameBuffer{ frameBuffer }
                , ViewId{ viewIdSet.Get() }
                , ViewClearState{ ViewId }
                , Width{ width }
                , Height{ height }
                , m_idSet{ viewIdSet }
            {
                assert(ViewId < bgfx::getCaps()->limits.maxViews);
            }

            FrameBufferData(FrameBufferData&) = delete;

            ~FrameBufferData()
            {
                bgfx::destroy(FrameBuffer);
                m_idSet.Recycle(ViewId);
            }

            void SetUpView()
            {
                bgfx::setViewFrameBuffer(ViewId, FrameBuffer);
                ViewClearState.Update();
                bgfx::setViewRect(ViewId, 0, 0, Width, Height);
            }

            bgfx::FrameBufferHandle FrameBuffer{ bgfx::kInvalidHandle };
            bgfx::ViewId ViewId{};
            ViewClearState ViewClearState;
            uint16_t Width{};
            uint16_t Height{};

        private:
            RecycleSet<bgfx::ViewId>& m_idSet;
        };

        struct FrameBufferManager final
        {
            FrameBufferData* CreateNew(bgfx::FrameBufferHandle frameBufferHandle, uint16_t width, uint16_t height)
            {
                return new FrameBufferData(frameBufferHandle, m_idSet, width, height);
            }

            void Bind(FrameBufferData* data)
            {
                assert(m_boundFrameBuffer == nullptr);
                m_boundFrameBuffer = data;

                // TODO: Consider doing this only on bgfx::reset(); the effects of this call don't survive reset, but as
                // long as there's no reset this doesn't technically need to be called every time the frame buffer is bound.
                m_boundFrameBuffer->SetUpView();

                // bgfx::setTexture()? Why?
                // TODO: View order?
            }

            bool IsFrameBufferBound() const
            {
                return m_boundFrameBuffer != nullptr;
            }

            FrameBufferData& GetBound() const
            {
                return *m_boundFrameBuffer;
            }

            void Unbind(FrameBufferData* data)
            {
                assert(m_boundFrameBuffer == data);
                m_boundFrameBuffer = nullptr;
            }

        private:
            RecycleSet<bgfx::ViewId> m_idSet{ 1 };
            FrameBufferData* m_boundFrameBuffer{ nullptr };
        };

        struct ProgramData final
        {
            ~ProgramData()
            {
                bgfx::destroy(Program);
            }

            std::unordered_map<std::string, uint32_t> AttributeLocations{};
            std::unordered_map<std::string, UniformInfo> VertexUniformNameToInfo{};
            std::unordered_map<std::string, UniformInfo> FragmentUniformNameToInfo{};

            bgfx::ProgramHandle Program{};

            struct UniformValue
            {
                std::vector<float> Data{};
                uint16_t ElementLength{};
            };

            std::unordered_map<uint16_t, UniformValue> Uniforms{};

            void SetUniform(bgfx::UniformHandle handle, gsl::span<const float> data, size_t elementLength = 1)
            {
                UniformValue& value = Uniforms[handle.idx];
                value.Data.assign(data.begin(), data.end());
                value.ElementLength = static_cast<uint16_t>(elementLength);
            }
        };

        void RequestAnimationFrame(const Napi::CallbackInfo& info);
        Napi::Value CreateVertexArray(const Napi::CallbackInfo& info);
        void DeleteVertexArray(const Napi::CallbackInfo& info);
        void BindVertexArray(const Napi::CallbackInfo& info);
        Napi::Value CreateIndexBuffer(const Napi::CallbackInfo& info);
        void DeleteIndexBuffer(const Napi::CallbackInfo& info);
        void RecordIndexBuffer(const Napi::CallbackInfo& info);
        Napi::Value CreateVertexBuffer(const Napi::CallbackInfo& info);
        void DeleteVertexBuffer(const Napi::CallbackInfo& info);
        void RecordVertexBuffer(const Napi::CallbackInfo& info);
        Napi::Value CreateProgram(const Napi::CallbackInfo& info);
        Napi::Value GetUniforms(const Napi::CallbackInfo& info);
        Napi::Value GetAttributes(const Napi::CallbackInfo& info);
        void SetProgram(const Napi::CallbackInfo& info);
        void SetState(const Napi::CallbackInfo& info);
        void SetZOffset(const Napi::CallbackInfo& info);
        Napi::Value GetZOffset(const Napi::CallbackInfo& info);
        void SetDepthTest(const Napi::CallbackInfo& info);
        Napi::Value GetDepthWrite(const Napi::CallbackInfo& info);
        void SetDepthWrite(const Napi::CallbackInfo& info);
        void SetColorWrite(const Napi::CallbackInfo& info);
        void SetBlendMode(const Napi::CallbackInfo& info);
        void SetMatrix(const Napi::CallbackInfo& info);
        void SetIntArray(const Napi::CallbackInfo& info);
        void SetIntArray2(const Napi::CallbackInfo& info);
        void SetIntArray3(const Napi::CallbackInfo& info);
        void SetIntArray4(const Napi::CallbackInfo& info);
        void SetFloatArray(const Napi::CallbackInfo& info);
        void SetFloatArray2(const Napi::CallbackInfo& info);
        void SetFloatArray3(const Napi::CallbackInfo& info);
        void SetFloatArray4(const Napi::CallbackInfo& info);
        void SetMatrices(const Napi::CallbackInfo& info);
        void SetMatrix3x3(const Napi::CallbackInfo& info);
        void SetMatrix2x2(const Napi::CallbackInfo& info);
        void SetFloat(const Napi::CallbackInfo& info);
        void SetFloat2(const Napi::CallbackInfo& info);
        void SetFloat3(const Napi::CallbackInfo& info);
        void SetFloat4(const Napi::CallbackInfo& info);
        void SetBool(const Napi::CallbackInfo& info);
        Napi::Value CreateTexture(const Napi::CallbackInfo& info);
        void LoadTexture(const Napi::CallbackInfo& info);
        void LoadCubeTexture(const Napi::CallbackInfo& info);
        Napi::Value GetTextureWidth(const Napi::CallbackInfo& info);
        Napi::Value GetTextureHeight(const Napi::CallbackInfo& info);
        void SetTextureSampling(const Napi::CallbackInfo& info);
        void SetTextureWrapMode(const Napi::CallbackInfo& info);
        void SetTextureAnisotropicLevel(const Napi::CallbackInfo& info);
        void SetTexture(const Napi::CallbackInfo& info);
        void DeleteTexture(const Napi::CallbackInfo& info);
        Napi::Value CreateFrameBuffer(const Napi::CallbackInfo& info);
        void BindFrameBuffer(const Napi::CallbackInfo& info);
        void UnbindFrameBuffer(const Napi::CallbackInfo& info);
        void DrawIndexed(const Napi::CallbackInfo& info);
        void Draw(const Napi::CallbackInfo& info);
        void Clear(const Napi::CallbackInfo& info);
        Napi::Value GetRenderWidth(const Napi::CallbackInfo& info);
        Napi::Value GetRenderHeight(const Napi::CallbackInfo& info);

        void DispatchAnimationFrameAsync(Napi::FunctionReference callback);

        ShaderCompiler m_shaderCompiler;

        ProgramData* m_currentProgram;

        RuntimeImpl& m_runtimeImpl;

        struct
        {
            uint32_t Width{};
            uint32_t Height{};
        } m_size;

        bx::DefaultAllocator m_allocator;
        uint64_t m_engineState;
        ViewClearState m_viewClearState;

        FrameBufferManager m_frameBufferManager{};

        void* m_nativeWindowPtr{};

        // Scratch vector used for data alignment.
        std::vector<float> m_scratch;
    };

    NativeEngine::Impl::Impl(void* nativeWindowPtr, RuntimeImpl& runtimeImpl)
        : m_runtimeImpl{ runtimeImpl }
        , m_currentProgram{ nullptr }
        , m_size{ 1024, 768 }
        , m_engineState{ BGFX_STATE_DEFAULT }
        , m_viewClearState{ 0 }
        , m_nativeWindowPtr{ nativeWindowPtr }
    {}

    void NativeEngine::Impl::Initialize(Napi::Env& env)
    {
        bgfx::Init init{};
        init.platformData.nwh = m_nativeWindowPtr;
        bgfx::setPlatformData(init.platformData);

        init.type = bgfx::RendererType::Direct3D11;
        init.resolution.width = m_size.Width;
        init.resolution.height = m_size.Height;
        init.resolution.reset = BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X4;
        bgfx::init(init);

        bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x443355FF, 1.0f, 0);
        bgfx::setViewRect(0, 0, 0, m_size.Width, m_size.Height);

        EngineDefiner::Define(env, this);

        DoXrStuff(reinterpret_cast<ID3D11Device*>(bgfx::getInternalData()->context));
    }

    void NativeEngine::Impl::UpdateSize(float width, float height)
    {
        auto w = static_cast<uint32_t>(width);
        auto h = static_cast<uint32_t>(height);

        if (w != m_size.Width || h != m_size.Height)
        {
            m_size = { w, h };
            UpdateRenderTarget();
        }
    }

    void NativeEngine::Impl::UpdateRenderTarget()
    {
        bgfx::reset(m_size.Width, m_size.Height, BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X4);
        bgfx::setViewRect(0, 0, 0, m_size.Width, m_size.Height);
    }

    // NativeEngine definitions
    void NativeEngine::Impl::RequestAnimationFrame(const Napi::CallbackInfo& info)
    {
        DispatchAnimationFrameAsync(Napi::Persistent(info[0].As<Napi::Function>()));
    }

    Napi::Value NativeEngine::Impl::CreateVertexArray(const Napi::CallbackInfo& info)
    {
        return Napi::External<VertexArray>::New(info.Env(), new VertexArray{});
    }

    void NativeEngine::Impl::DeleteVertexArray(const Napi::CallbackInfo& info)
    {
        delete info[0].As<Napi::External<VertexArray>>().Data();
    }

    void NativeEngine::Impl::BindVertexArray(const Napi::CallbackInfo& info)
    {
        const auto& vertexArray = *(info[0].As<Napi::External<VertexArray>>().Data());

        bgfx::setIndexBuffer(vertexArray.indexBuffer.handle);

        const auto& vertexBuffers = vertexArray.vertexBuffers;
        for (uint8_t index = 0; index < vertexBuffers.size(); ++index)
        {
            const auto& vertexBuffer = vertexBuffers[index];
            bgfx::setVertexBuffer(index, vertexBuffer.handle, vertexBuffer.startVertex, UINT32_MAX, vertexBuffer.declHandle);
        }
    }

    Napi::Value NativeEngine::Impl::CreateIndexBuffer(const Napi::CallbackInfo& info)
    {
        const Napi::TypedArray data = info[0].As<Napi::TypedArray>();
        const bgfx::Memory* ref = bgfx::copy(data.As<Napi::Uint8Array>().Data(), static_cast<uint32_t>(data.ByteLength()));
        const uint16_t flags = data.TypedArrayType() == napi_typedarray_type::napi_uint16_array ? 0 : BGFX_BUFFER_INDEX32;
        const bgfx::IndexBufferHandle handle = bgfx::createIndexBuffer(ref, flags);
        return Napi::Value::From(info.Env(), static_cast<uint32_t>(handle.idx));
    }

    void NativeEngine::Impl::DeleteIndexBuffer(const Napi::CallbackInfo& info)
    {
        const bgfx::IndexBufferHandle handle{ static_cast<uint16_t>(info[0].As<Napi::Number>().Uint32Value()) };
        bgfx::destroy(handle);
    }

    void NativeEngine::Impl::RecordIndexBuffer(const Napi::CallbackInfo& info)
    {
        VertexArray& vertexArray = *(info[0].As<Napi::External<VertexArray>>().Data());
        const bgfx::IndexBufferHandle handle{ static_cast<uint16_t>(info[1].As<Napi::Number>().Uint32Value()) };
        vertexArray.indexBuffer.handle = handle;
    }

    Napi::Value NativeEngine::Impl::CreateVertexBuffer(const Napi::CallbackInfo& info)
    {
        const Napi::Uint8Array data = info[0].As<Napi::Uint8Array>();

        // HACK: Create an empty valid vertex decl which will never be used. Consider fixing in bgfx.
        bgfx::VertexDecl decl;
        decl.begin();
        decl.m_stride = 1;
        decl.end();

        const bgfx::Memory* ref = bgfx::copy(data.Data(), static_cast<uint32_t>(data.ByteLength()));
        const bgfx::VertexBufferHandle handle = bgfx::createVertexBuffer(ref, decl);
        return Napi::Value::From(info.Env(), static_cast<uint32_t>(handle.idx));
    }

    void NativeEngine::Impl::DeleteVertexBuffer(const Napi::CallbackInfo& info)
    {
        const bgfx::VertexBufferHandle handle{ static_cast<uint16_t>(info[0].As<Napi::Number>().Uint32Value()) };
        bgfx::destroy(handle);
    }

    void NativeEngine::Impl::RecordVertexBuffer(const Napi::CallbackInfo& info)
    {
        VertexArray& vertexArray = *(info[0].As<Napi::External<VertexArray>>().Data());
        const bgfx::VertexBufferHandle handle{ static_cast<uint16_t>(info[1].As<Napi::Number>().Uint32Value()) };
        const uint32_t location = info[2].As<Napi::Number>().Uint32Value();
        const uint32_t byteOffset = info[3].As<Napi::Number>().Uint32Value();
        const uint32_t byteStride = info[4].As<Napi::Number>().Uint32Value();
        const uint32_t numElements = info[5].As<Napi::Number>().Uint32Value();
        const uint32_t type = info[6].As<Napi::Number>().Uint32Value();
        const bool normalized = info[7].As<Napi::Boolean>().Value();

        bgfx::VertexDecl decl;
        decl.begin();
        const bgfx::Attrib::Enum attrib = static_cast<bgfx::Attrib::Enum>(location);
        const bgfx::AttribType::Enum attribType = ConvertAttribType(static_cast<WebGLAttribType>(type));
        decl.add(attrib, numElements, attribType, normalized);
        decl.m_stride = static_cast<uint16_t>(byteStride);
        decl.end();

        vertexArray.vertexBuffers.push_back({ std::move(handle), byteOffset / byteStride, bgfx::createVertexDecl(decl) });
    }

    Napi::Value NativeEngine::Impl::CreateProgram(const Napi::CallbackInfo& info)
    {
        const auto vertexSource = info[0].As<Napi::String>().Utf8Value();
        // TODO: This is a HACK to account for the fact that DirectX and OpenGL disagree about the vertical orientation of screen space.
        // Remove this ASAP when we have a more long-term plan to account for this behavior.
        const auto fragmentSource = std::regex_replace(info[1].As<Napi::String>().Utf8Value(), std::regex("dFdy\\("), "-dFdy(");

        auto programData = new ProgramData();

        std::vector<uint8_t> vertexBytes{};
        std::vector<uint8_t> fragmentBytes{};
        std::unordered_map<std::string, uint32_t> attributeLocations;

        m_shaderCompiler.Compile(vertexSource, fragmentSource, [&](ShaderCompiler::ShaderInfo vertexShaderInfo, ShaderCompiler::ShaderInfo fragmentShaderInfo)
        {
            constexpr uint8_t BGFX_SHADER_BIN_VERSION = 6;

            // These hashes are generated internally by BGFX's custom shader compilation pipeline,
            // which we don't have access to.  Fortunately, however, they aren't used for anything
            // crucial; they just have to match.
            constexpr uint32_t vertexOutputsHash = 0xBAD1DEA;
            constexpr uint32_t fragmentInputsHash = vertexOutputsHash;

            {
                const spirv_cross::Compiler& compiler = *vertexShaderInfo.Compiler;
                const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
                assert(resources.uniform_buffers.size() == 1);
                const spirv_cross::Resource& uniformBuffer = resources.uniform_buffers[0];
                const spirv_cross::SmallVector<spirv_cross::Resource>& samplers = resources.separate_samplers;
                size_t numUniforms = compiler.get_type(uniformBuffer.base_type_id).member_types.size() + samplers.size();

                AppendBytes(vertexBytes, BX_MAKEFOURCC('V', 'S', 'H', BGFX_SHADER_BIN_VERSION));
                AppendBytes(vertexBytes, vertexOutputsHash);
                AppendBytes(vertexBytes, fragmentInputsHash);

                AppendBytes(vertexBytes, static_cast<uint16_t>(numUniforms));
                AppendUniformBuffer(vertexBytes, compiler, uniformBuffer, false);
                AppendSamplers(vertexBytes, compiler, samplers, false, programData->VertexUniformNameToInfo);

                AppendBytes(vertexBytes, static_cast<uint32_t>(vertexShaderInfo.Bytes.size()));
                AppendBytes(vertexBytes, vertexShaderInfo.Bytes);
                AppendBytes(vertexBytes, static_cast<uint8_t>(0));

                AppendBytes(vertexBytes, static_cast<uint8_t>(resources.stage_inputs.size()));
                for (const spirv_cross::Resource& stageInput : resources.stage_inputs)
                {
                    const uint32_t location = compiler.get_decoration(stageInput.id, spv::DecorationLocation);
                    AppendBytes(vertexBytes, bgfx::attribToId(static_cast<bgfx::Attrib::Enum>(location)));
                    attributeLocations[stageInput.name] = location;
                }

                AppendBytes(vertexBytes, static_cast<uint16_t>(compiler.get_declared_struct_size(compiler.get_type(uniformBuffer.base_type_id))));
            }

            {
                const spirv_cross::Compiler& compiler = *fragmentShaderInfo.Compiler;
                const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
                assert(resources.uniform_buffers.size() == 1);
                const spirv_cross::Resource& uniformBuffer = resources.uniform_buffers[0];
                const spirv_cross::SmallVector<spirv_cross::Resource>& samplers = resources.separate_samplers;
                size_t numUniforms = compiler.get_type(uniformBuffer.base_type_id).member_types.size() + samplers.size();

                AppendBytes(fragmentBytes, BX_MAKEFOURCC('F', 'S', 'H', BGFX_SHADER_BIN_VERSION));
                AppendBytes(fragmentBytes, vertexOutputsHash);
                AppendBytes(fragmentBytes, fragmentInputsHash);

                AppendBytes(fragmentBytes, static_cast<uint16_t>(numUniforms));
                AppendUniformBuffer(fragmentBytes, compiler, uniformBuffer, true);
                AppendSamplers(fragmentBytes, compiler, samplers, true, programData->FragmentUniformNameToInfo);

                AppendBytes(fragmentBytes, static_cast<uint32_t>(fragmentShaderInfo.Bytes.size()));
                AppendBytes(fragmentBytes, fragmentShaderInfo.Bytes);
                AppendBytes(fragmentBytes, static_cast<uint8_t>(0));

                // Fragment shaders don't have attributes.
                AppendBytes(fragmentBytes, static_cast<uint8_t>(0));

                AppendBytes(fragmentBytes, static_cast<uint16_t>(compiler.get_declared_struct_size(compiler.get_type(uniformBuffer.base_type_id))));
            }
        });

        auto vertexShader = bgfx::createShader(bgfx::copy(vertexBytes.data(), static_cast<uint32_t>(vertexBytes.size())));
        CacheUniformHandles(vertexShader, programData->VertexUniformNameToInfo);
        programData->AttributeLocations = std::move(attributeLocations);

        auto fragmentShader = bgfx::createShader(bgfx::copy(fragmentBytes.data(), static_cast<uint32_t>(fragmentBytes.size())));
        CacheUniformHandles(fragmentShader, programData->FragmentUniformNameToInfo);

        programData->Program = bgfx::createProgram(vertexShader, fragmentShader, true);

        auto finalizer = [](Napi::Env, ProgramData* data)
        {
            delete data;
        };

        return Napi::External<ProgramData>::New(info.Env(), programData, finalizer);
    }

    Napi::Value NativeEngine::Impl::GetUniforms(const Napi::CallbackInfo& info)
    {
        const auto program = info[0].As<Napi::External<ProgramData>>().Data();
        const auto names = info[1].As<Napi::Array>();

        auto length = names.Length();
        auto uniforms = Napi::Array::New(info.Env(), length);
        for (uint32_t index = 0; index < length; ++index)
        {
            const auto name = names[index].As<Napi::String>().Utf8Value();

            auto vertexFound = program->VertexUniformNameToInfo.find(name);
            auto fragmentFound = program->FragmentUniformNameToInfo.find(name);

            if (vertexFound != program->VertexUniformNameToInfo.end())
            {
                uniforms[index] = Napi::External<UniformInfo>::New(info.Env(), &vertexFound->second);
            }
            else if (fragmentFound != program->FragmentUniformNameToInfo.end())
            {
                uniforms[index] = Napi::External<UniformInfo>::New(info.Env(), &fragmentFound->second);
            }
            else
            {
                uniforms[index] = info.Env().Null();
            }
        }

        return uniforms;
    }

    Napi::Value NativeEngine::Impl::GetAttributes(const Napi::CallbackInfo& info)
    {
        const auto program = info[0].As<Napi::External<ProgramData>>().Data();
        const auto names = info[1].As<Napi::Array>();

        const auto& attributeLocations = program->AttributeLocations;

        auto length = names.Length();
        auto attributes = Napi::Array::New(info.Env(), length);
        for (uint32_t index = 0; index < length; ++index)
        {
            const auto name = names[index].As<Napi::String>().Utf8Value();
            const auto it = attributeLocations.find(name);
            int location = (it == attributeLocations.end() ? -1 : gsl::narrow_cast<int>(it->second));
            attributes[index] = Napi::Value::From(info.Env(), location);
        }

        return attributes;
    }

    void NativeEngine::Impl::SetProgram(const Napi::CallbackInfo& info)
    {
        auto program = info[0].As<Napi::External<ProgramData>>().Data();
        m_currentProgram = program;
    }

    void NativeEngine::Impl::SetState(const Napi::CallbackInfo& info)
    {
        const auto culling = info[0].As<Napi::Boolean>().Value();
        const auto reverseSide = info[2].As<Napi::Boolean>().Value();

        m_engineState &= ~BGFX_STATE_CULL_MASK;
        if (reverseSide)
        {
            m_engineState &= ~BGFX_STATE_FRONT_CCW;

            if (culling)
            {
                m_engineState |= BGFX_STATE_CULL_CW;
            }
        }
        else
        {
            m_engineState |= BGFX_STATE_FRONT_CCW;

            if (culling)
            {
                m_engineState |= BGFX_STATE_CULL_CCW;
            }
        }

        // TODO: zOffset
        const auto zOffset = info[1].As<Napi::Number>().FloatValue();

        bgfx::setState(m_engineState);
    }

    void NativeEngine::Impl::SetZOffset(const Napi::CallbackInfo& info)
    {
        const auto zOffset = info[0].As<Napi::Number>().FloatValue();

        // STUB: Stub.
    }

    Napi::Value NativeEngine::Impl::GetZOffset(const Napi::CallbackInfo& info)
    {
        // STUB: Stub.
        return{};
    }

    void NativeEngine::Impl::SetDepthTest(const Napi::CallbackInfo& info)
    {
        const auto enable = info[0].As<Napi::Boolean>().Value();

        // STUB: Stub.
    }

    Napi::Value NativeEngine::Impl::GetDepthWrite(const Napi::CallbackInfo& info)
    {
        // STUB: Stub.
        return{};
    }

    void NativeEngine::Impl::SetDepthWrite(const Napi::CallbackInfo& info)
    {
        const auto enable = info[0].As<Napi::Boolean>().Value();

        // STUB: Stub.
    }

    void NativeEngine::Impl::SetColorWrite(const Napi::CallbackInfo& info)
    {
        const auto enable = info[0].As<Napi::Boolean>().Value();

        // STUB: Stub.
    }

    void NativeEngine::Impl::SetBlendMode(const Napi::CallbackInfo& info)
    {
        const auto blendMode = static_cast<BlendMode>(info[0].As<Napi::Number>().Int32Value());

        m_engineState &= ~BGFX_STATE_BLEND_MASK;
        m_engineState |= ALPHA_MODE[blendMode];

        bgfx::setState(m_engineState);
    }

    void NativeEngine::Impl::SetMatrix(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const auto matrix = info[1].As<Napi::Float32Array>();

        const size_t elementLength = matrix.ElementLength();
        assert(elementLength == 16);

        m_currentProgram->SetUniform(uniformData->Handle, gsl::make_span(matrix.Data(), elementLength));
    }

    void NativeEngine::Impl::SetIntArray(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const int> array

        assert(false);
    }

    void NativeEngine::Impl::SetIntArray2(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const int> array

        assert(false);
    }

    void NativeEngine::Impl::SetIntArray3(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const int> array

        assert(false);
    }

    void NativeEngine::Impl::SetIntArray4(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const int> array

        assert(false);
    }

    void NativeEngine::Impl::SetFloatArray(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const auto array = info[1].As<Napi::Float32Array>();

        size_t elementLength = array.ElementLength();

        m_scratch.clear();
        for (size_t index = 0; index < elementLength; ++index)
        {
            const float values[] = { array[index], 0.0f, 0.0f, 0.0f };
            m_scratch.insert(m_scratch.end(), values, values + 4);
        }

        m_currentProgram->SetUniform(uniformData->Handle, m_scratch, elementLength);
    }

    void NativeEngine::Impl::SetFloatArray2(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const float> array

        assert(false);
    }

    void NativeEngine::Impl::SetFloatArray3(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const float> array

        assert(false);
    }

    void NativeEngine::Impl::SetFloatArray4(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const float> array

        assert(false);
    }

    void NativeEngine::Impl::SetMatrices(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const auto matricesArray = info[1].As<Napi::Float32Array>();

        const size_t elementLength = matricesArray.ElementLength();
        assert(elementLength % 16 == 0);

        m_currentProgram->SetUniform(uniformData->Handle, gsl::span(matricesArray.Data(), elementLength), elementLength / 16);
    }

    void NativeEngine::Impl::SetMatrix3x3(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const float> matrix

        assert(false);
    }

    void NativeEngine::Impl::SetMatrix2x2(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const float> matrix

        assert(false);
    }

    void NativeEngine::Impl::SetFloat(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const float values[] =
        {
            info[1].As<Napi::Number>().FloatValue(),
            0.0f,
            0.0f,
            0.0f
        };

        m_currentProgram->SetUniform(uniformData->Handle, values);
    }

    void NativeEngine::Impl::SetFloat2(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const float values[] =
        {
            info[1].As<Napi::Number>().FloatValue(),
            info[2].As<Napi::Number>().FloatValue(),
            0.0f,
            0.0f
        };

        m_currentProgram->SetUniform(uniformData->Handle, values);
    }

    void NativeEngine::Impl::SetFloat3(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const float values[] =
        {
            info[1].As<Napi::Number>().FloatValue(),
            info[2].As<Napi::Number>().FloatValue(),
            info[3].As<Napi::Number>().FloatValue(),
            0.0f
        };

        m_currentProgram->SetUniform(uniformData->Handle, values);
    }

    void NativeEngine::Impl::SetFloat4(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const float values[] =
        {
            info[1].As<Napi::Number>().FloatValue(),
            info[2].As<Napi::Number>().FloatValue(),
            info[3].As<Napi::Number>().FloatValue(),
            info[4].As<Napi::Number>().FloatValue()
        };

        m_currentProgram->SetUniform(uniformData->Handle, values);
    }

    void NativeEngine::Impl::SetBool(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, bool value

        assert(false);
    }

    Napi::Value NativeEngine::Impl::CreateTexture(const Napi::CallbackInfo& info)
    {
        return Napi::External<TextureData>::New(info.Env(), new TextureData());
    }

    void NativeEngine::Impl::LoadTexture(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        const auto buffer = info[1].As<Napi::ArrayBuffer>();
        const auto mipMap = info[2].As<Napi::Boolean>().Value();

        textureData->Images.push_back(bimg::imageParse(&m_allocator, buffer.Data(), static_cast<uint32_t>(buffer.ByteLength())));
        auto& image = *textureData->Images.front();

        textureData->Texture = bgfx::createTexture2D(
            image.m_width,
            image.m_height,
            false, // TODO: generate mipmaps when requested
            1,
            static_cast<bgfx::TextureFormat::Enum>(image.m_format),
            0,
            bgfx::makeRef(image.m_data, image.m_size));
    }

    void NativeEngine::Impl::LoadCubeTexture(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        const auto mipLevelsArray = info[1].As<Napi::Array>();
        const auto flipY = info[2].As<Napi::Boolean>().Value();

        std::vector<std::vector<bimg::ImageContainer*>> images{};
        images.reserve(mipLevelsArray.Length());

        uint32_t totalSize = 0;

        for (uint32_t mipLevel = 0; mipLevel < mipLevelsArray.Length(); mipLevel++)
        {
            const auto facesArray = mipLevelsArray[mipLevel].As<Napi::Array>();

            images.emplace_back().reserve(facesArray.Length());

            for (uint32_t face = 0; face < facesArray.Length(); face++)
            {
                const auto image = facesArray[face].As<Napi::TypedArray>();
                auto buffer = gsl::make_span(static_cast<uint8_t*>(image.ArrayBuffer().Data()) + image.ByteOffset(), image.ByteLength());

                textureData->Images.push_back(bimg::imageParse(&m_allocator, buffer.data(), static_cast<uint32_t>(buffer.size())));
                images.back().push_back(textureData->Images.back());
                totalSize += static_cast<uint32_t>(images.back().back()->m_size);
            }
        }

        auto allPixels = bgfx::alloc(totalSize);

        auto ptr = allPixels->data;
        for (uint32_t face = 0; face < images.front().size(); face++)
        {
            for (uint32_t mipLevel = 0; mipLevel < images.size(); mipLevel++)
            {
                const auto image = images[mipLevel][face];

                std::memcpy(ptr, image->m_data, image->m_size);

                if (flipY)
                {
                    FlipYInImageBytes(gsl::make_span(ptr, image->m_size), image->m_height, image->m_size / image->m_height);
                }

                ptr += image->m_size;
            }
        }

        bgfx::TextureFormat::Enum format{};
        switch (images.front().front()->m_format)
        {
            case bimg::TextureFormat::RGBA8:
            {
                format = bgfx::TextureFormat::RGBA8;
                break;
            }
            case bimg::TextureFormat::RGB8:
            {
                format = bgfx::TextureFormat::RGB8;
                break;
            }
            default:
            {
                throw std::exception();
            }
        }

        textureData->Texture = bgfx::createTextureCube(
            images.front().front()->m_width,         // Side size
            true,                                           // Has mips
            1,                                              // Number of layers
            format,                                         // Self-explanatory
            0x0,                                            // Flags
            allPixels);                                     // Memory
    }

    Napi::Value NativeEngine::Impl::GetTextureWidth(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        assert(textureData->Images.size() > 0 && !textureData->Images.front()->m_cubeMap);
        return Napi::Value::From(info.Env(), textureData->Images.front()->m_width);
    }

    Napi::Value NativeEngine::Impl::GetTextureHeight(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        assert(textureData->Images.size() > 0 && !textureData->Images.front()->m_cubeMap);
        return Napi::Value::From(info.Env(), textureData->Images.front()->m_width);
    }

    void NativeEngine::Impl::SetTextureSampling(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        const auto filter = static_cast<Filter>(info[1].As<Napi::Number>().Uint32Value());

        // STUB: Stub.
    }

    void NativeEngine::Impl::SetTextureWrapMode(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        const auto addressModeU = static_cast<AddressMode>(info[1].As<Napi::Number>().Uint32Value());
        const auto addressModeV = static_cast<AddressMode>(info[2].As<Napi::Number>().Uint32Value());
        const auto addressModeW = static_cast<AddressMode>(info[3].As<Napi::Number>().Uint32Value());

        // STUB: Stub.
    }

    void NativeEngine::Impl::SetTextureAnisotropicLevel(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        const auto value = info[1].As<Napi::Number>().Uint32Value();

        // STUB: Stub.
    }

    void NativeEngine::Impl::SetTexture(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const auto textureData = info[1].As<Napi::External<TextureData>>().Data();

        bgfx::setTexture(uniformData->Stage, uniformData->Handle, textureData->Texture);
    }

    void NativeEngine::Impl::DeleteTexture(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        delete textureData;
    }

    Napi::Value NativeEngine::Impl::CreateFrameBuffer(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        uint16_t width = static_cast<uint16_t>(info[1].As<Napi::Number>().Uint32Value());
        uint16_t height = static_cast<uint16_t>(info[2].As<Napi::Number>().Uint32Value());
        bgfx::TextureFormat::Enum format = static_cast<bgfx::TextureFormat::Enum>(info[3].As<Napi::Number>().Uint32Value());
        int samplingMode = info[4].As<Napi::Number>().Uint32Value();
        bool generateStencilBuffer = info[5].As<Napi::Boolean>();
        bool generateDepth = info[6].As<Napi::Boolean>();
        bool generateMipMaps = info[7].As<Napi::Boolean>();

        bgfx::FrameBufferHandle frameBufferHandle{};
        if (generateStencilBuffer && !generateDepth)
        {
            throw std::exception{ /* Does this case even make any sense? */ };
        }
        else if (!generateStencilBuffer && !generateDepth)
        {
            frameBufferHandle = bgfx::createFrameBuffer(width, height, TEXTURE_FORMAT[format], BGFX_TEXTURE_RT);
        }
        else
        {
            auto depthStencilFormat = bgfx::TextureFormat::D32;
            if (generateStencilBuffer)
            {
                depthStencilFormat = bgfx::TextureFormat::D24S8;
            }

            assert(bgfx::isTextureValid(0, false, 1, TEXTURE_FORMAT[format], BGFX_TEXTURE_RT));
            assert(bgfx::isTextureValid(0, false, 1, depthStencilFormat, BGFX_TEXTURE_RT));

            std::array<bgfx::TextureHandle, 2> textures
            {
                bgfx::createTexture2D(width, height, generateMipMaps, 1, TEXTURE_FORMAT[format], BGFX_TEXTURE_RT),
                bgfx::createTexture2D(width, height, generateMipMaps, 1, depthStencilFormat, BGFX_TEXTURE_RT)
            };
            std::array<bgfx::Attachment, textures.size()> attachments{};
            for (int idx = 0; idx < attachments.size(); ++idx)
            {
                attachments[idx].init(textures[idx]);
            }
            frameBufferHandle = bgfx::createFrameBuffer(static_cast<uint8_t>(attachments.size()), attachments.data(), true);
        }

        textureData->Texture = bgfx::getTexture(frameBufferHandle);

        return Napi::External<FrameBufferData>::New(info.Env(), m_frameBufferManager.CreateNew(frameBufferHandle, width, height));
    }

    void NativeEngine::Impl::BindFrameBuffer(const Napi::CallbackInfo& info)
    {
        const auto frameBufferData = info[0].As<Napi::External<FrameBufferData>>().Data();
        m_frameBufferManager.Bind(frameBufferData);
    }

    void NativeEngine::Impl::UnbindFrameBuffer(const Napi::CallbackInfo& info)
    {
        const auto frameBufferData = info[0].As<Napi::External<FrameBufferData>>().Data();
        m_frameBufferManager.Unbind(frameBufferData);
    }

    void NativeEngine::Impl::DrawIndexed(const Napi::CallbackInfo& info)
    {
        const auto fillMode = info[0].As<Napi::Number>().Int32Value();
        const auto elementStart = info[1].As<Napi::Number>().Int32Value();
        const auto elementCount = info[2].As<Napi::Number>().Int32Value();

        // TODO: handle viewport

        for (const auto& it : m_currentProgram->Uniforms)
        {
            const ProgramData::UniformValue& value = it.second;
            bgfx::setUniform({ it.first }, value.Data.data(), value.ElementLength);
        }

        bgfx::submit(m_frameBufferManager.IsFrameBufferBound() ? m_frameBufferManager.GetBound().ViewId : 0, m_currentProgram->Program, 0, true);
    }

    void NativeEngine::Impl::Draw(const Napi::CallbackInfo& info)
    {
        const auto fillMode = info[0].As<Napi::Number>().Int32Value();
        const auto elementStart = info[1].As<Napi::Number>().Int32Value();
        const auto elementCount = info[2].As<Napi::Number>().Int32Value();

        // STUB: Stub.
        // bgfx::submit(), right?  Which means we have to preserve here the state of
        // which program is being worked on.
    }

    void NativeEngine::Impl::Clear(const Napi::CallbackInfo& info)
    {
        if (m_frameBufferManager.IsFrameBufferBound())
        {
            m_frameBufferManager.GetBound().ViewClearState.Update(info);
        }
        else
        {
            m_viewClearState.Update(info);
        }
    }

    Napi::Value NativeEngine::Impl::GetRenderWidth(const Napi::CallbackInfo& info)
    {
        // TODO CHECK: Is this not just the size?  What is this?
        return Napi::Value::From(info.Env(), m_size.Width);
    }

    Napi::Value NativeEngine::Impl::GetRenderHeight(const Napi::CallbackInfo& info)
    {
        // TODO CHECK: Is this not just the size?  What is this?
        return Napi::Value::From(info.Env(), m_size.Height);
    }

    void NativeEngine::Impl::DispatchAnimationFrameAsync(Napi::FunctionReference callback)
    {
        // The purpose of encapsulating the callbackPtr in a std::shared_ptr is because, under the hood, the lambda is
        // put into a kind of function which requires a copy constructor for all of its captured variables.  Because
        // the Napi::FunctionReference is not copyable, this breaks when trying to capture the callback directly, so we
        // wrap it in a std::shared_ptr to allow the capture to function correctly.
        m_runtimeImpl.Execute([this, callbackPtr = std::make_shared<Napi::FunctionReference>(std::move(callback))](auto&)
        {
            //bgfx_test(static_cast<uint16_t>(m_size.Width), static_cast<uint16_t>(m_size.Height));

            callbackPtr->Call({});
            bgfx::frame();
        });
    }

    // NativeEngine exterior definitions.

    NativeEngine::NativeEngine(void* nativeWindowPtr, RuntimeImpl& runtimeImpl)
        : m_impl{ std::make_unique<NativeEngine::Impl>(nativeWindowPtr, runtimeImpl) }
    {
    }

    NativeEngine::~NativeEngine()
    {
    }

    void NativeEngine::Initialize(Napi::Env& env)
    {
        m_impl->Initialize(env);
    }

    void NativeEngine::UpdateSize(float width, float height)
    {
        m_impl->UpdateSize(width, height);
    }

    void NativeEngine::UpdateRenderTarget()
    {
        m_impl->UpdateRenderTarget();
    }
}