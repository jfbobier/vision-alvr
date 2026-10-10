// MIT License
//
// Copyright(c) 2025-2026 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "constantsbuffer.h"
#include "log.h"
#include "driver.h"
#include "utils.h"

#include "ReprojectVS.h"
#include "ReprojectPS.h"
#include "LayerVS.h"
#include "LayerPS.h"
#include "LayerCubePS.h"
#include "layerconstants.h"
#include "ipc_win.h"

#include <algorithm>
#include <deque>
#include <mmdeviceapi.h>

using namespace visionalvr_ipc;

// Logging. General log: <folder of this DLL>\logs\VisionALVR.log, appended by every VisionALVR component (one WriteFile per
// line, so the host and several games can share it): game start/exit and errors only. Debug log (verbose): OVRSHIM_LOG = path
// (harness), else shim_<exe>_<pid>.log in the host's debug session folder, which the shim learns through the IPC block; lines
// logged before that are kept in memory and written once the folder is known.
namespace shimlog {
    static std::mutex g_mutex;
    static FILE* g_env = nullptr;     // OVRSHIM_LOG (harness / manual debugging)
    static FILE* g_session = nullptr; // shim_<exe>_<pid>.log in the host's debug session folder
    static FILE* g_csv = nullptr;     // shim_<exe>_<pid>_frames.csv: one row per submitted frame (VDXR trace + compose/stamp/publish)
    static uint32_t g_csvRows = 0;
    static std::vector<std::string> g_pending;
    static std::wstring g_debugDir;
    static bool g_dirKnown = false;   // the host told us (debug on with a folder, or off)

    static std::string Now() {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char b[32];
        snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d.%03d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        return b;
    }

    static std::wstring ExePath() {
        wchar_t p[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        return p;
    }

    static std::wstring ExeName() {
        const std::wstring p = ExePath();
        const size_t k = p.find_last_of(L"\\/");
        return k == std::wstring::npos ? p : p.substr(k + 1);
    }

    static std::string Utf8(const std::wstring& w) {
        char b[512];
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof(b), nullptr, nullptr);
        return n > 0 ? std::string(b) : std::string();
    }

    static std::wstring DllDir() {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&DllDir, &m);
        wchar_t p[MAX_PATH] = {};
        GetModuleFileNameW(m, p, MAX_PATH);
        std::wstring d = p;
        const size_t k = d.find_last_of(L"\\/");
        return k == std::wstring::npos ? L"." : d.substr(0, k);
    }

    static std::string Format(const char* fmt, va_list a) {
        char b[2048];
        vsnprintf(b, sizeof(b), fmt, a);
        return b;
    }

    static void General(const char* level, const char* fmt, ...) {
        va_list a;
        va_start(a, fmt);
        const std::string msg = Format(fmt, a);
        va_end(a);
        static const std::wstring path = [] {
            const std::wstring logs = DllDir() + L"\\logs";
            CreateDirectoryW(logs.c_str(), nullptr);
            return logs + L"\\VisionALVR.log";
        }();
        static const std::string who = "[shim:" + Utf8(ExeName()) + "]";
        std::string lv = level;
        if (lv.size() < 5) {
            lv.append(5 - lv.size(), ' ');
        }
        const std::string line = Now() + " " + who + " " + lv + " " + msg + "\r\n";
        HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
            CloseHandle(h);
        }
    }

    static void InitEnvLocked() {
        static bool done = false;
        if (done) {
            return;
        }
        done = true;
        char path[MAX_PATH];
        const DWORD n = GetEnvironmentVariableA("OVRSHIM_LOG", path, MAX_PATH);
        if (n > 0 && n < MAX_PATH && strcmp(path, "0") != 0) {
            fopen_s(&g_env, path, "a");
        }
    }

    // Opens the debug log in the host's debug session folder (empty: debug off).
    static void SetDebugDir(const std::wstring& dir) {
        std::lock_guard lock(g_mutex);
        InitEnvLocked();
        if (g_dirKnown && dir == g_debugDir) {
            return;
        }
        g_dirKnown = true;
        g_debugDir = dir;
        if (g_session) {
            fclose(g_session);
            g_session = nullptr;
        }
        if (g_csv) {
            fclose(g_csv);
            g_csv = nullptr;
        }
        if (!dir.empty()) {
            wchar_t name[64];
            swprintf_s(name, L"_%lu.log", GetCurrentProcessId());
            std::wstring exe = ExeName();
            const size_t dot = exe.find_last_of(L'.');
            if (dot != std::wstring::npos) {
                exe = exe.substr(0, dot);
            }
            _wfopen_s(&g_session, (dir + L"\\shim_" + exe + name).c_str(), L"a");
            wchar_t csvName[64];
            swprintf_s(csvName, L"_%lu_frames.csv", GetCurrentProcessId());
            _wfopen_s(&g_csv, (dir + L"\\shim_" + exe + csvName).c_str(), L"a");
            if (g_csv) {
                // all times are QPC seconds (the host's nvh_qpc_seconds and VDXR's ovr_GetTimeInSeconds use the same clock)
                fputs("frame,ovr_frame,t_wait_ret,pred_display,t_begin,n_locate,t_locate_first,t_locate_last,locate_display,"
                      "t_xr_end,end_display,t_end_submit,t_async_wait,t_async_end,t_submit,t_gpu_done,t_publish,"
                      "ts_stamp_ns,ts_render_ns,warp_mdeg,horizon_ms,fresh_wait_ms,layers,dup,stamp_mode\n",
                      g_csv);
                g_csvRows = 0;
            }
        }
        if (g_session) {
            for (const auto& l : g_pending) {
                fputs(l.c_str(), g_session);
            }
            fflush(g_session);
        }
        g_pending.clear();
    }

    // One CSV row (already formatted, with its newline). Flushed every 90 rows so a crash loses at most a second.
    static void Csv(const std::string& row) {
        std::lock_guard lock(g_mutex);
        if (!g_csv) {
            return;
        }
        fputs(row.c_str(), g_csv);
        if (++g_csvRows % 90 == 0) {
            fflush(g_csv);
        }
    }

    static void Debug(const char* fmt, ...) {
        std::lock_guard lock(g_mutex);
        InitEnvLocked();
        va_list a;
        va_start(a, fmt);
        char pid[16];
        snprintf(pid, sizeof(pid), " [%lu] ", GetCurrentProcessId());
        const std::string line = Now() + pid + Format(fmt, a) + "\n";
        va_end(a);
        if (g_env) {
            fputs(line.c_str(), g_env);
            fflush(g_env);
        }
        if (g_session) {
            fputs(line.c_str(), g_session);
            fflush(g_session);
        } else if (!g_dirKnown && g_pending.size() < 200) {
            g_pending.push_back(line); // until the host says whether debug is on
        }
    }
} // namespace shimlog

// Per-frame timing trace from the OpenXR runtime (VDXR fork, visionalvr_trace.h): it resolves ovrshim_Trace from this DLL
// and reports xrWaitFrame / xrBeginFrame / xrLocateViews / xrEndFrame / async submission times (QPC seconds) per OVR frame id.
namespace vatrace {
    struct Frame {
        long long id{-1};
        double waitRet{0}, predDisplay{0}, begin{0}, locateFirst{0}, locateLast{0}, locateDisplay{0}, xrEnd{0}, endDisplay{0},
            endSubmit{0}, asyncWaitRet{0}, asyncEnd{0};
        uint32_t locateCount{0};
        uint32_t layers{0};
    };
    static std::mutex g_mutex;
    static Frame g_ring[64];
    static long long g_lastEndFrameId = -1; // the frame id of the ovr_EndFrame being processed (set right before it)

    static Frame& Slot(long long id) {
        Frame& f = g_ring[(size_t)(id & 63)];
        if (f.id != id) {
            f = Frame{};
            f.id = id;
        }
        return f;
    }

    static Frame Take(long long id) {
        std::lock_guard lock(g_mutex);
        const Frame& f = g_ring[(size_t)(id & 63)];
        return f.id == id ? f : Frame{};
    }
} // namespace vatrace

extern "C" __declspec(dllexport) void __cdecl ovrshim_Trace(int kind, long long frameId, double now, double a, double b) {
    std::lock_guard lock(vatrace::g_mutex);
    vatrace::Frame& f = vatrace::Slot(frameId);
    switch (kind) {
    case 1:
        f.waitRet = now;
        f.predDisplay = a;
        break;
    case 2:
        f.begin = now;
        break;
    case 3:
        if (!f.locateCount) {
            f.locateFirst = now;
        }
        f.locateLast = now;
        f.locateDisplay = a;
        f.locateCount++;
        break;
    case 4:
        f.xrEnd = now;
        f.endDisplay = a;
        f.layers = (uint32_t)b;
        break;
    case 5:
        f.endSubmit = now;
        vatrace::g_lastEndFrameId = frameId;
        break;
    case 6:
        f.asyncWaitRet = now;
        break;
    case 7:
        f.asyncEnd = now;
        vatrace::g_lastEndFrameId = frameId;
        break;
    default:
        break;
    }
}

#define ShimLog shimlog::Debug

// VDXR's own extension flags (virtualdesktop-openxr/OVR_Ext.h): layer shown to one eye only.
static constexpr int kLayerFlagDisableLeft = 0x100000;
static constexpr int kLayerFlagDisableRight = 0x200000;

using namespace ovrnull::driver;
using namespace ovrnull::log;
using namespace ovrnull::utils;

namespace {

    class HostDriver : public IDriver {
      private:
        static inline const OVR::Posef k_HeadToLeftController = {{0, 0, 0, 1}, {-0.15f, -0.2f, -0.35f}};
        static inline const OVR::Posef k_HeadToRightController = {{0, 0, 0, 1}, {0.15f, -0.2f, -0.35f}};

        static constexpr size_t k_SwapchainLength = 3;
        struct Swapchain {
            ComPtr<ID3D11Texture2D> textures[k_SwapchainLength];
            ComPtr<ID3D11ShaderResourceView> SRVs[k_SwapchainLength];
            ComPtr<ID3D11RenderTargetView> RTVs[k_SwapchainLength];
            ovrTextureSwapChainDesc desc{};
            uint32_t lastCommittedIndex{0};
        };

      public:
        HostDriver() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HostDriver_Ctor");

