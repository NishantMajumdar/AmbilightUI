#pragma once
#include <atomic>
#include <cstdint>

// --- GEOMETRY & MAPPING ---
inline std::atomic<int> LED_LEFT{ 10 };
inline std::atomic<int> LED_TOP{ 20 };
inline std::atomic<int> LED_RIGHT{ 10 };
inline std::atomic<int> LED_BOTTOM{ 20 };

inline std::atomic<int> START_POS{ 3 };
inline std::atomic<int> DIRECTION{ 0 };

// --- POWER & COLOR CALIBRATION ---
inline std::atomic<bool> STRIP_ON{ true };
inline std::atomic<float> BRIGHTNESS{ 1.0f };
inline std::atomic<float> SMOOTHING{ 0.5f };

inline std::atomic<float> RED_GAIN{ 1.0f };
inline std::atomic<float> GREEN_GAIN{ 1.0f };
inline std::atomic<float> BLUE_GAIN{ 1.0f };

// --- HARDWARE CONFIG ---
inline std::atomic<int> COM_PORT_NUM{ 3 };
inline std::atomic<int> AUX_LED_MODE{ 0 };
inline std::atomic<uint8_t> AUX_STATIC_R{ 255 }, AUX_STATIC_G{ 255 }, AUX_STATIC_B{ 255 };

// --- STATE TRIGGERS ---
inline std::atomic<bool> RECONNECT_SERIAL{ false };
inline std::atomic<bool> GEOMETRY_CHANGED{ false };

// --- SENSOR DASHBOARD GLOBALS ---
inline std::atomic<int> CURRENT_LDR_VALUE{ 1000 };
inline std::atomic<float> CURRENT_GPU_TEMP{ 0.0f };

// --- MODE & EFFECT CONTROLS ---
inline std::atomic<int> DISPLAY_MODE{ 0 };
inline std::atomic<uint8_t> SOLID_R{ 255 }, SOLID_G{ 0 }, SOLID_B{ 150 };
inline std::atomic<int> CURRENT_EFFECT{ 0 };

void StartAmbilightEngine();