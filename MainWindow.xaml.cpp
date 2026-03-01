#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "AmbilightEngine.h"
#include <thread>
#include <fstream>
#include <cmath>

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h> // Required for finding the Documents folder
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#include <string>

// --- DYNAMIC CONFIG PATH GENERATOR ---
std::wstring GetConfigFilePath() {
    PWSTR path = NULL;
    std::wstring configPath = L"ambilight_config.txt"; // Fallback just in case

    // Get the user's Documents folder path safely
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &path))) {
        std::wstring docsFolder(path);
        CoTaskMemFree(path); // Free the memory allocated by the Windows API

        // Create an AmbilightEngine folder inside Documents
        std::wstring appFolder = docsFolder + L"\\AmbilightEngine";
        CreateDirectoryW(appFolder.c_str(), NULL);

        // Final path: C:\Users\<Name>\Documents\AmbilightEngine\ambilight_config.txt
        configPath = appFolder + L"\\ambilight_config.txt";
    }
    return configPath;
}

// --- AUTO-LAUNCH HARDWARE MONITOR ---
void LaunchHardwareMonitor() {
    HWND hwnd = FindWindowA(NULL, "Open Hardware Monitor");
    if (hwnd == NULL) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        std::wstring exeDir = path;
        exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/"));
        std::wstring ohmPath = exeDir + L"\\Drivers\\OpenHardwareMonitor.exe";

        ShellExecuteW(NULL, L"open", ohmPath.c_str(), NULL, NULL, SW_SHOW);
    }
}

// --- STARTUP REGISTRY HELPERS ---
bool IsStartupEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type, size;
        bool exists = (RegQueryValueExW(hKey, L"AmbilightEngine", NULL, &type, NULL, &size) == ERROR_SUCCESS);
        RegCloseKey(hKey);
        return exists;
    }
    return false;
}

void SetStartupRegistry(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            RegSetValueExW(hKey, L"AmbilightEngine", 0, REG_SZ, (BYTE*)exePath, (static_cast<DWORD>(wcslen(exePath)) + 1) * sizeof(wchar_t));
        }
        else {
            RegDeleteValueW(hKey, L"AmbilightEngine");
        }
        RegCloseKey(hKey);
    }
}

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;

#define WM_TRAYICON (WM_USER + 1)
#define IDM_OPEN 2001
#define IDM_QUIT 2002

HWND g_hWnd = nullptr;
NOTIFYICONDATAW g_nid = {};