            {
                ComPtr<IDXGIFactory6> factory;
                CreateDXGIFactory2(0, IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()));
                ComPtr<IDXGIAdapter1> adapter;
                factory->EnumAdapterByGpuPreference(
                    0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.ReleaseAndGetAddressOf()));
                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);
                m_adapterLuid = desc.AdapterLuid;
            }
            OpenHost();
            if (m_ipc.state && (m_ipc.state->host.adapterLuidLow || m_ipc.state->host.adapterLuidHigh)) {
                m_adapterLuid.LowPart = m_ipc.state->host.adapterLuidLow;
                m_adapterLuid.HighPart = m_ipc.state->host.adapterLuidHigh;
            }
            m_displayRate = 72.f;
            m_photonsTime = 1.f / m_displayRate;
            m_eyePose[ovrEye_Left].Position.x = -0.0315f;
            m_eyePose[ovrEye_Right].Position.x = 0.0315f;
            m_eyeFov[ovrEye_Left].UpTan = m_eyeFov[ovrEye_Left].DownTan = m_eyeFov[ovrEye_Left].LeftTan =
                m_eyeFov[ovrEye_Left].RightTan = (float)M_PI_2;
            m_eyeFov[ovrEye_Right] = m_eyeFov[ovrEye_Left];
            m_recommendedResolution = {2496, 2688};
            LoadHostConfig();
            m_controllerButtons.ControllerType = ovrControllerType_Touch;
            m_controllerPose[0].ThePose = OVR::Posef(m_hmdPose.ThePose) * k_HeadToLeftController;
            m_controllerPose[1].ThePose = OVR::Posef(m_hmdPose.ThePose) * k_HeadToRightController;
            m_controllerSides = 0x3;

            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            m_nextFramePredictedDisplayTime = QpcToOvrTime(now);

            TraceLoggingWriteStop(local, "HostDriver_Ctor");
        }

        ~HostDriver() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HostDriver_Dtor");

            if (m_serverThread.joinable()) {
                m_terminateServerThread = true;
                m_serverThread.join();
                m_serverThread = {};
            }
            StopPublisher();

            while (!m_swapchains.empty()) {
                Swapchain* swapchain = (Swapchain*)*m_swapchains.begin();
                TraceLoggingWriteTagged(local, "HostDriver_Dtor", TLPArg(swapchain, "DeleteSwapchain"));
                delete swapchain;
                m_swapchains.erase(m_swapchains.begin());
            }

            if (m_ipc.state) {
                m_ipc.state->shim.shimAlive = 0;
                shimlog::General("INFO", "game exit: %llu frames", (unsigned long long)m_framesSubmitted);
            }
            visionalvr_ipc::CloseIpc(m_ipc);

            TraceLoggingWriteStop(local, "HostDriver_Dtor");
        }

        void SetSubmissionDevice(ID3D11Device* device) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HostDriver_SetSubmissionDevice", TLPArg(device, "Device"));

            if (!m_submissionDevice) {
                m_submissionDevice = device;
                m_submissionDevice->GetImmediateContext(m_submissionContext.ReleaseAndGetAddressOf());

                winrt::check_hresult(m_submissionDevice->CreateVertexShader(
                    k_ReprojectVS, sizeof(k_ReprojectVS), nullptr, m_reprojectVS.ReleaseAndGetAddressOf()));
                winrt::check_hresult(m_submissionDevice->CreatePixelShader(
                    k_ReprojectPS, sizeof(k_ReprojectPS), nullptr, m_reprojectPS.ReleaseAndGetAddressOf()));
                winrt::check_hresult(m_submissionDevice->CreateVertexShader(
                    k_LayerVS, sizeof(k_LayerVS), nullptr, m_layerVS.ReleaseAndGetAddressOf()));
                winrt::check_hresult(m_submissionDevice->CreatePixelShader(
                    k_LayerPS, sizeof(k_LayerPS), nullptr, m_layerPS.ReleaseAndGetAddressOf()));
                winrt::check_hresult(m_submissionDevice->CreatePixelShader(
                    k_LayerCubePS, sizeof(k_LayerCubePS), nullptr, m_layerCubePS.ReleaseAndGetAddressOf()));
                {
                    D3D11_BUFFER_DESC bd{};
                    bd.ByteWidth = ((sizeof(LayerConstants) + 15) / 16) * 16;
                    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                    bd.Usage = D3D11_USAGE_DYNAMIC;
                    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                    winrt::check_hresult(
                        m_submissionDevice->CreateBuffer(&bd, nullptr, m_layerConstantsBuffer.ReleaseAndGetAddressOf()));
                    // quads are visible from both sides: no culling
                    D3D11_RASTERIZER_DESC rd{};
                    rd.FillMode = D3D11_FILL_SOLID;
                    rd.CullMode = D3D11_CULL_NONE;
                    rd.DepthClipEnable = TRUE;
                    winrt::check_hresult(
                        m_submissionDevice->CreateRasterizerState(&rd, m_noCullState.ReleaseAndGetAddressOf()));
                }
                {
                    D3D11_BUFFER_DESC desc{};
                    desc.ByteWidth = ((sizeof(ConstantsBuffer) + 15) / 16) * 16;
                    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                    desc.Usage = D3D11_USAGE_DYNAMIC;
                    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

                    winrt::check_hresult(
                        m_submissionDevice->CreateBuffer(&desc, nullptr, m_constantsBuffer.ReleaseAndGetAddressOf()));
                }
                {
                    D3D11_DEPTH_STENCIL_DESC desc{};
                    winrt::check_hresult(m_submissionDevice->CreateDepthStencilState(
                        &desc, m_noDepthTestState.ReleaseAndGetAddressOf()));
                }
                {
                    D3D11_BLEND_DESC desc{};
                    desc.RenderTarget[0].BlendEnable = TRUE;
                    desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
                    desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
                    desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
                    desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
                    desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
                    desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
                    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
                    winrt::check_hresult(
                        m_submissionDevice->CreateBlendState(&desc, m_blendState.ReleaseAndGetAddressOf()));
                }
                {
                    D3D11_SAMPLER_DESC desc{};
                    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                    desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
                    desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
                    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                    desc.MaxAnisotropy = 1;
                    desc.MinLOD = D3D11_MIP_LOD_BIAS_MIN;
                    desc.MaxLOD = D3D11_MIP_LOD_BIAS_MAX;
                    winrt::check_hresult(
                        m_submissionDevice->CreateSamplerState(&desc, m_linearSampler.ReleaseAndGetAddressOf()));
                }
            }

            CreateSbsRing();
            StartPublisher();

            if (!m_serverThread.joinable()) {
                m_terminateServerThread = false;
                m_serverThread = std::thread([&]() {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                    timeBeginPeriod(1);
                    const DWORD periodMs = (DWORD)(1000.f / m_displayRate);
                    LARGE_INTEGER lastTick{};
                    QueryPerformanceCounter(&lastTick);
                    while (!m_terminateServerThread) {
                        // The host paces the display: wait for its vsync event. If the host is absent or stalls,
                        // fall back to a local timer so the app never hangs.
                        DWORD r = WAIT_TIMEOUT;
                        if (m_ipc.state && m_ipc.vsync) {
                            r = WaitForSingleObject(m_ipc.vsync, periodMs * 2 + 5);
                        } else {
                            Sleep(periodMs);
                        }
                        LARGE_INTEGER now;
                        QueryPerformanceCounter(&now);
                        double tick = QpcToOvrTime(now);
                        if (r == WAIT_OBJECT_0 && m_ipc.state && m_ipc.state->host.nextVsyncTimeS > 0) {
                            tick = m_ipc.state->host.nextVsyncTimeS; // time of the host's latest vsync tick
                        }
                        if (m_ipc.state) {
                            m_ipc.state->shim.shimHeartbeat++;
                        }
                        (r == WAIT_OBJECT_0 ? m_vsyncFromHost : m_vsyncTimeouts)++;
                        std::unique_lock lock(m_frameMutex);
                        // ALVR's pacing clock schedules each tick; use its interval (the grid adapts to the client), not the nominal rate
                        double interval = 1.0 / m_displayRate;
                        if (r == WAIT_OBJECT_0 && m_ipc.state) {
                            const double hostInterval = m_ipc.state->host.vsyncIntervalS;
                            // follow ALVR's grid, but never let one odd tick (e.g. a very short remainder) distort the prediction
                            if (hostInterval > interval * 0.75 && hostInterval < interval * 1.25) {
                                interval = hostInterval;
                            }
                        }
                        m_vsyncInterval = interval;
                        m_nextFramePredictedDisplayTime = tick + interval + m_photonsTime;
                        m_lastSignaledVsync++;
                        m_frameVsync.notify_all();
                    }
                    timeEndPeriod(1);
                });
            }

            TraceLoggingWriteStop(local, "HostDriver_SetSubmissionDevice");
        }

        void* CreateSwapchain(const ovrTextureSwapChainDesc& ovrDesc) override {
            // Never let a C++ exception unwind through the OVR C API into the runtime (VDXR): report and return null.
            try {
                return CreateSwapchainImpl(ovrDesc);
            } catch (const winrt::hresult_error& e) {
                ShimLog("CreateSwapchain failed: type=%d format=%d %ux%u array=%d mips=%d samples=%d misc=0x%x bind=0x%x -> HRESULT 0x%08x",
                        (int)ovrDesc.Type, (int)ovrDesc.Format, ovrDesc.Width, ovrDesc.Height, ovrDesc.ArraySize, ovrDesc.MipLevels,
                        ovrDesc.SampleCount, (unsigned)ovrDesc.MiscFlags, (unsigned)ovrDesc.BindFlags, (unsigned)e.code().value);
                shimlog::General("ERROR", "swapchain creation failed (format %d, %ux%u, array %d, samples %d): HRESULT 0x%08x",
                                 (int)ovrDesc.Format, ovrDesc.Width, ovrDesc.Height, ovrDesc.ArraySize, ovrDesc.SampleCount, (unsigned)e.code().value);
            } catch (const std::exception& e) {
                ShimLog("CreateSwapchain failed: %s", e.what());
                shimlog::General("ERROR", "swapchain creation failed: %s", e.what());
            }
            return nullptr;
        }

        void* CreateSwapchainImpl(const ovrTextureSwapChainDesc& ovrDesc) {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "HostDriver_CreateSwapchain",
                                   TLArg(ToString(ovrDesc.Type), "Type"),
                                   TLArg(ToString(ovrDesc.Format), "Format"),
                                   TLArg(ovrDesc.ArraySize, "ArraySize"),
                                   TLArg(ovrDesc.Width, "Width"),
                                   TLArg(ovrDesc.Height, "Height"),
                                   TLArg(ovrDesc.MipLevels, "MipLevels"),
                                   TLArg(ovrDesc.SampleCount, "SampleCount"),
                                   TLArg(!!ovrDesc.StaticImage, "StaticImage"),
                                   TLArg(ovrDesc.MiscFlags, "MiscFlags"),
                                   TLArg(ovrDesc.BindFlags, "BindFlags"));

            Swapchain* swapchain = nullptr;

            const DXGI_FORMAT format = ToDxgiTextureFormat(ovrDesc.Format);
            const bool isDepthSwapchain = (ovrDesc.BindFlags & ovrTextureBind_DX_DepthStencil) || IsDepthFormat(format);

            D3D11_TEXTURE2D_DESC desc{};
            desc.Format = format;
            // Per OVR documentation, depth swapchains are always typeless.
            if ((ovrDesc.MiscFlags & ovrTextureMisc_DX_Typeless) || isDepthSwapchain) {
                desc.Format = GetTypelessFormat(desc.Format);
            }
            desc.Width = ovrDesc.Width;
            desc.Height = ovrDesc.Height;
            desc.SampleDesc.Count = ovrDesc.SampleCount;
            desc.MipLevels = ovrDesc.MipLevels;
            desc.ArraySize = ovrDesc.ArraySize;
            if (ovrDesc.Type == ovrTexture_Cube && (desc.ArraySize == 0 || desc.ArraySize % 6 != 0)) {
                desc.ArraySize = 6 * std::max<UINT>(1, desc.ArraySize); // one cube = 6 faces (VDXR asks for ArraySize 1)
            }
            desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
            if (ovrDesc.BindFlags & ovrTextureBind_DX_RenderTarget) {
                desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
            }
            if (ovrDesc.BindFlags & ovrTextureBind_DX_UnorderedAccess) {
                desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
            }
            if (ovrDesc.BindFlags & ovrTextureBind_DX_DepthStencil) {
                desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
            }
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            if (ovrDesc.Type == ovrTexture_Cube) {
                desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
            }
            if (ovrDesc.MiscFlags & ovrTextureMisc_AllowGenerateMips) {
                desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
            }

            swapchain = new Swapchain;
            swapchain->desc = ovrDesc;

            for (size_t i = 0; i < (ovrDesc.StaticImage ? 1 : k_SwapchainLength); i++) {
                // Create the typeless, app swapchain images.
                winrt::check_hresult(m_submissionDevice->CreateTexture2D(
                    &desc, nullptr, swapchain->textures[i].ReleaseAndGetAddressOf()));

                if (!isDepthSwapchain) {
                    // RTV for mirror rendering (single-slice swapchains only: cube/array textures need array views)
                    if ((ovrDesc.BindFlags & ovrTextureBind_DX_RenderTarget) && ovrDesc.ArraySize == 1 && ovrDesc.Type != ovrTexture_Cube) {
                        D3D11_RENDER_TARGET_VIEW_DESC desc{};
                        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                        desc.Format = format;
                        winrt::check_hresult(m_submissionDevice->CreateRenderTargetView(
                            swapchain->textures[i].Get(), &desc, swapchain->RTVs[i].ReleaseAndGetAddressOf()));
                    }
                    // SRV for compositing.
                    {
                        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                        desc.Format = format;
                        if (ovrDesc.Type == ovrTexture_Cube) {
                            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
                            desc.TextureCube.MipLevels = -1;
                        } else {
                            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                            desc.Texture2D.MipLevels = -1;
                        }
                        winrt::check_hresult(m_submissionDevice->CreateShaderResourceView(
                            swapchain->textures[i].Get(), &desc, swapchain->SRVs[i].ReleaseAndGetAddressOf()));
                    }
                }
            }
            {
                std::unique_lock lock(m_swapchainMutex);
                m_swapchains.insert(swapchain);
            }

            TraceLoggingWriteStop(local, "HostDriver_CreateSwapchain", TLPArg(swapchain, "Swapchain"));

            return swapchain;
        }

        void DestroySwapchain(void* swapchain) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HostDriver_DestroySwapchain", TLPArg(swapchain, "Swapchain"));

            std::unique_lock lock(m_swapchainMutex);
            if (m_swapchains.erase(swapchain)) {
                Swapchain* swapchainObject = (Swapchain*)swapchain;
                delete swapchainObject;
            }

            TraceLoggingWriteStop(local, "HostDriver_DestroySwapchain");
        }

        ovrTextureSwapChainDesc GetSwapchainDesc(void* swapchain) const override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HostDriver_GetSwapchainDesc", TLPArg(swapchain, "Swapchain"));

            std::shared_lock lock(m_swapchainMutex);
            ovrTextureSwapChainDesc desc{};
            if (m_swapchains.count(swapchain)) {
                Swapchain* swapchainObject = (Swapchain*)swapchain;
                desc = swapchainObject->desc;
            }

            TraceLoggingWriteStop(local, "HostDriver_GetSwapchainDesc");

            return desc;
        }

        ID3D11Texture2D* GetSwapchainImage(void* swapchain, int index) const override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(
                local, "HostDriver_GetSwapchainImage", TLPArg(swapchain, "Swapchain"), TLArg(index, "Index"));

            ID3D11Texture2D* image = nullptr;

            std::shared_lock lock(m_swapchainMutex);
            if (m_swapchains.count(swapchain)) {
                Swapchain* swapchainObject = (Swapchain*)swapchain;
                if (index < (!swapchainObject->desc.StaticImage ? k_SwapchainLength : 1)) {
                    image = swapchainObject->textures[index].Get();
                }
            }

            TraceLoggingWriteStop(local, "HostDriver_GetSwapchainImage", TLPArg(image, "Image"));

            return image;
        }

        int GetSwapchainImageIndex(void* swapchain) const override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HostDriver_GetSwapchainImageIndex", TLPArg(swapchain, "Swapchain"));

            int index = -1;
            std::shared_lock lock(m_swapchainMutex);
            if (m_swapchains.count(swapchain)) {
                Swapchain* swapchainObject = (Swapchain*)swapchain;
                index = swapchainObject->lastCommittedIndex == 0
                            ? (!swapchainObject->desc.StaticImage ? (k_SwapchainLength - 1) : 0)
                            : (swapchainObject->lastCommittedIndex - 1);
            }

            TraceLoggingWriteStop(local, "HostDriver_GetSwapchainImageIndex", TLArg(index, "Index"));

            return index;
        }

        bool CommitSwapchainImage(void* swapchain) const override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HostDriver_CommitSwapchainImage", TLPArg(swapchain, "Swapchain"));

            bool success = false;

            std::shared_lock lock(m_swapchainMutex);
            if (m_swapchains.count(swapchain)) {
                Swapchain* swapchainObject = (Swapchain*)swapchain;
                swapchainObject->lastCommittedIndex++;
                if (swapchainObject->lastCommittedIndex >=
                    (!swapchainObject->desc.StaticImage ? k_SwapchainLength : 1)) {
                    swapchainObject->lastCommittedIndex = 0;
                }
                TraceLoggingWriteTagged(
                    local, "HostDriver_CommitSwapchainImage", TLArg(swapchainObject->lastCommittedIndex, "Index"));
                success = true;
            }

            TraceLoggingWriteStop(local, "HostDriver_CommitSwapchainImage", TLArg(success, "Success"));

            return success;
        }

        void SetMirrorTexture(void* swapchain) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HostDriver_SetMirrorTexture", TLPArg(swapchain, "Swapchain"));

            if (swapchain) {
                std::shared_lock lock(m_swapchainMutex);
                if (m_swapchains.count(swapchain)) {
                    Swapchain* swapchainObject = (Swapchain*)swapchain;
                    m_mirrorRTV = swapchainObject->RTVs[0];
                    m_mirrorWidth = swapchainObject->desc.Width;
                    m_mirrorHeight = swapchainObject->desc.Height;
                }
            } else {
                m_mirrorRTV.Reset();
            }

            TraceLoggingWriteStop(local, "HostDriver_SetMirrorTexture");
        }

        double GetPredictedDisplayTime(long long frameIndex) const override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HostDriver_GetPredictedDisplayTime", TLArg(frameIndex, "Frame"));

            double predictedDisplayTime;
            // Base our prediction on the last successfully waited frame.
            if (frameIndex <= m_lastWaitedFrame) {
                predictedDisplayTime = m_nextFramePredictedDisplayTime;
            } else {
                const auto numFramesInTheFuture = frameIndex - m_lastWaitedFrame;
                predictedDisplayTime = m_nextFramePredictedDisplayTime + numFramesInTheFuture * m_vsyncInterval;
            }

            TraceLoggingWriteStop(
                local, "HostDriver_GetPredictedDisplayTime", TLArg(predictedDisplayTime, "PredictedDisplayTime"));

            return predictedDisplayTime;
        }

        void WaitForVsync(long long frameIndex) override {
            if (m_ipc.state && m_ipc.state->host.configSeq != m_hostConfigSeq) {
                LoadHostConfig(false);
                ShimLog("host config changed: FoV/eye offsets reloaded");
            }
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HostDriver_WaitForVsync", TLArg(frameIndex, "Frame"));

            std::unique_lock lock(m_frameMutex);
            // Wait when a future frame is being waited on, or when a frame was submitted since the last wait: VDXR's async
            // submission thread asks for frame id `m_frameCompleted`, which can still be the previous id when the app
            // finished its frame quickly, and skipping that wait let apps run past the display rate (97-111 fps at 90 Hz in
            // the first headset test), rendering frames that were never shown and loading the GPU the encoder shares.
            const bool submittedSinceWait = m_submittedSinceWait.exchange(false);
            m_waitCalls++;
            if (frameIndex <= m_lastWaitedFrame) {
                m_waitRepeatedIndex++;
            }
            if (frameIndex > m_lastWaitedFrame || submittedSinceWait) {
                using namespace std::chrono_literals;

                TraceLocalActivity(wait);
                TraceLoggingWriteStart(wait,
                                       "HostDriver_WaitForVsync_DirectModeComponent",
                                       TLArg(m_lastSignaledVsync, "LastSignaledVsync"));

                // TODO: If the app fell behind, we shouldn't be waiting here.
                const auto lastSignaledVsync = m_lastSignaledVsync;
                if (!m_frameVsync.wait_for(lock, 100ms, [&]() { return m_lastSignaledVsync > lastSignaledVsync; })) {
                    m_waitTimeouts++;
                }
                m_waitWaited++;
                lock.unlock();
                {
                    // GPU throttle, like a real runtime (and SteamVR's WaitGetPoses): the app may start a new frame only when at
                    // most one of its submitted frames is still completing on the GPU. Without it a GPU-bound app runs several
                    // frames ahead, its frames then complete in bunches (two in one display period, then none) and the stream
                    // skips and repeats instead of showing one frame per period.
                    std::unique_lock plock(m_publishMutex);
                    if (!m_publishCv.wait_for(plock, 50ms, [&] { return m_publishQueue.size() <= 1; })) {
                        m_gpuThrottleTimeouts++;
                    } else if (m_publishQueue.size() == 1) {
                        // (one still in flight is the normal pipelined case)
                    }
                    m_gpuThrottleWaits++;
                }
                lock.lock();
                m_lastWaitedFrame = std::max(m_lastWaitedFrame, frameIndex);

                TraceLoggingWriteStop(wait,
                                      "HostDriver_WaitForVsync_DirectModeComponent",
                                      TLArg(m_lastSignaledVsync, "LastSignaledVsync"));
            }

            TraceLoggingWriteStop(local,
                                  "HostDriver_WaitForVsync",
                                  TLArg(m_nextFramePredictedDisplayTime, "NextFramePredictedDisplayTime"));
        }

        static bool IsSrgb(DXGI_FORMAT f) {
            return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                   f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
        }

        enum class TargetMode {
            DirectEncode, // UNORM target: sRGB-sourced content is encoded to sRGB values as it is drawn (single-layer fast path)
            Linear,       // FP16 linear target: all sources converted to linear, encoded by a final pass
            Plain         // mirror texture: values pass through unchanged
        };

        // Exponent for the final encoded value: the client decodes with pow(v, encoding gamma); the user's display gamma (> 1 =
        // brighter) is folded into the same pass.
        void UpdateOutputGamma() {
            float user = m_ipc.state ? m_ipc.state->host.userGamma : 1.f;
            if (!(user > 0.2f && user < 5.f)) {
                user = 1.f;
            }
            m_outputGamma = 1.f / (m_encodingGamma * user);
        }

        // The user's colour correction (VisionALVR.exe sliders, read from the IPC block every frame), for the draw that writes the
        // encoded value only.
        void FillColor(bool final, float& brightness, float& contrast, float& saturation, float& sharpening, int& finalPass) const {
            brightness = contrast = saturation = sharpening = 0.f;
            finalPass = final ? 1 : 0;
            if (!final || !m_ipc.state) {
                return;
            }
            const auto& h = m_ipc.state->host;
            auto clampf = [](float v, float lo, float hi) { return v != v ? 0.f : (v < lo ? lo : (v > hi ? hi : v)); };
            brightness = clampf(h.colorBrightness, -0.5f, 0.5f);
            contrast = clampf(h.colorContrast, -0.5f, 0.5f);
            saturation = clampf(h.colorSaturation, -1.f, 1.f);
            sharpening = clampf(h.colorSharpening, 0.f, 2.f);
        }

        // Follows the host's debug switch: the verbose log goes to its debug session folder.
        void UpdateDebugDir() {
            if (!m_ipc.state) {
                return;
            }
            const auto& h = m_ipc.state->host;
            wchar_t dir[260];
            memcpy(dir, h.debugDir, sizeof(dir));
            dir[259] = 0;
            shimlog::SetDebugDir(h.debugOn ? std::wstring(dir) : std::wstring());
        }

        // The negotiated ALVR encoding gamma is applied where the final encoded value is written (the shared slot), nowhere else.
        float OutputGamma(TargetMode mode) const {
            return mode == TargetMode::DirectEncode ? m_outputGamma : 1.f;
        }

        static void SrgbFlags(TargetMode mode, bool sourceIsSrgb, int& encode, int& decode) {
            encode = (mode == TargetMode::DirectEncode && sourceIsSrgb) ? 1 : 0;
            decode = (mode == TargetMode::Linear && !sourceIsSrgb) ? 1 : 0;
        }

        static bool IsComposedLayerType(const ovrLayerHeader* h) {
            if (!h) {
                return false;
            }
            switch (h->Type) {
            case ovrLayerType_EyeFov:
            case ovrLayerType_EyeFovDepth:
            case ovrLayerType_Quad:
            case ovrLayerType_Cylinder:
            case ovrLayerType_Cube:
                return true;
            default:
                return false;
            }
        }

        void UploadLayerConstants(const LayerConstants& c) {
            D3D11_MAPPED_SUBRESOURCE mapped;
            winrt::check_hresult(
                m_submissionContext->Map(m_layerConstantsBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
            memcpy(mapped.pData, &c, sizeof(c));
            m_submissionContext->Unmap(m_layerConstantsBuffer.Get(), 0);
        }

        static D3D11_VIEWPORT EyeViewport(uint32_t eye, uint32_t width, uint32_t height) {
            D3D11_VIEWPORT viewport{};
            viewport.Width = width / 2.f;
            viewport.TopLeftX = eye * viewport.Width;
            viewport.Height = (float)height;
            viewport.MaxDepth = 1.f;
            return viewport;
        }

        // Draws all supported layers into `rtv` (left eye in the left half, right eye in the right half), in submission order.
        // The head pose a frame is stamped with when it differs from the pose the projection layer was rendered with
        // (stampMode 1): the layers are drawn as seen from this pose.
        struct Stamp {
            bool warp{false};
            OVR::Posef head;
        };

        // Output-eye NDC -> homogeneous source-eye NDC for the projection-layer pass (ReprojectVS/PS, which divide per pixel):
        // the FoV change and the rotation qRel = qSrc^-1 * qOut from the orientation the frame is stamped with (out) to the one
        // the layer was rendered with (src). Returned in the cbuffer's memory layout (column-major float4x4 for mul(v, M)).
        static DirectX::XMFLOAT4X4 EyeHomography(const ovrFovPort& outFov, const ovrFovPort& srcFov, const OVR::Quatf& qRel) {
            // [x y 1] -> view direction in the output eye (OVR: -Z forward, +Y up, NDC y up)
            const double A[3][3] = {{(outFov.LeftTan + outFov.RightTan) / 2.0, 0, (outFov.RightTan - outFov.LeftTan) / 2.0},
                                    {0, (outFov.UpTan + outFov.DownTan) / 2.0, (outFov.UpTan - outFov.DownTan) / 2.0},
                                    {0, 0, -1}};
            double R[3][3];
            for (int c = 0; c < 3; c++) {
                const OVR::Vector3f r = qRel.Rotate(OVR::Vector3f(c == 0 ? 1.f : 0.f, c == 1 ? 1.f : 0.f, c == 2 ? 1.f : 0.f));
                R[0][c] = r.x;
                R[1][c] = r.y;
                R[2][c] = r.z;
            }
            // direction in the source eye -> (X, Y, Z) with source NDC = (X / Z, Y / Z), Z = -d.z
            const double sx = (srcFov.LeftTan + srcFov.RightTan) / 2.0, sy = (srcFov.UpTan + srcFov.DownTan) / 2.0;
            const double B[3][3] = {{1 / sx, 0, (srcFov.RightTan - srcFov.LeftTan) / 2.0 / sx},
                                    {0, 1 / sy, (srcFov.UpTan - srcFov.DownTan) / 2.0 / sy},
                                    {0, 0, -1}};
            double RA[3][3]{}, H[3][3]{};
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    for (int k = 0; k < 3; k++) {
                        RA[i][j] += R[i][k] * A[k][j];
                    }
                }
            }
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    for (int k = 0; k < 3; k++) {
                        H[i][j] += B[i][k] * RA[k][j];
                    }
                }
            }
            // HLSL reads the float4x4 column-major, so memory [j][i] is M(i, j) and mul(v, M)_j = sum_i v_i * mem[j][i] = sum_i H[j][i] v_i
            DirectX::XMFLOAT4X4 m{};
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    m.m[i][j] = (float)H[i][j];
                }
            }
            return m;
        }

        bool ComposeLayers(const std::vector<const ovrLayerHeader*>& layers,
                           ID3D11RenderTargetView* rtv,
                           uint32_t width,
                           uint32_t height,
                           TargetMode mode,
                           const Stamp* stamp = nullptr) {
            const float clearColor[] = {0.f, 0.f, 0.f, 1.f};
            m_submissionContext->ClearRenderTargetView(rtv, clearColor);

            m_submissionContext->OMSetRenderTargets(1, &rtv, nullptr);
            m_submissionContext->RSSetState(m_noCullState.Get());
            m_submissionContext->PSSetSamplers(0, 1, m_linearSampler.GetAddressOf());
            m_submissionContext->OMSetDepthStencilState(m_noDepthTestState.Get(), 0xff);
            m_submissionContext->OMSetBlendState(m_blendState.Get(), nullptr, 0xffffffff);

            // Eye poses (in tracking space) that quad/cylinder/cube layers are drawn from: the pose the projection layer
            // was rendered with (so world-locked layers line up with the world), else the head pose the app last read.
            ovrPosef eyeView[ovrEye_Count];
            bool haveProjection = false;
            for (const ovrLayerHeader* h : layers) {
                if (h && (h->Type == ovrLayerType_EyeFov || h->Type == ovrLayerType_EyeFovDepth)) {
                    const auto* l = (const ovrLayerEyeFov*)h;
                    eyeView[0] = l->RenderPose[0];
                    eyeView[1] = l->RenderPose[1];
                    haveProjection = true;
                    break;
                }
            }
            if (stamp && stamp->warp) {
                // the frame is stamped with (and placed by the headset at) this pose: draw world-locked layers from it
                for (int e = 0; e < ovrEye_Count; e++) {
                    eyeView[e] = stamp->head * OVR::Posef(m_eyePose[e]);
                }
            } else if (!haveProjection) {
                ovrPosef head;
                {
                    std::lock_guard lock(m_lastHeadMutex);
                    head = m_lastHeadPose;
                }
                for (int e = 0; e < ovrEye_Count; e++) {
                    eyeView[e] = OVR::Posef(head) * OVR::Posef(m_eyePose[e]);
                }
            }

            bool ok = true;
            {
                std::shared_lock lock(m_swapchainMutex);

                for (const ovrLayerHeader* layerHeader : layers) {
                    const ovrLayer_Union* layer = (ovrLayer_Union*)layerHeader;

                    // Per OVR documentation, this is legal and equivalent to ovrLayerType_Disabled.
                    if (!layer) {
                        continue;
                    }
                    const unsigned flags = layer->Header.Flags;
                    const bool skipLeft = (flags & kLayerFlagDisableLeft) != 0;
                    const bool skipRight = (flags & kLayerFlagDisableRight) != 0;

                    if (layer->Header.Type == ovrLayerType_EyeFov || layer->Header.Type == ovrLayerType_EyeFovDepth) {
                        const auto eyeFov = &layer->EyeFov;

                        m_submissionContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        m_submissionContext->VSSetShader(m_reprojectVS.Get(), nullptr, 0);
                        m_submissionContext->PSSetShader(m_reprojectPS.Get(), nullptr, 0);
                        m_submissionContext->VSSetConstantBuffers(0, 1, m_constantsBuffer.GetAddressOf());
                        m_submissionContext->PSSetConstantBuffers(0, 1, m_constantsBuffer.GetAddressOf());

                        ConstantsBuffer constants{};
                        constants.flipY = flags & ovrLayerFlag_TextureOriginAtBottomLeft;
                        for (uint32_t eye = 0; eye < ovrEye_Count; eye++) {
                            if ((eye == 0 && skipLeft) || (eye == 1 && skipRight)) {
                                continue;
                            }
                            // FoV change, plus the rotation from the stamped pose to the rendered one when the frame is stamped
                            // with a newer tracking sample than the app rendered with (rotation-only reprojection, like a
                            // compositor's timewarp; the position delta over a few ms is below a millimetre)
                            OVR::Quatf qRel; // identity
                            if (stamp && stamp->warp) {
                                const OVR::Quatf qSrc(eyeFov->RenderPose[eye].Orientation);
                                const OVR::Quatf qOut = (stamp->head * OVR::Posef(m_eyePose[eye])).Rotation;
                                qRel = qSrc.Inverted() * qOut;
                            }
                            constants.reprojectionMatrix = EyeHomography(m_eyeFov[eye], eyeFov->Fov[eye], qRel);

                            // OVR allows specifying null texture for the right eye.
                            const auto swapchain = eye == 0 || !eyeFov->ColorTexture[eye] ? eyeFov->ColorTexture[0]
                                                                                          : eyeFov->ColorTexture[eye];
                            if (!m_swapchains.count(swapchain)) {
                                ok = false;
                                continue;
                            }
                            Swapchain* swapchainObject = (Swapchain*)swapchain;
                            SrgbFlags(mode, IsSrgb(ToDxgiTextureFormat(swapchainObject->desc.Format)),
                                      constants.encodeSrgb, constants.decodeSrgb);
                            constants.outputGamma = OutputGamma(mode);
                            FillColor(mode == TargetMode::DirectEncode, constants.brightness, constants.contrast, constants.saturation,
                                      constants.sharpening, constants.finalPass);

                            constants.imageRectNormalized.offset = {
                                (float)eyeFov->Viewport[eye].Pos.x / swapchainObject->desc.Width,
                                (float)eyeFov->Viewport[eye].Pos.y / swapchainObject->desc.Height};
                            constants.imageRectNormalized.extent = {
                                (float)eyeFov->Viewport[eye].Size.w / swapchainObject->desc.Width,
                                (float)eyeFov->Viewport[eye].Size.h / swapchainObject->desc.Height};

                            D3D11_MAPPED_SUBRESOURCE mappedResources;
                            winrt::check_hresult(m_submissionContext->Map(
                                m_constantsBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResources));
                            memcpy(mappedResources.pData, &constants, sizeof(constants));
                            m_submissionContext->Unmap(m_constantsBuffer.Get(), 0);

                            const D3D11_VIEWPORT viewport = EyeViewport(eye, width, height);
                            m_submissionContext->RSSetViewports(1, &viewport);
                            m_submissionContext->PSSetShaderResources(
                                0, 1, swapchainObject->SRVs[swapchainObject->lastCommittedIndex].GetAddressOf());
                            m_submissionContext->Draw(3, 0);
                        }
                    } else if (layer->Header.Type == ovrLayerType_Quad || layer->Header.Type == ovrLayerType_Cylinder) {
                        // Quad and cylinder share ColorTexture / Viewport / pose at the same offsets (checked by VDXR too).
                        const bool isCylinder = layer->Header.Type == ovrLayerType_Cylinder;
                        const auto& q = layer->Quad;
                        if (!m_swapchains.count(q.ColorTexture)) {
                            ok = false;
                            continue;
                        }
                        Swapchain* sc = (Swapchain*)q.ColorTexture;
                        const bool headLocked = (flags & ovrLayerFlag_HeadLocked) != 0;
                        const ovrPosef layerPose = isCylinder ? layer->Cylinder.CylinderPoseCenter : q.QuadPoseCenter;

                        LayerConstants c{};
                        c.kind = isCylinder ? LAYER_KIND_CYLINDER : LAYER_KIND_QUAD;
                        c.flipY = (flags & ovrLayerFlag_TextureOriginAtBottomLeft) ? 1 : 0;
                        c.premultiply = 0; // VDXR already hands OVR premultiplied textures (alpha pre-processing in its swapchain shader)
                        c.segments = 64;
                        SrgbFlags(mode, IsSrgb(ToDxgiTextureFormat(sc->desc.Format)), c.encodeSrgb, c.decodeSrgb);
                        c.outputGamma = OutputGamma(mode);
                        FillColor(mode == TargetMode::DirectEncode, c.brightness, c.contrast, c.saturation, c.sharpening, c.finalPass);
                        c.uvOffset = {(float)q.Viewport.Pos.x / sc->desc.Width, (float)q.Viewport.Pos.y / sc->desc.Height};
                        c.uvExtent = {(float)q.Viewport.Size.w / sc->desc.Width, (float)q.Viewport.Size.h / sc->desc.Height};
                        if (isCylinder) {
                            c.radius = layer->Cylinder.CylinderRadius;
                            c.angle = layer->Cylinder.CylinderAngle;
                            c.aspect = layer->Cylinder.CylinderAspectRatio > 0.f ? layer->Cylinder.CylinderAspectRatio : 1.f;
                        } else {
                            c.size = {q.QuadSize.x, q.QuadSize.y};
                        }

                        m_submissionContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                        m_submissionContext->VSSetShader(m_layerVS.Get(), nullptr, 0);
                        m_submissionContext->PSSetShader(m_layerPS.Get(), nullptr, 0);
                        m_submissionContext->VSSetConstantBuffers(0, 1, m_layerConstantsBuffer.GetAddressOf());
                        m_submissionContext->PSSetConstantBuffers(0, 1, m_layerConstantsBuffer.GetAddressOf());
                        m_submissionContext->PSSetShaderResources(0, 1, sc->SRVs[sc->lastCommittedIndex].GetAddressOf());

                        for (uint32_t eye = 0; eye < ovrEye_Count; eye++) {
                            if ((eye == 0 && skipLeft) || (eye == 1 && skipRight)) {
                                continue;
                            }
                            // head-locked layers are in head space: the eye sits at its offset from the head
                            const auto view = LoadInvertedOvrPose(headLocked ? m_eyePose[eye] : eyeView[eye]);
                            const auto proj = DirectX::XMMatrixTranspose(LoadOvrProjection(m_eyeFov[eye], 0.05f, 100.f));
                            DirectX::XMStoreFloat4x4(&c.worldViewProj,
                                                     DirectX::XMMatrixTranspose(LoadOvrPose(layerPose) * view * proj));
                            UploadLayerConstants(c);

                            const D3D11_VIEWPORT viewport = EyeViewport(eye, width, height);
                            m_submissionContext->RSSetViewports(1, &viewport);
                            m_submissionContext->Draw(isCylinder ? 2 * (c.segments + 1) : 4, 0);
                        }
                    } else if (layer->Header.Type == ovrLayerType_Cube) {
                        const auto& cube = layer->Cube;
                        if (!m_swapchains.count(cube.CubeMapTexture)) {
                            ok = false;
                            continue;
                        }
                        Swapchain* sc = (Swapchain*)cube.CubeMapTexture;
                        const bool headLocked = (flags & ovrLayerFlag_HeadLocked) != 0;

                        LayerConstants c{};
                        c.kind = LAYER_KIND_CUBE;
                        SrgbFlags(mode, IsSrgb(ToDxgiTextureFormat(sc->desc.Format)), c.encodeSrgb, c.decodeSrgb);
                        c.outputGamma = OutputGamma(mode);
                        FillColor(mode == TargetMode::DirectEncode, c.brightness, c.contrast, c.saturation, c.sharpening, c.finalPass);

                        m_submissionContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        m_submissionContext->VSSetShader(m_layerVS.Get(), nullptr, 0);
                        m_submissionContext->PSSetShader(m_layerCubePS.Get(), nullptr, 0);
                        m_submissionContext->VSSetConstantBuffers(0, 1, m_layerConstantsBuffer.GetAddressOf());
                        m_submissionContext->PSSetConstantBuffers(0, 1, m_layerConstantsBuffer.GetAddressOf());
                        m_submissionContext->PSSetShaderResources(0, 1, sc->SRVs[sc->lastCommittedIndex].GetAddressOf());

                        for (uint32_t eye = 0; eye < ovrEye_Count; eye++) {
                            if ((eye == 0 && skipLeft) || (eye == 1 && skipRight)) {
                                continue;
                            }
                            using namespace DirectX;
                            const ovrPosef& vp = headLocked ? m_eyePose[eye] : eyeView[eye];
                            const XMMATRIX proj = XMMatrixTranspose(LoadOvrProjection(m_eyeFov[eye], 0.05f, 100.f));
                            // view direction -> world (eye orientation) -> cube space (inverse cube orientation), then the
                            // handedness flip: the OVR cube map is left-handed (looking down -Z the +X face is on the left).
                            const XMVECTOR qEye = XMLoadFloat4((const XMFLOAT4*)&vp.Orientation);
                            const XMVECTOR qCube = XMLoadFloat4((const XMFLOAT4*)&cube.Orientation);
                            const XMMATRIX toCube = XMMatrixRotationQuaternion(qEye) *
                                                    XMMatrixRotationQuaternion(XMQuaternionConjugate(qCube)) *
                                                    XMMatrixScaling(-1.f, 1.f, 1.f);
                            XMStoreFloat4x4(&c.invProj, XMMatrixTranspose(XMMatrixInverse(nullptr, proj)));
                            XMStoreFloat4x4(&c.viewToCube, XMMatrixTranspose(toCube));
                            UploadLayerConstants(c);

                            const D3D11_VIEWPORT viewport = EyeViewport(eye, width, height);
                            m_submissionContext->RSSetViewports(1, &viewport);
                            m_submissionContext->Draw(3, 0);
                        }
                    }
                    // Disabled / unsupported layer types are skipped.
                }
            }

            ID3D11RenderTargetView* nullRTV[] = {nullptr};
            m_submissionContext->OMSetRenderTargets(1, nullRTV, nullptr);
            ID3D11ShaderResourceView* nullSRV[] = {nullptr};
            m_submissionContext->PSSetShaderResources(0, 1, nullSRV);
            return ok;
        }

        // Linear FP16 frame -> sRGB-encoded UNORM slot (both eyes at once).
        void EncodeLinearToSlot(ID3D11RenderTargetView* rtv, uint32_t width, uint32_t height) {
            LayerConstants c{};
            c.kind = LAYER_KIND_FULLSCREEN;
            c.uvOffset = {0.f, 0.f};
            c.uvExtent = {1.f, 1.f};
            c.encodeSrgb = 1;
            c.outputGamma = m_outputGamma;
            FillColor(true, c.brightness, c.contrast, c.saturation, c.sharpening, c.finalPass);
            UploadLayerConstants(c);
            m_submissionContext->OMSetRenderTargets(1, &rtv, nullptr);
            m_submissionContext->RSSetState(m_noCullState.Get());
            m_submissionContext->OMSetBlendState(nullptr, nullptr, 0xffffffff); // overwrite
            D3D11_VIEWPORT viewport{};
            viewport.Width = (float)width;
            viewport.Height = (float)height;
            viewport.MaxDepth = 1.f;
            m_submissionContext->RSSetViewports(1, &viewport);
            m_submissionContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_submissionContext->VSSetShader(m_layerVS.Get(), nullptr, 0);
            m_submissionContext->PSSetShader(m_layerPS.Get(), nullptr, 0);
            m_submissionContext->VSSetConstantBuffers(0, 1, m_layerConstantsBuffer.GetAddressOf());
            m_submissionContext->PSSetConstantBuffers(0, 1, m_layerConstantsBuffer.GetAddressOf());
            m_submissionContext->PSSetSamplers(0, 1, m_linearSampler.GetAddressOf());
            m_submissionContext->PSSetShaderResources(0, 1, m_linearSRV.GetAddressOf());
            m_submissionContext->Draw(3, 0);
            ID3D11RenderTargetView* nullRTV[] = {nullptr};
            m_submissionContext->OMSetRenderTargets(1, nullRTV, nullptr);
            ID3D11ShaderResourceView* nullSRV[] = {nullptr};
            m_submissionContext->PSSetShaderResources(0, 1, nullSRV);
        }

        bool SubmitFrame(const std::vector<const ovrLayerHeader*>& layers) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HostDriver_SubmitFrame");
            m_submittedSinceWait = true;
            if (m_ipc.state) {
                UpdateOutputGamma(); // the user's gamma can change live
                if (m_framesSubmitted % 90 == 0) {
                    UpdateDebugDir();
                }
            }
            LogLayersIfChanged(layers);

            // Timestamp (and pose) the frame leaves with. stampMode 1: the newest tracking sample, the layers rotated to it
            // (below); otherwise the sample the app rendered with (FrameClientTimestamp, after the compose).
            Stamp stamp{};
            uint64_t stampTs = 0;
            FrameRow row{};
            row.tSubmit = QpcSeconds();
            row.ovrFrame = vatrace::g_lastEndFrameId;
            row.stampMode = m_ipc.state ? m_ipc.state->host.stampMode : 0;
            for (const ovrLayerHeader* h : layers) {
                row.layers += h ? 1 : 0;
            }
            if (m_ipc.state && m_ipc.state->host.stampMode >= 1) {
                ovrPoseStatef newest{};
                uint64_t ts = 0;
                if (ReadHostHead(newest, ts) && ts != 0) {
                    if (ts + 1000000000ull < m_lastStampTs) {
                        m_lastStampTs = 0; // the client's clock restarted (new headset session)
                    }
                    const float waitMs = m_ipc.state->host.freshWaitMs;
                    if (ts <= m_lastStampTs && waitMs > 0.f) {
                        // No tracking sample since the previous frame was stamped: the headset would treat this frame as already
                        // shown. Give the next sample a few ms (tracking arrives once per headset display frame, with jitter).
                        const double t0 = QpcSeconds();
                        m_diagFreshWaits++;
                        for (uint32_t spins = 0;; spins++) {
                            ovrPoseStatef p{};
                            uint64_t t2 = 0;
                            if (ReadHostHead(p, t2) && t2 > ts) {
                                newest = p;
                                ts = t2;
                                break;
                            }
                            if (QpcSeconds() - t0 >= waitMs / 1000.0) {
                                m_diagStillStale++;
                                break;
                            }
                            if (spins % 16 == 15) {
                                SwitchToThread();
                            } else {
                                _mm_pause();
                            }
                        }
                        m_diagFreshWaitMs.push_back((float)((QpcSeconds() - t0) * 1000.0));
                        row.freshWaitMs = m_diagFreshWaitMs.back();
                    }
                    stamp.warp = true;
                    stamp.head = OVR::Posef(newest.ThePose);
                    stampTs = ts;
                }
            }

            bool ok = true;
            if (m_mirrorRTV) {
                ok &= ComposeLayers(layers, m_mirrorRTV.Get(), m_mirrorWidth, m_mirrorHeight, TargetMode::Plain, &stamp);
            }

            if (m_ipc.state && m_sbsWidth) {
                const double composeStart = QpcSeconds();
                const uint32_t slot = (uint32_t)(m_framesSubmitted % visionalvr_ipc::kSlots);
                int composed = 0;
                for (const ovrLayerHeader* h : layers) {
                    composed += IsComposedLayerType(h) ? 1 : 0;
                }
                if (composed > 1 && m_linearRTV) {
                    // Several layers (e.g. quads over the world): blend in linear light like a real compositor, then
                    // sRGB-encode once into the shared slot.
                    ok &= ComposeLayers(layers, m_linearRTV.Get(), m_sbsWidth, m_sbsHeight, TargetMode::Linear, &stamp);
                    EncodeLinearToSlot(m_sbsRTV[slot].Get(), m_sbsWidth, m_sbsHeight);
                } else {
                    ok &= ComposeLayers(layers, m_sbsRTV[slot].Get(), m_sbsWidth, m_sbsHeight, TargetMode::DirectEncode, &stamp);
                }

                const uint64_t matchedTs = FrameClientTimestamp(layers); // the sample the app rendered with (diagnostics; the stamp in mode 0)
                const uint64_t clientTs = stamp.warp ? stampTs : matchedTs;
                if (stamp.warp) {
                    // how far the layers were rotated, and how much newer the stamp is than the rendered pose
                    for (const ovrLayerHeader* h : layers) {
                        if (h && (h->Type == ovrLayerType_EyeFov || h->Type == ovrLayerType_EyeFovDepth)) {
                            const OVR::Quatf qSrc(((const ovrLayerEyeFov*)h)->RenderPose[0].Orientation);
                            const OVR::Quatf qOut = (stamp.head * OVR::Posef(m_eyePose[0])).Rotation;
                            const OVR::Quatf d = qSrc.Inverted() * qOut;
                            const double s = std::min(1.0, std::sqrt((double)d.x * d.x + (double)d.y * d.y + (double)d.z * d.z));
                            m_diagWarpMdeg.push_back((float)(2.0 * std::asin(s) * 57295.78));
                            row.warpMdeg = m_diagWarpMdeg.back();
                            break;
                        }
                    }
                    m_diagWarpAgeMs.push_back(clientTs >= matchedTs ? (float)((clientTs - matchedTs) / 1e6) : -(float)((matchedTs - clientTs) / 1e6));
                    if (clientTs >= matchedTs && clientTs - matchedTs < 100000000ull) {
                        // how much newer the stamp is than the rendered pose: the horizon the pose extrapolation (stampMode 2) aims at
                        const double age = (double)(clientTs - matchedTs) / 1e9;
                        m_predictHorizonS = 0.9 * m_predictHorizonS.load() + 0.1 * age;
                    }
                }
                if (clientTs != 0 && clientTs == m_lastStampTs) {
                    m_diagSameTs++;
                    row.dup = true;
                }
                row.tsStamp = clientTs;
                row.tsRender = matchedTs;
                row.horizonMs = (float)(m_predictHorizonS.load() * 1000.0);
                if (clientTs != 0) {
                    m_lastStampTs = clientTs;
                }
                ++m_framesSubmitted;
                if (m_framesSubmitted % 900 == 0) {
                    const auto& st = m_ipc.state->shim;
                    const double nowS = QpcSeconds();
                    const double rate = m_statT0 > 0 ? (m_lastSignaledVsync - m_statTicks0) / (nowS - m_statT0) : 0;
                    const double fps = m_statT0 > 0 ? 900.0 / (nowS - m_statT0) : 0;
                    m_statT0 = nowS;
                    m_statTicks0 = m_lastSignaledVsync;
                    ShimLog("frames %llu: %.1f fps, vsync %.1f Hz (host %u events, %u local timeouts), last compose+GPU %.2f ms, "
                            "layers %d, ts matched %u (by display time %llu) / fallback %u; vsync ticks %lld, "
                            "WaitToBeginFrame calls %llu (waited %llu, repeated index %llu, timeouts %llu); publish drops %u; "
                            "GPU throttle timeouts %u",
                            (unsigned long long)m_framesSubmitted, fps, rate, (unsigned)m_vsyncFromHost, (unsigned)m_vsyncTimeouts,
                            st.lastComposeMs, composed, st.tsMatched, (unsigned long long)m_tsByDisplayTime, st.tsFallback,
                            m_lastSignaledVsync, (unsigned long long)m_waitCalls, (unsigned long long)m_waitWaited,
                            (unsigned long long)m_waitRepeatedIndex, (unsigned long long)m_waitTimeouts, m_diagPublishDropped,
                            m_gpuThrottleTimeouts);
                    auto pct = [](std::vector<float>& v, double q) {
                        if (v.empty()) {
                            return 0.0f;
                        }
                        const size_t i = std::min(v.size() - 1, (size_t)(q * v.size()));
                        std::nth_element(v.begin(), v.begin() + i, v.end());
                        return v[i];
                    };
                    const size_t n = m_diagAngleMdeg.size();
                    ShimLog("pose match (%zu frames): residual mdeg p50 %.3f p95 %.3f max %.3f; picked newest/2nd/3rd/older %u/%u/%u/%u, "
                            "age behind newest ms p50 %.1f p95 %.1f max %.1f; ties 1/2/3+ %u/%u/%u; no pose for display time %u; "
                            "same timestamp as the previous frame %u",
                            n, pct(m_diagAngleMdeg, 0.5), pct(m_diagAngleMdeg, 0.95), pct(m_diagAngleMdeg, 1.0), m_diagPickIdx[0],
                            m_diagPickIdx[1], m_diagPickIdx[2], m_diagPickIdx[3], pct(m_diagAgeMs, 0.5), pct(m_diagAgeMs, 0.95),
                            pct(m_diagAgeMs, 1.0), m_diagTies[0], m_diagTies[1], m_diagTies[2], m_diagNoDisplayTime, m_diagSameTs);
                    if (!m_diagWarpMdeg.empty()) {
                        ShimLog("stamp newest (%zu frames): warp mdeg p50 %.1f p95 %.1f max %.1f; stamp newer than rendered pose ms p50 %.1f p95 %.1f max %.1f; "
                                "fresh waits %u (ms p50 %.2f max %.2f; still stale %u); predict: horizon %.1f ms, head rate deg/s p50 %.1f p95 %.1f max %.1f, reads %u",
                                m_diagWarpMdeg.size(), pct(m_diagWarpMdeg, 0.5), pct(m_diagWarpMdeg, 0.95), pct(m_diagWarpMdeg, 1.0),
                                pct(m_diagWarpAgeMs, 0.5), pct(m_diagWarpAgeMs, 0.95), pct(m_diagWarpAgeMs, 1.0), m_diagFreshWaits,
                                pct(m_diagFreshWaitMs, 0.5), pct(m_diagFreshWaitMs, 1.0), m_diagStillStale, m_predictHorizonS.load() * 1000.0,
                                pct(m_diagOmegaDps, 0.5), pct(m_diagOmegaDps, 0.95), pct(m_diagOmegaDps, 1.0), m_diagPredicted);
                    }
                    m_diagOmegaDps.clear();
                    m_diagPredicted = 0;
                    m_diagWarpMdeg.clear();
                    m_diagWarpAgeMs.clear();
                    m_diagFreshWaitMs.clear();
                    m_diagFreshWaits = 0;
                    m_diagStillStale = 0;
                    m_diagAngleMdeg.clear();
                    m_diagAgeMs.clear();
                    memset(m_diagPickIdx, 0, sizeof(m_diagPickIdx));
                    memset(m_diagTies, 0, sizeof(m_diagTies));
                    m_diagNoDisplayTime = 0;
                    m_diagSameTs = 0;
                }
                if (m_publishThread.joinable()) {
                    // The host may only read the slot once the GPU is done with it, and that includes the app's own rendering of
                    // this frame (same device, in order). Waiting here would block the app's render thread for the whole GPU frame
                    // (CPU and GPU work serialised: a 6 ms CPU + 8 ms GPU frame drops from 90 to 45 fps). Like a real compositor,
                    // return now: DXGI signals an event when the GPU is done and the publisher thread hands the frame to the host.
                    std::unique_lock lock(m_publishMutex);
                    // Never block here: VDXR's async submission thread calls us, and the app's xrEndFrame waits for that thread,
                    // so a wait on the GPU (a game with a deep GPU queue) or on the host would stall the game. With 3 frames
                    // already in flight (the ring has 4 slots) the oldest one not being published is dropped instead.
                    while (m_publishQueue.size() >= 3) {
                        m_publishQueue.erase(m_publishQueue.begin() + 1);
                        m_diagPublishDropped++;
                    }
                    ResetEvent(m_gpuDone[slot]);
                    if (FAILED(m_dxgiDevice2->EnqueueSetEvent(m_gpuDone[slot]))) {
                        // still hand it to the publisher (the only thread that publishes while it runs), already complete
                        WaitGpuIdleSpin();
                        SetEvent(m_gpuDone[slot]);
                    }
                    m_publishQueue.push_back({slot, clientTs, composeStart, row});
                    lock.unlock();
                    m_publishCv.notify_all();
                } else {
                    // OVRSHIM_SYNC_SUBMIT=1: wait for the GPU on the app thread (previous behaviour, kill switch)
                    WaitGpuIdleSpin();
                    PublishFrame(slot, clientTs, composeStart);
                    WriteFrameRow(row, QpcSeconds(), QpcSeconds());
                }
            }

            if (!m_ipc.state) {
                ProcessActionKeys();

                // "Maintain" the pose time.
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);

                std::unique_lock lock(m_hmdPoseMutex);
                m_hmdPose.TimeInSeconds = QpcToOvrTime(now);
                std::unique_lock lock2(m_controllerMutex);
                m_controllerPose[0].TimeInSeconds = m_controllerPose[1].TimeInSeconds =
                    m_controllerButtons.TimeInSeconds = m_hmdPose.TimeInSeconds;
            }

            TraceLoggingWriteStop(local, "HostDriver_SubmitFrame");

            return ok;
        }

        // The shim's own spin wait (OVRSHIM_SYNC_SUBMIT=1, or if EnqueueSetEvent fails): until the GPU is done, at most 50 ms.
        void WaitGpuIdleSpin() {
            m_submissionContext->End(m_doneQuery.Get());
            m_submissionContext->Flush();
            const double t0 = QpcSeconds();
            BOOL done = FALSE;
            while (m_submissionContext->GetData(m_doneQuery.Get(), &done, sizeof(done), 0) != S_OK || !done) {
                if (QpcSeconds() - t0 > 0.05) {
                    break;
                }
                _mm_pause();
            }
        }

        // Hands a composed (GPU-complete) slot to the host. Called by exactly one thread at a time: the publisher thread, or the
        // app thread when there is no publisher.
        void PublishFrame(uint32_t slot, uint64_t clientTs, double composeStart) {
            auto& s = m_ipc.state->shim;
            s.lastComposeMs = (float)((QpcSeconds() - composeStart) * 1000.0);
            s.frameSeq++; // odd: the host re-reads until it sees the same even value before and after
            MemoryBarrier();
            s.frameSlot = slot;
            s.frameClientTsNs = clientTs;
            s.frameSubmitTimeS = QpcSeconds();
            s.frameCounter = ++m_framesPublished;
            MemoryBarrier();
            s.frameSeq++; // even: stable
            SetEvent(m_ipc.frame);
        }

        void StartPublisher() {
            if (!m_ipc.state || !m_sbsWidth || m_publishThread.joinable()) {
                return;
            }
            wchar_t buf[8];
            const DWORD n = GetEnvironmentVariableW(L"OVRSHIM_SYNC_SUBMIT", buf, 8);
            const bool sync = n > 0 && n < 8 && buf[0] == L'1';
            if (!sync && SUCCEEDED(m_submissionDevice.As(&m_dxgiDevice2))) {
                for (auto& e : m_gpuDone) {
                    e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                }
                m_stopPublisher = false;
                m_publishThread = std::thread([this]() {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                    for (;;) {
                        PendingFrame f{};
                        {
                            std::unique_lock lock(m_publishMutex);
                            m_publishCv.wait(lock, [&] { return m_stopPublisher || !m_publishQueue.empty(); });
                            if (m_publishQueue.empty()) {
                                return; // stopping and drained
                            }
                            f = m_publishQueue.front();
                        }
                        // same 50 ms cap as the spin wait: a hung GPU must not stall the stream forever
                        WaitForSingleObject(m_gpuDone[f.slot], 50);
                        const double gpuDone = QpcSeconds();
                        PublishFrame(f.slot, f.clientTs, f.composeStart);
                        WriteFrameRow(f.row, gpuDone, QpcSeconds());
                        {
                            std::unique_lock lock(m_publishMutex);
                            // the submission thread may have dropped entries behind us: pop only the one we published
                            if (!m_publishQueue.empty() && m_publishQueue.front().slot == f.slot && m_publishQueue.front().clientTs == f.clientTs) {
                                m_publishQueue.pop_front();
                            }
                        }
                        m_publishCv.notify_all();
                    }
                });
            }
            m_ipc.state->shim.submitMode = m_publishThread.joinable() ? 1 : 0;
            ShimLog("submit mode: %s", m_publishThread.joinable() ? "async (publisher thread)" : "sync (GPU wait on the app thread)");
            shimlog::General("INFO", "game start: %s | %u x %u %d-bit @ %.0f Hz | fov L %.2f/%.2f/%.2f/%.2f | encoding gamma %.2f | submit %s | IPC %ls",
                             shimlog::Utf8(shimlog::ExePath()).c_str(), m_sbsWidth, m_sbsHeight, m_ipc.state->host.encodeBits == 8 ? 8 : 10, m_displayRate,
                             m_eyeFov[0].LeftTan, m_eyeFov[0].RightTan, m_eyeFov[0].UpTan, m_eyeFov[0].DownTan, m_encodingGamma,
                             m_publishThread.joinable() ? "async" : "sync", IpcNamespace());
        }

        void StopPublisher() {
            if (m_publishThread.joinable()) {
                {
                    std::unique_lock lock(m_publishMutex);
                    m_stopPublisher = true;
                }
                m_publishCv.notify_all();
                m_publishThread.join();
            }
            for (auto& e : m_gpuDone) {
                if (e) {
                    CloseHandle(e);
                    e = nullptr;
                }
            }
        }

        // Client tracking timestamp to send this frame with: the one of the head pose the projection layer was rendered with.
        // Like ALVR's own SteamVR driver (PoseHistory::GetBestPoseMatch), match the layer's RenderPose against the poses the
        // app read; "the last pose read" is wrong for engines that read the next frame's pose before submitting this one
        // (render-thread pipelining), and the headset then reprojects the frame against the wrong pose.
        uint64_t FrameClientTimestamp(const std::vector<const ovrLayerHeader*>& layers) {
            const ovrLayerEyeFov* eye = nullptr;
            for (const ovrLayerHeader* h : layers) {
                if (h && (h->Type == ovrLayerType_EyeFov || h->Type == ovrLayerType_EyeFovDepth)) {
                    eye = (const ovrLayerEyeFov*)h;
                    break;
                }
            }
            uint64_t ts = m_lastPoseClientTsNs.load();
            bool matched = false;
            if (eye) {
                // eye orientation = head orientation * eye-to-head rotation (identity for ALVR's orthogonal views)
                const OVR::Quatf target =
                    OVR::Quatf(eye->RenderPose[0].Orientation) * OVR::Quatf(m_eyePose[ovrEye_Left].Orientation).Inverted();
                std::lock_guard lock(m_lastHeadMutex);
                auto sample = [&](size_t k) -> const PoseSample& { // k = 0 is the newest
                    return m_poseRing[(m_poseRingHead + kPoseRing - 1 - k) % kPoseRing];
                };
                // Angle between a recorded pose and the layer's, from the vector part of the relative rotation (sin of half the
                // angle): exact down to float rounding (~1e-7 rad). The |dot| = cos(angle / 2) used before cancels near 1 and
                // could not tell apart poses less than ~0.07 degree apart.
                auto angle = [&](const PoseSample& p) {
                    const double pw = p.orientation.w, px = -(double)p.orientation.x, py = -(double)p.orientation.y,
                                 pz = -(double)p.orientation.z; // conjugate of p
                    const double rx = pw * target.x + px * target.w + py * target.z - pz * target.y;
                    const double ry = pw * target.y - px * target.z + py * target.w + pz * target.x;
                    const double rz = pw * target.z + px * target.y - py * target.x + pz * target.w;
                    return 2.0 * std::asin(std::min(1.0, std::sqrt(rx * rx + ry * ry + rz * rz)));
                };
                if (m_poseRingCount && sample(0).clientTsNs < m_lastFrameTsNs) {
                    m_lastFrameTsNs = 0; // the client's clock restarted (new headset session)
                }
                // The pose the app rendered with is the recorded one with the smallest angle; a head that turns back passes the
                // same orientation twice, so never go behind the previous frame's timestamp. Poses within float noise of the best
                // are a tie (a still head): VDXR stamps the layer with the frame's display time (SensorSampleTime), the time the
                // app asked its pose for, so prefer a tie read for that time (pipelined engines read frame N+1's pose before
                // submitting frame N: taking the newest of the ties stamped frame N with N+1's sample, a few pixels of tremble),
                // else the newest. Display time alone is not enough: some engines render with a pose read for another time.
                constexpr double kTie = 2e-6;         // rad (~0.0001 degree)
                constexpr double kMaxAngle = 0.0087;  // rad (~0.5 degree): the app rendered with a pose it read from us
                double best = 1e9;
                for (size_t k = 0; k < m_poseRingCount; k++) {
                    if (sample(k).clientTsNs >= m_lastFrameTsNs) {
                        best = std::min(best, angle(sample(k)));
                    }
                }
                if (best <= kMaxAngle) {
                    size_t pick = kPoseRing, pickTimed = kPoseRing;
                    for (size_t k = 0; k < m_poseRingCount; k++) { // newest first
                        const PoseSample& p = sample(k);
                        if (p.clientTsNs < m_lastFrameTsNs || angle(p) > best + kTie) {
                            continue;
                        }
                        if (pick == kPoseRing) {
                            pick = k;
                        }
                        if (pickTimed == kPoseRing && eye->SensorSampleTime > 0.0 &&
                            std::abs(p.displayTime - eye->SensorSampleTime) < 1e-4) {
                            pickTimed = k;
                        }
                    }
                    if (pickTimed < kPoseRing) {
                        pick = pickTimed;
                        m_tsByDisplayTime++;
                    } else {
                        m_diagNoDisplayTime++;
                    }
                    if (pick < kPoseRing) {
                        ts = sample(pick).clientTsNs;
                        matched = true;
                        uint32_t ties = 0;
                        for (size_t k = 0; k < m_poseRingCount; k++) {
                            if (sample(k).clientTsNs >= m_lastFrameTsNs && angle(sample(k)) <= best + kTie) {
                                ties++;
                            }
                        }
                        m_diagTies[std::min<uint32_t>(ties, 3) - 1]++;
                        m_diagPickIdx[std::min<size_t>(pick, 3)]++;
                        m_diagAngleMdeg.push_back((float)(best * 57295.78));
                        m_diagAgeMs.push_back((float)((sample(0).clientTsNs - sample(pick).clientTsNs) / 1e6));
                    }
                }
                m_lastFrameTsNs = std::max(m_lastFrameTsNs, ts);
            }
            if (m_ipc.state) {
                (matched ? m_ipc.state->shim.tsMatched : m_ipc.state->shim.tsFallback)++;
            }
            return ts;
        }

        // Logs the frame's layer layout when it changes (at most every 2 s, 300 lines per process): type, flags, swapchain
        // size/format/array, sub-rect, quad size / FoV. For diagnosing apps whose UI or view lands in the wrong place.
        void LogLayersIfChanged(const std::vector<const ovrLayerHeader*>& layers) {
            if (m_layerLogs >= 300) {
                return;
            }
            std::string sig;
            char b[256];
            std::shared_lock lock(m_swapchainMutex);
            auto sc = [&](void* p) {
                if (!p || !m_swapchains.count(p)) {
                    return std::string("?");
                }
                const auto& d = ((Swapchain*)p)->desc;
                snprintf(b, sizeof(b), "%dx%d fmt%d arr%d ms%d%s", d.Width, d.Height, (int)d.Format, d.ArraySize, d.SampleCount,
                         d.Type == ovrTexture_Cube ? " cube" : "");
                return std::string(b);
            };
            for (const ovrLayerHeader* h : layers) {
                if (!h) {
                    sig += " [null]";
                    continue;
                }
                const auto* l = (const ovrLayer_Union*)h;
                if (h->Type == ovrLayerType_EyeFov || h->Type == ovrLayerType_EyeFovDepth) {
                    const auto& e = l->EyeFov;
                    snprintf(b, sizeof(b), " [eyefov%s f=0x%x L{%s} vp(%d,%d %dx%d) fov(%.2f %.2f %.2f %.2f) R{", h->Type == ovrLayerType_EyeFovDepth ? "+depth" : "",
                             h->Flags, sc(e.ColorTexture[0]).c_str(), e.Viewport[0].Pos.x, e.Viewport[0].Pos.y, e.Viewport[0].Size.w,
                             e.Viewport[0].Size.h, e.Fov[0].LeftTan, e.Fov[0].RightTan, e.Fov[0].UpTan, e.Fov[0].DownTan);
                    sig += b;
                    snprintf(b, sizeof(b), "%s} vp(%d,%d %dx%d)]", e.ColorTexture[1] ? sc(e.ColorTexture[1]).c_str() : "=L", e.Viewport[1].Pos.x,
                             e.Viewport[1].Pos.y, e.Viewport[1].Size.w, e.Viewport[1].Size.h);
                    sig += b;
                } else if (h->Type == ovrLayerType_Quad || h->Type == ovrLayerType_Cylinder) {
                    const auto& q = l->Quad;
                    snprintf(b, sizeof(b), " [%s f=0x%x {%s} vp(%d,%d %dx%d) size(%.2f %.2f) pos(%.2f %.2f %.2f)]",
                             h->Type == ovrLayerType_Quad ? "quad" : "cyl", h->Flags, sc(q.ColorTexture).c_str(), q.Viewport.Pos.x,
                             q.Viewport.Pos.y, q.Viewport.Size.w, q.Viewport.Size.h, q.QuadSize.x, q.QuadSize.y,
                             q.QuadPoseCenter.Position.x, q.QuadPoseCenter.Position.y, q.QuadPoseCenter.Position.z);
                    sig += b;
                } else if (h->Type == ovrLayerType_Cube) {
                    snprintf(b, sizeof(b), " [cube f=0x%x {%s}]", h->Flags, sc(l->Cube.CubeMapTexture).c_str());
                    sig += b;
                } else {
                    snprintf(b, sizeof(b), " [type%d]", (int)h->Type);
                    sig += b;
                }
            }
            const double now = QpcSeconds();
            if (sig != m_lastLayerSig && now - m_lastLayerLogS >= 2.0) {
                m_lastLayerSig = sig;
                m_lastLayerLogS = now;
                m_layerLogs++;
                ShimLog("layers (%zu):%s", layers.size(), sig.c_str());
            }
        }

        void ProcessActionKeys() {
#define ACTION_KEY(label, key, action)                                                                                 \
    static bool wasCtrl##label##Pressed = false;                                                                       \
    const bool isCtrl##label##Pressed = GetAsyncKeyState(key) < 0;                                                     \
    if (!wasCtrl##label##Pressed && isCtrl##label##Pressed) {                                                          \
        action();                                                                                                      \
    }                                                                                                                  \
    wasCtrl##label##Pressed = isCtrl##label##Pressed;

            OVR::Posef transform = OVR::Posef::Identity();
            ACTION_KEY(Reset, 'R', [&] {
                std::unique_lock lock(m_hmdPoseMutex);
                m_hmdPose.ThePose = OVR::Posef::Identity();
            });
            ACTION_KEY(Forward, 'W', [&] { transform.Translation.z = -0.1f; });
            ACTION_KEY(Left, 'A', [&] { transform.Translation.x = -0.1f; });
            ACTION_KEY(Backward, 'S', [&] { transform.Translation.z = 0.1f; });
            ACTION_KEY(Right, 'D', [&] { transform.Translation.x = 0.1f; });
            ACTION_KEY(TurnLeft, 'Q', [&] { transform.Rotation = OVR::Quatf::FastFromRotationVector({0, 0.1f, 0}); });
            ACTION_KEY(TurnRight, 'E', [&] { transform.Rotation = OVR::Quatf::FastFromRotationVector({0, -0.1f, 0}); });
            std::unique_lock lock(m_hmdPoseMutex);
            m_hmdPose.ThePose = OVR::Posef(m_hmdPose.ThePose) * transform;

            std::unique_lock lock2(m_controllerMutex);
            m_controllerButtons.IndexTriggerRaw[0] = GetAsyncKeyState('U') < 0 ? 1.f : 0.f;
            if (GetAsyncKeyState('I') < 0) {
                m_controllerButtons.Buttons |= ovrButton_Enter;
            } else {
                m_controllerButtons.Buttons &= ~ovrButton_Enter;
            }
            m_controllerButtons.IndexTriggerRaw[1] = GetAsyncKeyState('O') < 0 ? 1.f : 0.f;
            if (GetAsyncKeyState('P') < 0) {
                m_controllerButtons.Buttons |= ovrButton_A;
            } else {
                m_controllerButtons.Buttons &= ~ovrButton_A;
            }
            m_controllerPose[0].ThePose = OVR::Posef(m_hmdPose.ThePose) * k_HeadToLeftController;
            m_controllerPose[1].ThePose = OVR::Posef(m_hmdPose.ThePose) * k_HeadToRightController;
        }

        ovrPoseStatef PropagatePose(const ovrPoseStatef& pose, double time) const {
            const float deltaTime = (float)(time - pose.TimeInSeconds);
            if (deltaTime < FLT_EPSILON) {
                return pose;
            }

            ovrPoseStatef predictedPose;

            // Integrate linear velocity over time.
            OVR::Vector3f linearVelocity = pose.LinearVelocity;
            OVR::Vector3f position = pose.ThePose.Position;
            position += (linearVelocity * deltaTime);
            predictedPose.ThePose.Position = position;
            predictedPose.LinearAcceleration = pose.LinearAcceleration;
            predictedPose.LinearVelocity = pose.LinearVelocity;

            // Integrate angular velocity and acceleration over time. ovrPoseStatef velocities are in tracking (world) space (VDXR
            // hands them to OpenXR as base-space velocities); OVR::Quat::TimeIntegrate expects body-frame rates.
            OVR::Quatf orientation = pose.ThePose.Orientation;
            predictedPose.ThePose.Orientation = orientation.TimeIntegrate(
                orientation.InverseRotate(pose.AngularVelocity), orientation.InverseRotate(pose.AngularAcceleration), deltaTime);
            predictedPose.AngularAcceleration = pose.AngularAcceleration;
            predictedPose.AngularVelocity = pose.AngularVelocity;

            predictedPose.TimeInSeconds = time;

            return predictedPose;
        }

        void OpenHost() {
            char exe[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exe, MAX_PATH);
            if (!OpenIpc(m_ipc)) {
                m_ipc = {}; // run standalone (keyboard-driven, like OVRNull)
                ShimLog("start: %s: host IPC not available (alvr_host not running, or a different IPC version): standalone", exe);
                shimlog::General("WARN", "VisionALVR host not reachable (not running, or a different version): this game will not reach the headset");
                return;
            }
            {
                const std::wstring name = shimlog::ExeName();
                wcsncpy_s(m_ipc.state->shim.appExe, name.c_str(), _TRUNCATE);
            }
            UpdateDebugDir();
            ShimLog("start: %s: host IPC open (%ls namespace, %s), client %s", exe, IpcNamespace(),
                    m_ipc.created ? "created here: the host has not started yet" : "host's objects",
                    m_ipc.state->host.clientConnected ? "connected" : "not connected yet");
            m_ipc.state->shim.shimAlive = 1;
            // Like SteamVR, wait for the headset (streaming client) before presenting a device.
            wchar_t buf[16];
            const DWORD n = GetEnvironmentVariableW(L"OVRSHIM_WAIT_S", buf, 16);
            const double waitS = n ? _wtof(buf) : 20.0;
            const double t0 = QpcSeconds();
            while (!m_ipc.state->host.clientConnected && QpcSeconds() - t0 < waitS) {
                Sleep(50);
            }
            ShimLog("headset %s after %.1f s", m_ipc.state->host.clientConnected ? "ready" : "NOT connected (gave up waiting)", QpcSeconds() - t0);
            if (!m_ipc.state->host.clientConnected) {
                shimlog::General("WARN", "no headset connected after %.0f s: the game starts with default display settings%s", QpcSeconds() - t0,
                                 m_ipc.created ? " (the IPC block was created here: the host was not running, or runs in another namespace/elevation)" : "");
            }
            UpdateDebugDir();
        }

        // initial: also the display rate and recommended resolution (fixed for the life of the app's session). Later reloads
        // (the host re-publishes the config when the headset sends new views, e.g. an IPD change) only take FoV and eye offsets.
        void LoadHostConfig(bool initial = true) {
            if (!m_ipc.state) {
                return;
            }
            const auto& h = m_ipc.state->host;
            m_hostConfigSeq = h.configSeq;
            if (h.encodingGamma > 0.1f && h.encodingGamma < 10.f) {
                m_encodingGamma = h.encodingGamma;
            }
            UpdateOutputGamma();
            if (initial && h.displayRateHz > 1) {
                m_displayRate = (float)h.displayRateHz;
                m_vsyncInterval = 1.0 / m_displayRate;
                m_photonsTime = 1.f / m_displayRate;
            }
            if (initial && h.eyeWidth > 0 && h.eyeHeight > 0) {
                // The app renders at this size (GPU cost); the composed SBS frame keeps the headset's size and the projection pass
                // resamples. renderScale < 1 is the lever for GPU-bound games (SteamVR's "resolution per eye" slider).
                const float scale = (h.renderScale > 0.25f && h.renderScale <= 2.f) ? h.renderScale : 1.f;
                m_recommendedResolution = {(int)(h.eyeWidth * scale + 0.5f) & ~1, (int)(h.eyeHeight * scale + 0.5f) & ~1};
                if (scale != 1.f) {
                    ShimLog("render scale %.2f: app renders %d x %d per eye (headset %d x %d)", scale, m_recommendedResolution.w,
                            m_recommendedResolution.h, h.eyeWidth, h.eyeHeight);
                }
            }
            for (int e = 0; e < 2; e++) {
                if (h.eyeFovTan[e][0] > 0) {
                    m_eyeFov[e].LeftTan = h.eyeFovTan[e][0];
                    m_eyeFov[e].RightTan = h.eyeFovTan[e][1];
                    m_eyeFov[e].UpTan = h.eyeFovTan[e][2];
                    m_eyeFov[e].DownTan = h.eyeFovTan[e][3];
                    m_eyePose[e].Position = {h.eyeOffset[e][0], h.eyeOffset[e][1], h.eyeOffset[e][2]};
                }
            }
        }

        void CreateSbsRing() {
            if (!m_ipc.state || m_sbsWidth) {
                return;
            }
            const auto& h = m_ipc.state->host;
            if (h.eyeWidth <= 0 || h.eyeHeight <= 0) {
                return;
            }
            m_sbsWidth = (uint32_t)h.eyeWidth * 2;
            m_sbsHeight = (uint32_t)h.eyeHeight;
            const DXGI_FORMAT fmt = h.encodeBits == 10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;

            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = m_sbsWidth;
            desc.Height = m_sbsHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = fmt;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            for (int i = 0; i < visionalvr_ipc::kSlots; i++) {
                winrt::check_hresult(m_submissionDevice->CreateTexture2D(&desc, nullptr, m_sbsTex[i].ReleaseAndGetAddressOf()));
                winrt::check_hresult(m_submissionDevice->CreateRenderTargetView(
                    m_sbsTex[i].Get(), nullptr, m_sbsRTV[i].ReleaseAndGetAddressOf()));
                ComPtr<IDXGIResource> res;
                winrt::check_hresult(m_sbsTex[i].As(&res));
                HANDLE shared = nullptr;
                winrt::check_hresult(res->GetSharedHandle(&shared));
                m_ipc.state->shim.slotHandle[i] = (uint64_t)(uintptr_t)shared;
            }
            {
                // linear-light FP16 intermediate for multi-layer frames
                D3D11_TEXTURE2D_DESC ld{};
                ld.Width = m_sbsWidth;
                ld.Height = m_sbsHeight;
                ld.MipLevels = 1;
                ld.ArraySize = 1;
                ld.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                ld.SampleDesc.Count = 1;
                ld.Usage = D3D11_USAGE_DEFAULT;
                ld.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                winrt::check_hresult(m_submissionDevice->CreateTexture2D(&ld, nullptr, m_linearTex.ReleaseAndGetAddressOf()));
                winrt::check_hresult(m_submissionDevice->CreateRenderTargetView(m_linearTex.Get(), nullptr, m_linearRTV.ReleaseAndGetAddressOf()));
                winrt::check_hresult(m_submissionDevice->CreateShaderResourceView(m_linearTex.Get(), nullptr, m_linearSRV.ReleaseAndGetAddressOf()));
            }
            D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
            winrt::check_hresult(m_submissionDevice->CreateQuery(&qd, m_doneQuery.ReleaseAndGetAddressOf()));
            m_ipc.state->shim.sbsWidth = (int32_t)m_sbsWidth;
            m_ipc.state->shim.sbsHeight = (int32_t)m_sbsHeight;
            m_ipc.state->shim.sbsFormat = (uint32_t)fmt;
            ShimLog("ring: encoding gamma %.2f; %u x %u %s-bit, %.2f Hz, fov L(%.3f %.3f %.3f %.3f) R(%.3f %.3f %.3f %.3f) tan", m_encodingGamma, m_sbsWidth, m_sbsHeight,
                    h.encodeBits == 10 ? "10" : "8", m_displayRate, m_eyeFov[0].LeftTan, m_eyeFov[0].RightTan, m_eyeFov[0].UpTan,
                    m_eyeFov[0].DownTan, m_eyeFov[1].LeftTan, m_eyeFov[1].RightTan, m_eyeFov[1].UpTan, m_eyeFov[1].DownTan);
            MemoryBarrier();
            m_ipc.state->shim.frameCounter = 0;
        }

        std::string GetManufacturerName() const override {
            return "Virtual Desktop";
        }

        std::string GetModelNumber() const override {
            return "Emulated Device";
        }

        LUID GetAdapterLuid() const override {
            return m_adapterLuid;
        }

        float GetDisplayRate() const override {
            return m_displayRate;
        }

        ovrSizei GetRecommendedResolution() const override {
            return m_recommendedResolution;
        }

        ovrFovPort GetEyeFov(ovrEyeType eye) const override {
            return m_eyeFov[eye];
        }

        ovrPosef GetEyePose(ovrEyeType eye) const override {
            std::shared_lock lock(m_hmdPoseMutex);
            return m_eyePose[eye];
        }

        void SetStageTracking(bool useStage) override {
            m_useStageTracking = useStage;
        }

        bool IsStageTracking() const override {
            return m_useStageTracking;
        }

        float GetEyeHeight() const override {
            return OVR_DEFAULT_EYE_HEIGHT;
        }

        bool IsAswActive() const override {
            return false;
        }

        // Reads the host's latest head pose (seqlock) as an ovrPoseStatef stamped with the shim-clock receive time.
        bool ReadHostHead(ovrPoseStatef& out, uint64_t& clientTsNs) const {
            if (!m_ipc.state) {
                return false;
            }
            const auto& h = m_ipc.state->host;
            for (int attempt = 0; attempt < 8; attempt++) {
                const uint64_t s1 = h.headSeq;
                if (s1 & 1 || s1 == 0) {
                    _mm_pause();
                    continue;
                }
                MemoryBarrier();
                const visionalvr_ipc::Pose p = h.head;
                const double t = h.headHostTimeS;
                const uint64_t ts = h.headClientTsNs;
                float lv[3], av[3];
                memcpy(lv, h.headLinVel, sizeof(lv));
                memcpy(av, h.headAngVel, sizeof(av));
                MemoryBarrier();
                if (h.headSeq != s1) {
                    continue;
                }
                ovrPoseStatef pose{};
                pose.ThePose.Orientation = {p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]};
                pose.ThePose.Position = {p.position[0], p.position[1], p.position[2]};
                pose.LinearVelocity = {lv[0], lv[1], lv[2]};
                pose.AngularVelocity = {av[0], av[1], av[2]};
                pose.TimeInSeconds = t;
                out = pose;
                clientTsNs = ts;
                return true;
            }
            return false;
        }

        // Reads one hand from the host under the same seqlock as the head (no torn pose). False if the hand is not tracked.
        bool ReadHostHand(int side, ovrPoseStatef& out) const {
            const auto& h = m_ipc.state->host;
            for (int attempt = 0; attempt < 8; attempt++) {
                const uint64_t s1 = h.headSeq;
                if (s1 & 1) {
                    _mm_pause();
                    continue;
                }
                MemoryBarrier();
                const uint32_t valid = h.handValid[side];
                const visionalvr_ipc::Pose p = h.hand[side];
                float lv[3], av[3];
                memcpy(lv, h.handLinVel[side], sizeof(lv));
                memcpy(av, h.handAngVel[side], sizeof(av));
                const double t = h.headHostTimeS;
                MemoryBarrier();
                if (h.headSeq != s1) {
                    continue;
                }
                if (!valid) {
                    return false;
                }
                out = {};
                out.ThePose.Orientation = {p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]};
                out.ThePose.Position = {p.position[0], p.position[1], p.position[2]};
                out.LinearVelocity = {lv[0], lv[1], lv[2]};
                out.AngularVelocity = {av[0], av[1], av[2]};
                out.TimeInSeconds = t;
                return true;
            }
            return false;
        }

        // Head angular / linear velocity from the two newest distinct tracking samples (their client timestamps give the exact
        // spacing), lightly smoothed. The headset's own velocity is not used: it is noisy (finite differences of ARKit poses).
        void UpdateHeadVelocity(uint64_t ts, const ovrPoseStatef& s) const {
            const OVR::Quatf q(s.ThePose.Orientation);
            const OVR::Vector3f p(s.ThePose.Position);
            if (ts < m_prevSample.clientTsNs) {
                m_prevSample.clientTsNs = 0; // the client's clock restarted
                m_headAngVel = m_headLinVel = OVR::Vector3f(0.f, 0.f, 0.f);
            }
            if (m_prevSample.clientTsNs != 0 && ts > m_prevSample.clientTsNs) {
                const double dt = (ts - m_prevSample.clientTsNs) / 1e9;
                if (dt >= 0.004 && dt <= 0.06) {
                    const OVR::Quatf dq = (q * m_prevSample.q.Inverted()).Normalized(); // world-frame rotation prev -> new
                    OVR::Vector3f axis;
                    float angle = 0.f;
                    dq.GetAxisAngle(&axis, &angle);
                    if (angle > 3.14159265f) {
                        angle -= 2.f * 3.14159265f; // the short way round
                    }
                    const float rate = angle / (float)dt;
                    const OVR::Vector3f v = (p - m_prevSample.p) / (float)dt;
                    if (std::abs(rate) <= 15.f && v.Length() <= 5.f) { // else a tracking glitch / recenter: keep the previous estimate
                        m_headAngVel = m_headAngVel * 0.5f + axis * (rate * 0.5f);
                        m_headLinVel = m_headLinVel * 0.5f + v * 0.5f;
                        m_diagOmegaDps.push_back(m_headAngVel.Length() * 57.2958f);
                    }
                }
            }
            if (ts != m_prevSample.clientTsNs) {
                m_prevSample = {ts, q, p};
            }
        }

        ovrPoseStatef GetHmdPose(double absTime) const override {
            ovrPoseStatef latched;
            uint64_t clientTs = 0;
            const bool fromHost = ReadHostHead(latched, clientTs);
            if (fromHost) {
                // Remember which client tracking sample the app is about to render with: the frame it submits
                // must carry this timestamp so the headset can reproject against the right pose.
                m_lastPoseClientTsNs = clientTs;
                if (m_ipc.state && m_ipc.state->host.stampMode == 2) {
                    // Extrapolate the sample to the time of the sample the frame will be stamped with (SubmitFrame measures that
                    // horizon per app: its render pipelining), so the stamp-time rotation of the layers stays small and the
                    // content's viewpoint (which the rotation cannot fix) is closer to the stamped pose. The rotation at
                    // SubmitFrame removes whatever error is left, so a wrong guess costs nothing but a larger warp.
                    std::lock_guard lock(m_lastHeadMutex);
                    UpdateHeadVelocity(clientTs, latched);
                    const double h = std::clamp(m_predictHorizonS.load(), 0.0, 0.06);
                    latched.AngularVelocity = m_headAngVel;
                    latched.LinearVelocity = m_headLinVel;
                    latched.TimeInSeconds = absTime - h; // PropagatePose integrates exactly h
                    m_diagPredicted++;
                }
            } else {
                std::shared_lock lock(m_hmdPoseMutex);
                latched = m_hmdPose;
            }
            ovrPoseStatef result = PropagatePose(latched, absTime);
            if (fromHost) {
                // what the app sees as head velocity stays zero (previous behaviour: the headset reprojects against the pose
                // we stamp, and apps extrapolating with a velocity would fight that)
                result.AngularVelocity = result.LinearVelocity = {0.f, 0.f, 0.f};
            }
            {
                std::lock_guard lock(m_lastHeadMutex);
                m_lastHeadPose = result.ThePose;
                if (clientTs) {
                    // history of what the app read, for FrameClientTimestamp (skip exact repeats of the newest entry)
                    const OVR::Quatf q = result.ThePose.Orientation;
                    const PoseSample* newest = m_poseRingCount ? &m_poseRing[(m_poseRingHead + kPoseRing - 1) % kPoseRing] : nullptr;
                    if (!newest || newest->clientTsNs != clientTs || newest->displayTime != absTime ||
                        std::abs(newest->orientation.Dot(q)) < 0.9999999f) {
                        m_poseRing[m_poseRingHead] = {clientTs, q, absTime};
                        m_poseRingHead = (m_poseRingHead + 1) % kPoseRing;
                        m_poseRingCount = std::min(m_poseRingCount + 1, kPoseRing);
                    }
                }
            }
            return result;
        }

        bool HasController(ovrHandType side) const override {
            std::shared_lock lock(m_controllerMutex);
            return m_controllerSides & (1 << side);
        }

        ovrPoseStatef GetControllerPose(ovrHandType side, double absTime) const override {
            ovrPoseStatef latched;
            const bool tracked = m_ipc.state && ReadHostHand(side, latched); // the headset tracks this controller
            if (tracked) {
                // pose from the host
            } else if (m_ipc.state) {
                // No tracked controller from the headset: hold the controllers at a fixed offset from the head.
                ovrPoseStatef head;
                uint64_t ts;
                if (!ReadHostHead(head, ts)) {
                    head = m_hmdPose;
                }
                latched = head;
                latched.ThePose = OVR::Posef(head.ThePose) * (side == ovrHand_Left ? k_HeadToLeftController : k_HeadToRightController);
            } else {
                std::shared_lock lock(m_controllerMutex);
                latched = m_controllerPose[side];
            }
            return PropagatePose(latched, absTime);
        }

        ovrInputState GetControllerButtons() const override {
            ovrInputState inputState = m_controllerButtons;
            if (m_ipc.state) {
                using namespace visionalvr_ipc;
                const auto& h = m_ipc.state->host;
                unsigned int buttons = 0, touches = 0;
                for (uint32_t side = 0; side < ovrHand_Count; side++) {
                    const HandInput in = h.handInput[side];
                    const bool left = side == ovrHand_Left;
                    if (in.clicks & kClickPrimary) buttons |= left ? ovrButton_X : ovrButton_A;
                    if (in.clicks & kClickSecondary) buttons |= left ? ovrButton_Y : ovrButton_B;
                    if (in.clicks & kClickThumbstick) buttons |= left ? ovrButton_LThumb : ovrButton_RThumb;
                    if ((in.clicks & kClickMenu) && left) buttons |= ovrButton_Enter;
                    if ((in.clicks & kClickSystem) && !left) buttons |= ovrButton_Home;
                    if (in.touches & kTouchPrimary) touches |= left ? ovrTouch_X : ovrTouch_A;
                    if (in.touches & kTouchSecondary) touches |= left ? ovrTouch_Y : ovrTouch_B;
                    if (in.touches & kTouchThumbstick) touches |= left ? ovrTouch_LThumb : ovrTouch_RThumb;
                    if (in.touches & kTouchTrigger) touches |= left ? ovrTouch_LIndexTrigger : ovrTouch_RIndexTrigger;
                    if (in.touches & kTouchThumbrest) touches |= left ? ovrTouch_LThumbRest : ovrTouch_RThumbRest;
                    inputState.IndexTriggerRaw[side] = in.trigger;
                    inputState.HandTriggerRaw[side] = in.squeeze;
                    inputState.ThumbstickRaw[side] = {in.stickX, in.stickY};
                }
                inputState.Buttons = buttons;
                inputState.Touches = touches;
                inputState.TimeInSeconds = QpcSeconds();
            }
            for (uint32_t side = 0; side < ovrHand_Count; side++) {
                inputState.IndexTrigger[side] = inputState.IndexTriggerNoDeadzone[side] =
                    inputState.IndexTriggerRaw[side];
                inputState.HandTrigger[side] = inputState.HandTriggerNoDeadzone[side] = inputState.HandTriggerRaw[side];
                inputState.Thumbstick[side] = inputState.ThumbstickNoDeadzone[side] = inputState.ThumbstickRaw[side];
            }
            return inputState;
        }

        void SetControllerVibration(ovrHandType side, float frequency, float amplitude) override {
            // VDXR re-asserts the current vibration every sync; the host turns changes into ALVR haptic pulses.
            if (m_ipc.state && (side == ovrHand_Left || side == ovrHand_Right)) {
                auto& hap = m_ipc.state->shim.haptic[side];
                hap.frequency = frequency;
                hap.amplitude = amplitude;
                MemoryBarrier();
                hap.seq++;
            }
        }

      private:
        std::thread m_serverThread;
        std::atomic<bool> m_terminateServerThread = false;

        LUID m_adapterLuid{};
        ComPtr<ID3D11Device> m_submissionDevice;
        ComPtr<ID3D11DeviceContext> m_submissionContext;
        ComPtr<ID3D11VertexShader> m_reprojectVS;
        ComPtr<ID3D11PixelShader> m_reprojectPS;
        ComPtr<ID3D11VertexShader> m_layerVS;
        ComPtr<ID3D11PixelShader> m_layerPS;
        ComPtr<ID3D11PixelShader> m_layerCubePS;
        ComPtr<ID3D11Buffer> m_layerConstantsBuffer;
        ComPtr<ID3D11RasterizerState> m_noCullState;
        ComPtr<ID3D11Texture2D> m_linearTex;
        ComPtr<ID3D11RenderTargetView> m_linearRTV;
        ComPtr<ID3D11ShaderResourceView> m_linearSRV;
        mutable std::mutex m_lastHeadMutex;
        mutable ovrPosef m_lastHeadPose = OVR::Posef::Identity();
        ComPtr<ID3D11Buffer> m_constantsBuffer;
        ComPtr<ID3D11DepthStencilState> m_noDepthTestState;
        ComPtr<ID3D11BlendState> m_blendState;
        ComPtr<ID3D11SamplerState> m_linearSampler;

        float m_displayRate{};
        float m_photonsTime{0};
        float m_outputGamma{1.f};   // exponent written into the stream: 1 / (encoding gamma * user gamma)
        float m_encodingGamma{1.f}; // negotiated ALVR encoding gamma
        uint32_t m_hostConfigSeq = 0;
        ovrSizei m_recommendedResolution{};
        ovrFovPort m_eyeFov[ovrEye_Count]{};
        bool m_useStageTracking{false};

        mutable std::shared_mutex m_swapchainMutex;
        std::unordered_set<void*> m_swapchains;
        ComPtr<ID3D11RenderTargetView> m_mirrorRTV;
        uint32_t m_mirrorWidth{0};
        uint32_t m_mirrorHeight{0};

        mutable std::shared_mutex m_controllerMutex;
        uint32_t m_controllerSides{0};
        ovrPoseStatef m_controllerPose[ovrHand_Count]{{OVR::Posef::Identity()}, {OVR::Posef::Identity()}};
        ovrInputState m_controllerButtons{};

        mutable std::shared_mutex m_hmdPoseMutex;
        ovrPoseStatef m_hmdPose{OVR::Posef::Identity()};
        ovrPosef m_eyePose[ovrEye_Count]{OVR::Posef::Identity(), OVR::Posef::Identity()};

        mutable std::mutex m_frameMutex;
        long long m_lastSignaledVsync{};
        long long m_lastWaitedFrame{};
        std::atomic<bool> m_submittedSinceWait{false};
        uint64_t m_waitCalls{0}, m_waitWaited{0}, m_waitRepeatedIndex{0}, m_waitTimeouts{0};
        double m_statT0{0};
        long long m_statTicks0{0};
        std::atomic<uint32_t> m_vsyncFromHost{0}, m_vsyncTimeouts{0};
        std::string m_lastLayerSig;
        double m_lastLayerLogS{0};
        int m_layerLogs{0};
        double m_nextFramePredictedDisplayTime{0.0};
        double m_vsyncInterval{1.0 / 72.0}; // latest tick-to-tick interval (host-scheduled), nominal until the host reports one
        std::condition_variable m_frameVsync;

        // Host IPC and the composed side-by-side frame ring.
        visionalvr_ipc::Ipc m_ipc{};
        mutable std::atomic<uint64_t> m_lastPoseClientTsNs{0};
        uint64_t m_framesPublished{0}; // frames handed to the host (frameCounter)
        uint64_t m_framesSubmitted{0}; // frames composed (picks the ring slot)
        // head poses the app read, with their client timestamps (guarded by m_lastHeadMutex)
        struct PoseSample {
            uint64_t clientTsNs;
            OVR::Quatf orientation;
            double displayTime; // the absTime the app asked the pose for (its frame's predicted display time)
        };
        uint64_t m_tsByDisplayTime{0}; // frames matched through SensorSampleTime (FrameClientTimestamp)
        // matching diagnostics since the last stats line: residual angle of the match (millidegrees), which sample was picked
        // (0 = the newest the app had read), its age behind the newest, and how many recorded poses tied with it
        std::vector<float> m_diagAngleMdeg, m_diagAgeMs;
        uint32_t m_diagPickIdx[4]{};   // picked sample index 0, 1, 2, 3+
        uint32_t m_diagTies[3]{};      // 1, 2, 3+ poses within the tie window
        uint32_t m_diagNoDisplayTime{0}; // no recorded pose for the frame's display time
        uint32_t m_diagSameTs{0};        // frames stamped with the previous frame's timestamp (the client treats them as shown)
        uint64_t m_lastStampTs{0};       // client timestamp the previous frame was stamped with
        std::vector<float> m_diagWarpMdeg, m_diagWarpAgeMs, m_diagFreshWaitMs; // stampMode 1 diagnostics
        uint32_t m_diagFreshWaits{0}, m_diagStillStale{0};
        uint32_t m_diagPublishDropped{0}; // frames dropped before publish because 3 were already in flight (never blocks the game)
        uint32_t m_gpuThrottleWaits{0}, m_gpuThrottleTimeouts{0}; // WaitForVsync GPU throttle (frames in flight <= 2)
        // stampMode 2: head velocity from the tracking samples and the extrapolation horizon (see GetHmdPose)
        struct SampleRef {
            uint64_t clientTsNs{0};
            OVR::Quatf q;
            OVR::Vector3f p;
        };
        mutable SampleRef m_prevSample;
        mutable OVR::Vector3f m_headAngVel{0.f, 0.f, 0.f}, m_headLinVel{0.f, 0.f, 0.f}; // world space, smoothed
        mutable std::atomic<double> m_predictHorizonS{1.0 / 90.0};
        mutable std::vector<float> m_diagOmegaDps;
        mutable uint32_t m_diagPredicted{0};
        static constexpr size_t kPoseRing = 256;
        mutable PoseSample m_poseRing[kPoseRing]{};
        mutable size_t m_poseRingHead{0};
        mutable size_t m_poseRingCount{0};
        uint64_t m_lastFrameTsNs{0}; // timestamp of the previous frame (frames never go back in time)
        // async publish: frames composed but not yet GPU-complete, oldest first
        // per-frame telemetry row (shim_<exe>_<pid>_frames.csv), completed by the publisher with the GPU-done / publish times
        struct FrameRow {
            long long ovrFrame{-1};
            double tSubmit{0};
            uint64_t tsStamp{0}, tsRender{0};
            float warpMdeg{0}, horizonMs{0}, freshWaitMs{0};
            uint32_t layers{0};
            bool dup{false};
            uint32_t stampMode{0};
        };
        struct PendingFrame {
            uint32_t slot;
            uint64_t clientTs;
            double composeStart;
            FrameRow row;
        };

        void WriteFrameRow(const FrameRow& r, double gpuDone, double published) {
            const vatrace::Frame t = vatrace::Take(r.ovrFrame);
            char b[1024];
            snprintf(b, sizeof(b),
                     "%llu,%lld,%.6f,%.6f,%.6f,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%llu,%llu,%.1f,%.2f,%.2f,%u,%d,%u\n",
                     (unsigned long long)m_framesPublished, r.ovrFrame, t.waitRet, t.predDisplay, t.begin, t.locateCount, t.locateFirst,
                     t.locateLast, t.locateDisplay, t.xrEnd, t.endDisplay, t.endSubmit, t.asyncWaitRet, t.asyncEnd, r.tSubmit, gpuDone,
                     published, (unsigned long long)r.tsStamp, (unsigned long long)r.tsRender, r.warpMdeg, r.horizonMs, r.freshWaitMs,
                     r.layers, r.dup ? 1 : 0, r.stampMode);
            shimlog::Csv(b);
        }
        ComPtr<IDXGIDevice2> m_dxgiDevice2;
        HANDLE m_gpuDone[visionalvr_ipc::kSlots]{};
        std::thread m_publishThread;
        std::mutex m_publishMutex;
        std::condition_variable m_publishCv;
        std::deque<PendingFrame> m_publishQueue;
        bool m_stopPublisher{false};
        uint32_t m_sbsWidth{0};
        uint32_t m_sbsHeight{0};
        ComPtr<ID3D11Texture2D> m_sbsTex[visionalvr_ipc::kSlots];
        ComPtr<ID3D11RenderTargetView> m_sbsRTV[visionalvr_ipc::kSlots];
        ComPtr<ID3D11Query> m_doneQuery;
    };

} // namespace

