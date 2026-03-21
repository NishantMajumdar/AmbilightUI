#include "pch.h"
#include "AmbilightEngine.h"

#undef min
#undef max

#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <cmath>
#include <string>
#include <algorithm>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <comdef.h>
#include <Wbemidl.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "wbemuuid.lib")

const int BAUDRATE = 500000;
const int FPS_TARGET = 55;
const int LAYERS = 10;
const float MAX_DEPTH = 0.50f;
const float GAMMA_VAL = 2.2f;
const int PIXEL_STEP = 16;

const int COVER_THRESHOLD = 500;
const float HOLD_TIME = 3.0f;
const float FADE_SPEED = 0.05f;

struct RGBColor { float r, g, b; };

std::chrono::time_point<std::chrono::steady_clock> cover_start_time;
bool is_covering = false;
float brightness_scale = 1.0f;
int ldr_value = 1000;

std::vector<RGBColor> global_leds;
bool new_frame_ready = false;
std::mutex frame_mutex;

std::vector<float> weights;
float weight_sum = 0;

std::vector<uint8_t> gpu_rgb = { 0, 255, 0 };
std::mutex serial_mutex;
HANDLE hSerial = INVALID_HANDLE_VALUE;
uint8_t gamma_table[256];

// --- HELPER: MATH FOR RAINBOW EFFECTS ---
RGBColor HSVtoRGB(float H, float S, float V) {
    float C = S * V;
    float X = C * (1.0f - std::fabs(std::fmod(H / 60.0f, 2.0f) - 1.0f));
    float m = V - C;
    float r = 0, g = 0, b = 0;
    if (H >= 0 && H < 60) { r = C; g = X; }
    else if (H >= 60 && H < 120) { r = X; g = C; }
    else if (H >= 120 && H < 180) { g = C; b = X; }
    else if (H >= 180 && H < 240) { g = X; b = C; }
    else if (H >= 240 && H < 300) { r = X; b = C; }
    else { r = C; b = X; }
    return { (r + m) * 255.0f, (g + m) * 255.0f, (b + m) * 255.0f };
}

void init_gamma_table() {
    for (int i = 0; i < 256; i++) {
        gamma_table[i] = static_cast<uint8_t>(pow(i / 255.0f, GAMMA_VAL) * 255.0f);
    }
}

std::vector<uint8_t> adalight_header(int count) {
    if (count <= 0) return { 'A', 'd', 'a', 0, 0, 0x55 };
    uint8_t hi = ((count - 1) >> 8) & 0xFF;
    uint8_t lo = (count - 1) & 0xFF;
    uint8_t chk = hi ^ lo ^ 0x55;
    return { 'A', 'd', 'a', hi, lo, chk };
}

RGBColor get_average_color(const uint8_t* buffer, size_t buffer_size, int start_x, int start_y, int width, int height, int row_pitch, int step) {
    long long r_sum = 0, g_sum = 0, b_sum = 0;
    int count = 0;

    for (int y = start_y; y < start_y + height; y += step) {
        int row_offset = y * row_pitch;
        for (int x = start_x; x < start_x + width; x += step) {
            int offset = row_offset + (x * 4);
            if (offset + 2 < buffer_size) {
                b_sum += buffer[offset];
                g_sum += buffer[offset + 1];
                r_sum += buffer[offset + 2];
                count++;
            }
        }
    }
    if (count == 0) return { 0.0f, 0.0f, 0.0f };
    return { (float)r_sum / count, (float)g_sum / count, (float)b_sum / count };
}