LRESULT CALLBACK TrayWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (uMsg == WM_TRAYICON) {
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hWnd, SW_RESTORE);
            SetForegroundWindow(hWnd);
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
        }
        else if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_OPEN, L"Open Control Panel");
            InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_QUIT, L"Quit Ambilight Engine");

            SetForegroundWindow(hWnd);
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(hMenu);
        }
    }
    else if (uMsg == WM_COMMAND) {
        int wmId = LOWORD(wParam);
        if (wmId == IDM_OPEN) {
            ShowWindow(hWnd, SW_RESTORE);
            SetForegroundWindow(hWnd);
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
        }
        else if (wmId == IDM_QUIT) {
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            system("taskkill /IM OpenHardwareMonitor.exe /F");
            ExitProcess(0);
        }
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

namespace winrt::AmbilightUI::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
        LoadSettings();

        MasterPowerToggle().IsOn(STRIP_ON.load());
        StyleToggle().IsOn(true);
        BrightnessSlider().Value(BRIGHTNESS.load());
        SmoothingSlider().Value(SMOOTHING.load());
        RedGainSlider().Value(RED_GAIN.load());
        GreenGainSlider().Value(GREEN_GAIN.load());
        BlueGainSlider().Value(BLUE_GAIN.load());
        ComPortBox().SelectedIndex(COM_PORT_NUM.load() - 1);
        AuxModeBox().SelectedIndex(AUX_LED_MODE.load() == 1 ? 1 : 0);

        TopLedBox().Value(LED_TOP.load());
        LeftLedBox().Value(LED_LEFT.load());
        RightLedBox().Value(LED_RIGHT.load());
        BottomLedBox().Value(LED_BOTTOM.load());

        StartPosBox().SelectedIndex(START_POS.load());
        DirectionBox().SelectedIndex(DIRECTION.load());

        try { StartupToggle().IsOn(IsStartupEnabled()); }
        catch (...) {}

        try {
            ModeBox().SelectedIndex(DISPLAY_MODE.load());
            EffectBox().SelectedIndex(CURRENT_EFFECT.load());
            SolidColorButton().Visibility(DISPLAY_MODE.load() == 1 ? Visibility::Visible : Visibility::Collapsed);
            EffectBox().Visibility(DISPLAY_MODE.load() == 2 ? Visibility::Visible : Visibility::Collapsed);
        }
        catch (...) {}

        Windows::UI::Color auxColor;
        auxColor.A = 255; auxColor.R = AUX_STATIC_R.load(); auxColor.G = AUX_STATIC_G.load(); auxColor.B = AUX_STATIC_B.load();
        AuxColorPicker().Color(auxColor);
        AuxColorPicker().IsEnabled(AUX_LED_MODE.load() == 0);

        Windows::UI::Color stripColor;
        stripColor.A = 255; stripColor.R = SOLID_R.load(); stripColor.G = SOLID_G.load(); stripColor.B = SOLID_B.load();
        try { StripColorPicker().Color(stripColor); }
        catch (...) {}

        m_isReady = true;
        NavView().SelectedItem(NavView().MenuItems().GetAt(0));

        auto dispatcherQueue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        m_syncTimer = dispatcherQueue.CreateTimer();
        m_syncTimer.Interval(std::chrono::milliseconds(200));
        m_syncTimer.Tick({ this, &MainWindow::OnSyncTimerTick });
        m_syncTimer.Start();

        auto windowNative = this->try_as<::IWindowNative>();
        if (windowNative) windowNative->get_WindowHandle(&g_hWnd);

        winrt::Microsoft::UI::WindowId windowId = winrt::Microsoft::UI::GetWindowIdFromWindow(g_hWnd);
        auto appWindow = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);
        appWindow.Closing({ this, &MainWindow::AppWindow_Closing });

        SetWindowSubclass(g_hWnd, TrayWndProc, 1, 0);

        LaunchHardwareMonitor();

        std::thread engineThread(StartAmbilightEngine);
        engineThread.detach();
    }

    void MainWindow::OnSyncTimerTick(winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const&, IInspectable const&)
    {
        if (!m_isReady) return;

        if (MasterPowerToggle().IsOn() != STRIP_ON.load()) {
            bool wasReady = m_isReady;
            m_isReady = false;
            MasterPowerToggle().IsOn(STRIP_ON.load());
            StatusText().Text(STRIP_ON.load() ? L"Status: Active" : L"Status: Inactive");
            StatusText().Foreground(STRIP_ON.load() ? SolidColorBrush(Colors::LightGreen()) : SolidColorBrush(Colors::Gray()));
            m_isReady = wasReady;
        }

        // --- 1. Update LDR Data ---
        int currentLdr = CURRENT_LDR_VALUE.load();
        LdrValueText().Text(L"Current Value: " + winrt::to_hstring(currentLdr));
        LdrProgressBar().Value(currentLdr);

        // --- 2. Update GPU Temp Data (Failsafe Formatter) ---
        float currentGpuTemp = CURRENT_GPU_TEMP.load();

        if (currentGpuTemp > 0.0f) {
            // Using \u00B0 for the degree symbol to prevent encoding glitches
            GpuTempText().Text(L"Current Temp: " + winrt::to_hstring((int)currentGpuTemp) + L" \u00B0C");
            GpuTempProgressBar().Value(currentGpuTemp);
        }
        else {
            GpuTempText().Text(L"Current Temp: -- \u00B0C");
            GpuTempProgressBar().Value(20);
        }
    }

    void MainWindow::SaveSettings() {
        if (!m_isReady) return;

        std::wstring configPath = GetConfigFilePath();
        std::ofstream file(configPath); // Uses the new Documents path

        file << STRIP_ON.load() << "\n" << BRIGHTNESS.load() << "\n" << SMOOTHING.load() << "\n"
            << RED_GAIN.load() << "\n" << GREEN_GAIN.load() << "\n" << BLUE_GAIN.load() << "\n"
            << COM_PORT_NUM.load() << "\n" << AUX_LED_MODE.load() << "\n"
            << (int)AUX_STATIC_R.load() << "\n" << (int)AUX_STATIC_G.load() << "\n" << (int)AUX_STATIC_B.load() << "\n"
            << LED_TOP.load() << "\n" << LED_LEFT.load() << "\n" << LED_RIGHT.load() << "\n" << LED_BOTTOM.load() << "\n"
            << START_POS.load() << "\n" << DIRECTION.load() << "\n"
            << DISPLAY_MODE.load() << "\n" << (int)SOLID_R.load() << "\n" << (int)SOLID_G.load() << "\n" << (int)SOLID_B.load() << "\n"
            << CURRENT_EFFECT.load();
    }

    void MainWindow::LoadSettings() {
        std::wstring configPath = GetConfigFilePath();
        std::ifstream file(configPath); // Uses the new Documents path

        if (file.is_open()) {
            bool bVal; float fVal; int iVal;
            if (file >> bVal) STRIP_ON = bVal;
            if (file >> fVal) BRIGHTNESS = fVal;
            if (file >> fVal) SMOOTHING = fVal;
            if (file >> fVal) RED_GAIN = fVal;
            if (file >> fVal) GREEN_GAIN = fVal;
            if (file >> fVal) BLUE_GAIN = fVal;
            if (file >> iVal) COM_PORT_NUM = iVal;
            if (file >> iVal) AUX_LED_MODE = iVal;
            if (file >> iVal) AUX_STATIC_R = static_cast<uint8_t>(iVal);
            if (file >> iVal) AUX_STATIC_G = static_cast<uint8_t>(iVal);
            if (file >> iVal) AUX_STATIC_B = static_cast<uint8_t>(iVal);
            if (file >> iVal) LED_TOP = iVal;
            if (file >> iVal) LED_LEFT = iVal;
            if (file >> iVal) LED_RIGHT = iVal;
            if (file >> iVal) LED_BOTTOM = iVal;
            if (file >> iVal) START_POS = iVal;
            if (file >> iVal) DIRECTION = iVal;
            if (file >> iVal) DISPLAY_MODE = iVal;
            if (file >> iVal) SOLID_R = static_cast<uint8_t>(iVal);
            if (file >> iVal) SOLID_G = static_cast<uint8_t>(iVal);
            if (file >> iVal) SOLID_B = static_cast<uint8_t>(iVal);
            if (file >> iVal) CURRENT_EFFECT = iVal;
        }
    }

    void MainWindow::AppWindow_Closing(winrt::Microsoft::UI::Windowing::AppWindow const& sender, winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args)
    {
        args.Cancel(true);
        SaveSettings();
        ShowWindow(g_hWnd, SW_HIDE);

        memset(&g_nid, 0, sizeof(g_nid));
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = g_hWnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;

        wchar_t path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        std::wstring exeDir = path;
        exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/"));
        std::wstring iconPath = exeDir + L"\\app.ico";
        g_nid.hIcon = (HICON)LoadImageW(NULL, iconPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE);

        wcscpy_s(g_nid.szTip, L"Ambilight Engine");
        Shell_NotifyIconW(NIM_ADD, &g_nid);
    }

    void MainWindow::NavView_SelectionChanged(NavigationView const&, NavigationViewSelectionChangedEventArgs const& args)
    {
        if (!m_isReady || !args.SelectedItem()) return;

        auto item = args.SelectedItem().try_as<NavigationViewItem>();
        if (!item || !item.Tag()) return;

        winrt::hstring tag = winrt::unbox_value<winrt::hstring>(item.Tag());

        PowerPanel().Visibility(Visibility::Collapsed);
        EffectsPanel().Visibility(Visibility::Collapsed);
        MotionPanel().Visibility(Visibility::Collapsed);
        ColorPanel().Visibility(Visibility::Collapsed);
        MappingPanel().Visibility(Visibility::Collapsed);
        HardwarePanel().Visibility(Visibility::Collapsed);
        SensorsPanel().Visibility(Visibility::Collapsed);
        AboutPanel().Visibility(Visibility::Collapsed);

        if (tag == L"Power") {
            HeaderTextBlock().Text(L"Power & Styling");
            PowerPanel().Visibility(Visibility::Visible);
        }
        else if (tag == L"Effects") {
            HeaderTextBlock().Text(L"Modes & Effects");
            EffectsPanel().Visibility(Visibility::Visible);
        }
        else if (tag == L"Motion") {
            HeaderTextBlock().Text(L"Motion & Brightness");
            MotionPanel().Visibility(Visibility::Visible);
        }
        else if (tag == L"Color") {
            HeaderTextBlock().Text(L"Color Calibration");
            ColorPanel().Visibility(Visibility::Visible);
        }
        else if (tag == L"Mapping") {
            HeaderTextBlock().Text(L"LED Mapping Setup");
            MappingPanel().Visibility(Visibility::Visible);
        }
        else if (tag == L"Hardware") {
            HeaderTextBlock().Text(L"Hardware & External LED");
            HardwarePanel().Visibility(Visibility::Visible);
        }
        else if (tag == L"Sensors") {
            HeaderTextBlock().Text(L"Sensor Dashboard");
            SensorsPanel().Visibility(Visibility::Visible);
        }
        else if (tag == L"About") {
            HeaderTextBlock().Text(L"About");
            AboutPanel().Visibility(Visibility::Visible);
        }
    }

    void MainWindow::ModeBox_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
        if (!m_isReady || !ModeBox()) return;
        int mode = ModeBox().SelectedIndex();
        DISPLAY_MODE = mode;
        SolidColorButton().Visibility(mode == 1 ? Visibility::Visible : Visibility::Collapsed);
        EffectBox().Visibility(mode == 2 ? Visibility::Visible : Visibility::Collapsed);
        SaveSettings();
    }

    void MainWindow::StripColor_ColorChanged(ColorPicker const&, ColorChangedEventArgs const& args) {
        if (!m_isReady) return;
        auto c = args.NewColor();
        SOLID_R = c.R;
        SOLID_G = c.G;
        SOLID_B = c.B;
        SaveSettings();
    }

    void MainWindow::EffectBox_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
        if (!m_isReady || !EffectBox()) return;
        CURRENT_EFFECT = EffectBox().SelectedIndex();
        SaveSettings();
    }

    void MainWindow::AuxMode_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (!m_isReady || !AuxModeBox() || !AuxColorPicker()) return;
        AUX_LED_MODE = AuxModeBox().SelectedIndex() == 1 ? 1 : 0;
        AuxColorPicker().IsEnabled(AUX_LED_MODE == 0);
        SaveSettings();
    }

    void MainWindow::AuxColor_ColorChanged(ColorPicker const&, ColorChangedEventArgs const& args)
    {
        if (!m_isReady) return;
        auto c = args.NewColor();
        AUX_STATIC_R = c.R;
        AUX_STATIC_G = c.G;
        AUX_STATIC_B = c.B;
        SaveSettings();
    }

    void MainWindow::MasterPower_Toggled(IInspectable const&, RoutedEventArgs const&) {
        if (!m_isReady) return;
        STRIP_ON = MasterPowerToggle().IsOn();
        StatusText().Text(STRIP_ON.load() ? L"Status: Active" : L"Status: Inactive");
        StatusText().Foreground(STRIP_ON.load() ? SolidColorBrush(Colors::LightGreen()) : SolidColorBrush(Colors::Gray()));
        SaveSettings();
    }

    void MainWindow::StartupToggle_Toggled(IInspectable const&, RoutedEventArgs const&) {
        if (!m_isReady) return;
        SetStartupRegistry(StartupToggle().IsOn());
    }

    void MainWindow::StyleToggle_Toggled(IInspectable const&, RoutedEventArgs const&) {
        if (!m_isReady) return;
        SystemBackdrop(nullptr);
        if (StyleToggle().IsOn()) SystemBackdrop(MicaBackdrop());
        else SystemBackdrop(DesktopAcrylicBackdrop());
    }

    void MainWindow::Slider_ValueChanged(IInspectable const& sender, RangeBaseValueChangedEventArgs const& args) {
        if (!m_isReady) return;
        auto slider = sender.try_as<Slider>();
        if (!slider) return;

        float val = static_cast<float>(args.NewValue());
        if (slider == BrightnessSlider()) BRIGHTNESS = val;
        else if (slider == SmoothingSlider()) SMOOTHING = val;
        else if (slider == RedGainSlider()) RED_GAIN = val;
        else if (slider == GreenGainSlider()) GREEN_GAIN = val;
        else if (slider == BlueGainSlider()) BLUE_GAIN = val;
        SaveSettings();
    }

    void MainWindow::ComPort_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
        if (!m_isReady || !ComPortBox()) return;
        COM_PORT_NUM = ComPortBox().SelectedIndex() + 1;
        RECONNECT_SERIAL = true;
        SaveSettings();
    }

    void MainWindow::LedBox_ValueChanged(NumberBox const& sender, NumberBoxValueChangedEventArgs const& args) {
        if (!m_isReady) return;
        if (std::isnan(args.NewValue())) return;

        int val = static_cast<int>(args.NewValue());
        if (sender == TopLedBox()) LED_TOP = val;
        else if (sender == LeftLedBox()) LED_LEFT = val;
        else if (sender == RightLedBox()) LED_RIGHT = val;
        else if (sender == BottomLedBox()) LED_BOTTOM = val;

        GEOMETRY_CHANGED = true;
        SaveSettings();
    }

    void MainWindow::StartPos_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
        if (!m_isReady || !StartPosBox()) return;
        START_POS = StartPosBox().SelectedIndex();
        GEOMETRY_CHANGED = true;
        SaveSettings();
    }

    void MainWindow::Direction_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
        if (!m_isReady || !DirectionBox()) return;
        DIRECTION = DirectionBox().SelectedIndex();
        GEOMETRY_CHANGED = true;
        SaveSettings();
    }
}