namespace ovrnull::driver {

    IDriver* CreateDriver() {
        return new HostDriver();
    }

} // namespace ovrnull::driver

// Audio endpoints. VDXR answers XR_OCULUS_audio_device_guid with ovr_GetAudioDeviceOutGuidStr. Unity's OpenXR plugin calls
// it every frame and looks the string up among the Windows endpoints; OVRNull's stub returned an empty string (and zeroed
// only sizeof(pointer) bytes), the lookup failed every frame ("[XR] GetAduioDeviceGUIDFromIDString Failed.") and its
// failure path corrupted the heap after a while (Unity games crashing in UnityOpenXR -> MMDevAPI -> PropVariantClear).
// The headset's audio is ALVR's capture of the Windows default output, so that endpoint is the right answer; the default
// input is the one ALVR's microphone feeds. stage11 removes OVRNull's versions of these two functions.
namespace {
    struct EndpointIdCache {
        std::mutex mutex;
        std::wstring id;
        ULONGLONG at{0};
    };

    // IMMDevice::GetId of the default endpoint for `flow`, refreshed at most once a second (callers poll every frame).
    std::wstring DefaultEndpointId(EDataFlow flow) {
        static EndpointIdCache caches[2];
        auto& c = caches[flow == eCapture ? 1 : 0];
        std::lock_guard lock(c.mutex);
        const ULONGLONG now = GetTickCount64();
        if (c.at != 0 && now - c.at < 1000) {
            return c.id;
        }
        c.at = now;
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // RPC_E_CHANGED_MODE: the thread already has COM
        std::wstring id;
        {
            ComPtr<IMMDeviceEnumerator> enumerator;
            ComPtr<IMMDevice> device;
            LPWSTR raw = nullptr;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
                SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device)) && SUCCEEDED(device->GetId(&raw))) {
                id = raw;
                CoTaskMemFree(raw);
            }
        }
        if (SUCCEEDED(init)) {
            CoUninitialize();
        }
        if (id != c.id) {
            shimlog::Debug("audio: default %s endpoint %s", flow == eCapture ? "input" : "output",
                           id.empty() ? "(none)" : shimlog::Utf8(id).c_str());
        }
        c.id = id;
        return id;
    }

    ovrResult CopyEndpointId(EDataFlow flow, WCHAR* buffer) {
        if (!buffer) {
            return ovrError_InvalidParameter;
        }
        ZeroMemory(buffer, OVR_AUDIO_MAX_DEVICE_STR_SIZE * sizeof(WCHAR));
        const std::wstring id = DefaultEndpointId(flow);
        // no endpoint: an empty string, as before (an error would make VDXR fail the OpenXR call every frame)
        if (!id.empty() && id.size() < OVR_AUDIO_MAX_DEVICE_STR_SIZE) {
            wcsncpy_s(buffer, OVR_AUDIO_MAX_DEVICE_STR_SIZE, id.c_str(), _TRUNCATE);
        }
        return ovrSuccess;
    }
} // namespace

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetAudioDeviceOutGuidStr(WCHAR deviceOutStrBuffer[OVR_AUDIO_MAX_DEVICE_STR_SIZE]) {
    return CopyEndpointId(eRender, deviceOutStrBuffer);
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetAudioDeviceInGuidStr(WCHAR deviceInStrBuffer[OVR_AUDIO_MAX_DEVICE_STR_SIZE]) {
    return CopyEndpointId(eCapture, deviceInStrBuffer);
}