void ConnectSerial() {
    if (hSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(hSerial);
        hSerial = INVALID_HANDLE_VALUE;
    }

    std::string portName = "\\\\.\\COM" + std::to_string(COM_PORT_NUM.load());
    hSerial = CreateFileA(portName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

    if (hSerial == INVALID_HANDLE_VALUE) return;

    DCB dcbSerialParams = { 0 };
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    GetCommState(hSerial, &dcbSerialParams);
    dcbSerialParams.BaudRate = BAUDRATE;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    SetCommState(hSerial, &dcbSerialParams);

    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(hSerial, &timeouts);

    PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
    std::this_thread::sleep_for(std::chrono::seconds(2));
}

// --- UNIVERSAL MULTI-GPU THERMAL MONITOR ---
void gpu_monitor() {
    HRESULT hres = CoInitializeEx(0, COINIT_MULTITHREADED);
    IWbemLocator* pLoc = NULL;
    hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc);
    if (FAILED(hres)) return;

    IWbemServices* pSvc = NULL;
    hres = pLoc->ConnectServer(_bstr_t(L"ROOT\\OpenHardwareMonitor"), NULL, NULL, 0, NULL, 0, 0, &pSvc);
    if (FAILED(hres)) { pLoc->Release(); return; }

    hres = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

    bool blink_state = false;
    auto last_blink = std::chrono::steady_clock::now();

    while (true) {
        IEnumWbemClassObject* pEnumerator = NULL;

        // GENERIC QUERY: Pull EVERY temperature sensor on the PC. We will filter manually.
        hres = pSvc->ExecQuery(bstr_t("WQL"), bstr_t("SELECT * FROM Sensor WHERE SensorType='Temperature'"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator);

        float max_temp = 0.0f;
        bool found_temp = false;

        if (pEnumerator) {
            IWbemClassObject* pclsObj = NULL;
            ULONG uReturn = 0;

            while (pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn) == S_OK && uReturn > 0) {
                VARIANT vtName, vtId, vtVal;
                VariantInit(&vtName); VariantInit(&vtId); VariantInit(&vtVal);

                pclsObj->Get(L"Name", 0, &vtName, 0, 0);
                pclsObj->Get(L"Identifier", 0, &vtId, 0, 0);
                pclsObj->Get(L"Value", 0, &vtVal, 0, 0);

                std::wstring name = (vtName.vt == VT_BSTR && vtName.bstrVal) ? vtName.bstrVal : L"";
                std::wstring id = (vtId.vt == VT_BSTR && vtId.bstrVal) ? vtId.bstrVal : L"";

                // Convert names to lowercase for a guaranteed match
                std::transform(name.begin(), name.end(), name.begin(), ::towlower);
                std::transform(id.begin(), id.end(), id.begin(), ::towlower);

                // AGGRESSIVE CATCH-ALL: Looks for AMD, Nvidia, Intel, or Generic Video markers
                if (name.find(L"gpu") != std::wstring::npos || id.find(L"gpu") != std::wstring::npos ||
                    id.find(L"video") != std::wstring::npos || name.find(L"radeon") != std::wstring::npos ||
                    name.find(L"geforce") != std::wstring::npos) {

                    if (SUCCEEDED(VariantChangeType(&vtVal, &vtVal, 0, VT_R4))) {
                        float temp = vtVal.fltVal;
                        if (temp > max_temp) {
                            max_temp = temp;
                            found_temp = true;
                        }
                    }
                }

                VariantClear(&vtName); VariantClear(&vtId); VariantClear(&vtVal);
                pclsObj->Release();
            }
            pEnumerator->Release();
        }

        // PUSH SAFELY TO UI GLOBALS
        if (found_temp) {
            CURRENT_GPU_TEMP.store(max_temp);

            std::vector<uint8_t> new_rgb;
            if (max_temp >= 85.0f) {
                auto now = std::chrono::steady_clock::now();
                std::chrono::duration<float> elapsed = now - last_blink;
                if (elapsed.count() > 0.5f) { blink_state = !blink_state; last_blink = now; }
                new_rgb = blink_state ? std::vector<uint8_t>{255, 0, 0} : std::vector<uint8_t>{ 0, 0, 0 };
            }
            else {
                float r = 0, g = 0, b = 0;
                if (max_temp <= 50.0f) { r = 0; g = 255; b = 0; }
                else if (max_temp < 60.0f) { float t = (max_temp - 57.0f) / 10.0f; r = 0 + (255 - 0) * t; g = 255; }
                else if (max_temp < 70.0f) { float t = (max_temp - 60.0f) / 10.0f; r = 255; g = 255 + (128 - 255) * t; }
                else if (max_temp < 80.0f) { float t = (max_temp - 70.0f) / 10.0f; r = 255; g = 128 + (0 - 128) * t; }
                else { r = 255; g = 0; b = 0; }
                new_rgb = { static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b) };
            }

            std::lock_guard<std::mutex> lock(serial_mutex);
            gpu_rgb = new_rgb;
        }
        else {
            // Failsafe: if no GPU is found (or it is sleeping), reset values so UI shows offline
            CURRENT_GPU_TEMP.store(0.0f);
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void capture_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d3dDevice, nullptr, &d3dContext);

    IDXGIDevice* dxgiDevice = nullptr; d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    IDXGIAdapter* dxgiAdapter = nullptr; dxgiDevice->GetAdapter(&dxgiAdapter);
    IDXGIOutput* dxgiOutput = nullptr; dxgiAdapter->EnumOutputs(0, &dxgiOutput);
    IDXGIOutput1* dxgiOutput1 = nullptr; dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&dxgiOutput1);

    IDXGIOutputDuplication* deskDupl = nullptr;
    dxgiOutput1->DuplicateOutput(d3dDevice, &deskDupl);

    D3D11_TEXTURE2D_DESC fd;
    ID3D11Texture2D* stagingTex[2] = { nullptr, nullptr };
    bool frame_ready[2] = { false, false };
    int write_idx = 0;

    while (true) {
        if (DISPLAY_MODE.load() != 0) {
            if (deskDupl) { deskDupl->Release(); deskDupl = nullptr; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (!deskDupl) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            dxgiOutput1->DuplicateOutput(d3dDevice, &deskDupl);
            continue;
        }

        IDXGIResource* desktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        HRESULT hr = deskDupl->AcquireNextFrame(0, &frameInfo, &desktopResource);

        if (hr == S_OK && desktopResource != nullptr) {
            ID3D11Texture2D* acquireTex = nullptr;
            desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&acquireTex);
            acquireTex->GetDesc(&fd);

            if (stagingTex[0] == nullptr) {
                fd.Usage = D3D11_USAGE_STAGING;
                fd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                fd.BindFlags = 0; fd.MiscFlags = 0;
                d3dDevice->CreateTexture2D(&fd, nullptr, &stagingTex[0]);
                d3dDevice->CreateTexture2D(&fd, nullptr, &stagingTex[1]);
            }

            d3dContext->CopyResource(stagingTex[write_idx], acquireTex);
            frame_ready[write_idx] = true;
            int read_idx = (write_idx + 1) % 2;

            if (frame_ready[read_idx]) {
                D3D11_MAPPED_SUBRESOURCE map;
                d3dContext->Map(stagingTex[read_idx], 0, D3D11_MAP_READ, 0, &map);

                int w = fd.Width;
                int h = fd.Height;
                int pitch = map.RowPitch;
                size_t buffer_size = (size_t)pitch * h;
                int layer_thickness = (int)(h * MAX_DEPTH) / LAYERS;

                int countL = std::max(0, LED_LEFT.load());
                int countT = std::max(0, LED_TOP.load());
                int countR = std::max(0, LED_RIGHT.load());
                int countB = std::max(0, LED_BOTTOM.load());

                const uint8_t* pData = reinterpret_cast<const uint8_t*>(map.pData);

                std::vector<RGBColor> top_leds, bottom_leds, left_leds, right_leds;

                if (countL > 0) {
                    int seg_h = h / countL;
                    for (int l = 0; l < countL; l++) {
                        float r = 0, g = 0, b = 0;
                        for (int layer = 0; layer < LAYERS; layer++) {
                            int x1 = layer * layer_thickness; int y1 = l * seg_h;
                            RGBColor c = get_average_color(pData, buffer_size, x1, y1, layer_thickness, seg_h, pitch, PIXEL_STEP);
                            r += c.r * weights[layer]; g += c.g * weights[layer]; b += c.b * weights[layer];
                        }
                        left_leds.push_back({ r / weight_sum, g / weight_sum, b / weight_sum });
                    }
                }

                if (countT > 0) {
                    int seg_w = w / countT;
                    for (int t = 0; t < countT; t++) {
                        float r = 0, g = 0, b = 0;
                        for (int layer = 0; layer < LAYERS; layer++) {
                            int y1 = layer * layer_thickness; int x1 = t * seg_w;
                            RGBColor c = get_average_color(pData, buffer_size, x1, y1, seg_w, layer_thickness, pitch, PIXEL_STEP);
                            r += c.r * weights[layer]; g += c.g * weights[layer]; b += c.b * weights[layer];
                        }
                        top_leds.push_back({ r / weight_sum, g / weight_sum, b / weight_sum });
                    }
                }

                if (countR > 0) {
                    int seg_h = h / countR;
                    for (int ri = 0; ri < countR; ri++) {
                        float r = 0, g = 0, b = 0;
                        for (int layer = 0; layer < LAYERS; layer++) {
                            int x2 = w - (layer * layer_thickness); int x1 = x2 - layer_thickness; int y1 = ri * seg_h;
                            RGBColor c = get_average_color(pData, buffer_size, x1, y1, layer_thickness, seg_h, pitch, PIXEL_STEP);
                            r += c.r * weights[layer]; g += c.g * weights[layer]; b += c.b * weights[layer];
                        }
                        right_leds.push_back({ r / weight_sum, g / weight_sum, b / weight_sum });
                    }
                }

                if (countB > 0) {
                    int seg_w = w / countB;
                    for (int b_idx = 0; b_idx < countB; b_idx++) {
                        float r = 0, g = 0, b = 0;
                        for (int layer = 0; layer < LAYERS; layer++) {
                            int y2 = h - (layer * layer_thickness); int y1 = y2 - layer_thickness; int x1 = b_idx * seg_w;
                            RGBColor c = get_average_color(pData, buffer_size, x1, y1, seg_w, layer_thickness, pitch, PIXEL_STEP);
                            r += c.r * weights[layer]; g += c.g * weights[layer]; b += c.b * weights[layer];
                        }
                        bottom_leds.push_back({ r / weight_sum, g / weight_sum, b / weight_sum });
                    }
                }

                d3dContext->Unmap(stagingTex[read_idx], 0);

                std::vector<RGBColor> temp_leds;
                temp_leds.reserve(countL + countT + countR + countB);

                int sp = START_POS.load();
                int dir = DIRECTION.load();

                auto add_segment = [&](const std::vector<RGBColor>& seg, bool reverse) {
                    if (reverse) temp_leds.insert(temp_leds.end(), seg.rbegin(), seg.rend());
                    else temp_leds.insert(temp_leds.end(), seg.begin(), seg.end());
                    };

                if (dir == 0) { // Clockwise
                    if (sp == 0) { // Top-Left
                        add_segment(top_leds, false); add_segment(right_leds, false); add_segment(bottom_leds, true); add_segment(left_leds, true);
                    }
                    else if (sp == 1) { // Top-Right
                        add_segment(right_leds, false); add_segment(bottom_leds, true); add_segment(left_leds, true); add_segment(top_leds, false);
                    }
                    else if (sp == 2) { // Bottom-Right
                        add_segment(bottom_leds, true); add_segment(left_leds, true); add_segment(top_leds, false); add_segment(right_leds, false);
                    }
                    else if (sp == 3) { // Bottom-Left
                        add_segment(left_leds, true); add_segment(top_leds, false); add_segment(right_leds, false); add_segment(bottom_leds, true);
                    }
                }
                else { // Anti-Clockwise
                    if (sp == 0) { // Top-Left
                        add_segment(left_leds, false); add_segment(bottom_leds, false); add_segment(right_leds, true); add_segment(top_leds, true);
                    }
                    else if (sp == 1) { // Top-Right
                        add_segment(top_leds, true); add_segment(left_leds, false); add_segment(bottom_leds, false); add_segment(right_leds, true);
                    }
                    else if (sp == 2) { // Bottom-Right
                        add_segment(right_leds, true); add_segment(top_leds, true); add_segment(left_leds, false); add_segment(bottom_leds, false);
                    }
                    else if (sp == 3) { // Bottom-Left
                        add_segment(bottom_leds, false); add_segment(right_leds, true); add_segment(top_leds, true); add_segment(left_leds, false);
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(frame_mutex);
                    global_leds = temp_leds;
                    new_frame_ready = true;
                }
            }

            write_idx = (write_idx + 1) % 2;
            acquireTex->Release();
            deskDupl->ReleaseFrame();

        }
        else if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
            deskDupl->Release(); deskDupl = nullptr;
            if (stagingTex[0]) { stagingTex[0]->Release(); stagingTex[0] = nullptr; }
            if (stagingTex[1]) { stagingTex[1]->Release(); stagingTex[1] = nullptr; }
            frame_ready[0] = false; frame_ready[1] = false;
        }
        if (desktopResource) desktopResource->Release();

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void StartAmbilightEngine() {
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    init_gamma_table();
    ConnectSerial();

    int total_leds = std::max(0, LED_LEFT.load()) + std::max(0, LED_TOP.load()) + std::max(0, LED_RIGHT.load()) + std::max(0, LED_BOTTOM.load());
    std::vector<uint8_t> header = adalight_header(total_leds);

    for (int i = 0; i < LAYERS; i++) weights.push_back(1.0f - (0.9f * ((float)i / (LAYERS - 1))));
    for (float w : weights) weight_sum += w;

    std::thread(gpu_monitor).detach();
    std::thread(capture_loop).detach();

    std::string ldr_buffer = "";
    std::vector<RGBColor> current_leds(total_leds, { 0,0,0 });
    std::vector<RGBColor> smoothed_leds(total_leds, { 0,0,0 });

    while (true) {
        auto start = std::chrono::steady_clock::now();

        if (GEOMETRY_CHANGED.load()) {
            total_leds = std::max(0, LED_LEFT.load()) + std::max(0, LED_TOP.load()) + std::max(0, LED_RIGHT.load()) + std::max(0, LED_BOTTOM.load());
            header = adalight_header(total_leds);

            smoothed_leds.clear();
            smoothed_leds.resize(total_leds, { 0,0,0 });
            current_leds.clear();
            current_leds.resize(total_leds, { 0,0,0 });

            GEOMETRY_CHANGED.store(false);
        }

        if (RECONNECT_SERIAL.load()) {
            ConnectSerial();
            RECONNECT_SERIAL.store(false);
        }

        static float effect_time = 0.0f;
        effect_time += 0.02f;

        int current_mode = DISPLAY_MODE.load();

        if (current_mode == 0) {
            std::lock_guard<std::mutex> lock(frame_mutex);
            if (new_frame_ready && global_leds.size() == total_leds) {
                current_leds = global_leds;
                new_frame_ready = false;
            }
        }
        else if (current_mode == 1) {
            RGBColor solid = { (float)SOLID_R.load(), (float)SOLID_G.load(), (float)SOLID_B.load() };
            for (int i = 0; i < total_leds; i++) current_leds[i] = solid;
        }
        else if (current_mode == 2) {
            int effect = CURRENT_EFFECT.load();
            if (effect == 0) {
                for (int i = 0; i < total_leds; i++) {
                    float hue = std::fmod((i * 5.0f) + (effect_time * 100.0f), 360.0f);
                    current_leds[i] = HSVtoRGB(hue, 1.0f, 1.0f);
                }
            }
            else if (effect == 1) {
                float brightness = (std::sin(effect_time * 3.0f) + 1.0f) / 2.0f;
                RGBColor breath = { SOLID_R.load() * brightness, SOLID_G.load() * brightness, SOLID_B.load() * brightness };
                for (int i = 0; i < total_leds; i++) current_leds[i] = breath;
            }
        }

        std::vector<uint8_t> payload;
        payload.reserve(total_leds * 3);

        bool masterPower = STRIP_ON.load();
        float dynamicBrightness = BRIGHTNESS.load();
        float dynamicSmoothing = SMOOTHING.load();
        float rGain = RED_GAIN.load();
        float gGain = GREEN_GAIN.load();
        float bGain = BLUE_GAIN.load();

        float target = masterPower ? 1.0f : 0.0f;
        if (brightness_scale < target) brightness_scale = std::min(target, brightness_scale + FADE_SPEED);
        else if (brightness_scale > target) brightness_scale = std::max(target, brightness_scale - FADE_SPEED);

        for (int i = 0; i < total_leds; i++) {
            smoothed_leds[i].r = (current_leds[i].r * dynamicSmoothing) + (smoothed_leds[i].r * (1.0f - dynamicSmoothing));
            smoothed_leds[i].g = (current_leds[i].g * dynamicSmoothing) + (smoothed_leds[i].g * (1.0f - dynamicSmoothing));
            smoothed_leds[i].b = (current_leds[i].b * dynamicSmoothing) + (smoothed_leds[i].b * (1.0f - dynamicSmoothing));

            float r_temp = smoothed_leds[i].r * rGain * dynamicBrightness;
            float g_temp = smoothed_leds[i].g * gGain * dynamicBrightness;
            float b_temp = smoothed_leds[i].b * bGain * dynamicBrightness;

            uint8_t r_out = gamma_table[std::clamp((int)r_temp, 0, 255)];
            uint8_t g_out = gamma_table[std::clamp((int)g_temp, 0, 255)];
            uint8_t b_out = gamma_table[std::clamp((int)b_temp, 0, 255)];

            payload.push_back((uint8_t)(r_out * brightness_scale));
            payload.push_back((uint8_t)(g_out * brightness_scale));
            payload.push_back((uint8_t)(b_out * brightness_scale));
        }

        uint8_t aux_r = 0, aux_g = 0, aux_b = 0;
        if (AUX_LED_MODE.load() == 1) {
            std::lock_guard<std::mutex> lock(serial_mutex);
            aux_r = gpu_rgb[0]; aux_g = gpu_rgb[1]; aux_b = gpu_rgb[2];
        }
        else {
            aux_r = AUX_STATIC_R.load(); aux_g = AUX_STATIC_G.load(); aux_b = AUX_STATIC_B.load();
        }
        std::vector<uint8_t> gpu_payload = { 'X', aux_r, aux_g, aux_b };

        std::vector<uint8_t> master_payload;
        master_payload.reserve(header.size() + payload.size() + gpu_payload.size());
        master_payload.insert(master_payload.end(), header.begin(), header.end());
        master_payload.insert(master_payload.end(), payload.begin(), payload.end());
        master_payload.insert(master_payload.end(), gpu_payload.begin(), gpu_payload.end());

        DWORD bytesWritten;
        std::lock_guard<std::mutex> lock(serial_mutex);
        if (hSerial != INVALID_HANDLE_VALUE) {

            DWORD errors;
            COMSTAT status;
            ClearCommError(hSerial, &errors, &status);

            if (status.cbInQue > 0) {
                char buf[256];
                DWORD bytesRead;
                DWORD toRead = std::min(status.cbInQue, (DWORD)sizeof(buf) - 1);

                if (ReadFile(hSerial, buf, toRead, &bytesRead, NULL) && bytesRead > 0) {
                    buf[bytesRead] = '\0';
                    ldr_buffer += buf;

                    size_t pos;
                    while ((pos = ldr_buffer.find('\n')) != std::string::npos) {
                        std::string line = ldr_buffer.substr(0, pos);
                        ldr_buffer.erase(0, pos + 1);

                        if (!line.empty() && line[0] == 'L') {
                            try {
                                ldr_value = std::stoi(line.substr(1));
                                CURRENT_LDR_VALUE.store(ldr_value);

                                if (ldr_value < COVER_THRESHOLD) {
                                    if (!is_covering) {
                                        cover_start_time = std::chrono::steady_clock::now();
                                        is_covering = true;
                                    }
                                    else {
                                        std::chrono::duration<float> elapsed = std::chrono::steady_clock::now() - cover_start_time;
                                        if (elapsed.count() >= HOLD_TIME) {
                                            STRIP_ON.store(true);
                                        }
                                    }
                                }
                                else {
                                    is_covering = false;
                                }
                            }
                            catch (...) {}
                        }
                    }
                }
            }
            WriteFile(hSerial, master_payload.data(), static_cast<DWORD>(master_payload.size()), &bytesWritten, NULL);
        }

        auto end = std::chrono::steady_clock::now();
        std::chrono::duration<float> elapsed = end - start;
        float frame_time = 1.0f / FPS_TARGET;
        if (elapsed.count() < frame_time) {
            std::this_thread::sleep_for(std::chrono::duration<float>(frame_time - elapsed.count()));
        }
    }
}