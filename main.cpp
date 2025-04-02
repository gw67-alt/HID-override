#include <windows.h>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <queue>
#include <string.h>
#include <chrono>

// Constants
constexpr USHORT LOOPBACK_VENDOR_ID = 0x0C45;
constexpr USHORT LOOPBACK_PRODUCT_ID = 0x7403;
constexpr size_t MAX_QUEUE_SIZE = 32;  // Limit queue size to prevent memory growth
constexpr int POLLING_INTERVAL_MS = 1;  // Faster polling interval

// Optimized fixed-size HID Reports
enum class HIDReportType : uint8_t {
    KEYBOARD = 0x01,
    MOUSE = 0x02,
    GAMEPAD = 0x03
};

// Fixed-size mouse report to avoid dynamic allocation
struct MouseReport {
    uint8_t buttons;      // Button states
    int16_t x;            // X movement
    int16_t y;            // Y movement
    int8_t wheel;         // Wheel movement
    uint32_t timestamp;

    MouseReport() : buttons(0), x(0), y(0), wheel(0), timestamp(0) {}
};

// Fixed-size keyboard report to avoid dynamic allocation
struct KeyboardReport {
    uint8_t modifiers;    // Ctrl, Alt, Shift, etc.
    uint8_t reserved;     // Reserved byte
    uint8_t keys[6];      // Up to 6 keys pressed simultaneously
    uint32_t timestamp;

    KeyboardReport() : modifiers(0), reserved(0), timestamp(0) {
        memset(keys, 0, sizeof(keys));
    }
};

// Global state
HHOOK g_mouseHook = NULL;
HHOOK g_keyboardHook = NULL;
POINT g_lastCursorPos = {0, 0};
std::atomic<bool> g_running(true);
std::atomic<bool> g_processingEvents(false);
std::atomic<bool> g_blockFeedback(false);
bool g_enableProfiling = false;

// Lock-free queues using fixed arrays
struct MouseQueue {
    MouseReport reports[MAX_QUEUE_SIZE];
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};

    bool push(const MouseReport& report) {
        size_t current_tail = tail.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) % MAX_QUEUE_SIZE;
        if (next_tail == head.load(std::memory_order_acquire))
            return false;  // Queue is full

        reports[current_tail] = report;
        tail.store(next_tail, std::memory_order_release);
        return true;
    }

    bool pop(MouseReport& report) {
        size_t current_head = head.load(std::memory_order_relaxed);
        if (current_head == tail.load(std::memory_order_acquire))
            return false;  // Queue is empty

        report = reports[current_head];
        head.store((current_head + 1) % MAX_QUEUE_SIZE, std::memory_order_release);
        return true;
    }

    bool isEmpty() {
        return head.load(std::memory_order_acquire) ==
               tail.load(std::memory_order_acquire);
    }
} g_mouseQueue;

struct KeyboardQueue {
    KeyboardReport reports[MAX_QUEUE_SIZE];
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};

    bool push(const KeyboardReport& report) {
        size_t current_tail = tail.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) % MAX_QUEUE_SIZE;
        if (next_tail == head.load(std::memory_order_acquire))
            return false;  // Queue is full

        reports[current_tail] = report;
        tail.store(next_tail, std::memory_order_release);
        return true;
    }

    bool pop(KeyboardReport& report) {
        size_t current_head = head.load(std::memory_order_relaxed);
        if (current_head == tail.load(std::memory_order_acquire))
            return false;  // Queue is empty

        report = reports[current_head];
        head.store((current_head + 1) % MAX_QUEUE_SIZE, std::memory_order_release);
        return true;
    }

    bool isEmpty() {
        return head.load(std::memory_order_acquire) ==
               tail.load(std::memory_order_acquire);
    }
} g_keyboardQueue;

// Optimized keyboard state tracking
bool g_keyState[256] = {false};

// Optimized mouse hook procedure using direct queue access
LRESULT CALLBACK OptimizedMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    // Skip processing if in feedback prevention mode or hook code is negative
    if (nCode < 0 || g_processingEvents.load(std::memory_order_acquire) ||
        g_blockFeedback.load(std::memory_order_acquire)) {
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }

    // Process the mouse event
    MSLLHOOKSTRUCT* pMouseStruct = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    MouseReport report;
    report.timestamp = GetTickCount();

    // Fast handling of mouse events
    switch (wParam) {
        case WM_MOUSEMOVE:
            // Get relative movement
            report.x = static_cast<int16_t>(pMouseStruct->pt.x - g_lastCursorPos.x);
            report.y = static_cast<int16_t>(pMouseStruct->pt.y - g_lastCursorPos.y);
            g_lastCursorPos = pMouseStruct->pt;

            // Skip sending if no actual movement (optimization)
            if (report.x == 0 && report.y == 0)
                return CallNextHookEx(NULL, nCode, wParam, lParam);
            break;

        case WM_LBUTTONDOWN:
            report.buttons |= 0x01;
            break;
        case WM_LBUTTONUP:
            report.buttons &= ~0x01;
            break;
        case WM_RBUTTONDOWN:
            report.buttons |= 0x02;
            break;
        case WM_RBUTTONUP:
            report.buttons &= ~0x02;
            break;
        case WM_MBUTTONDOWN:
            report.buttons |= 0x04;
            break;
        case WM_MBUTTONUP:
            report.buttons &= ~0x04;
            break;
        case WM_MOUSEWHEEL:
            report.wheel = GET_WHEEL_DELTA_WPARAM(pMouseStruct->mouseData) > 0 ? 1 : -1;
            break;
        default:
            // Skip other mouse events for efficiency
            return CallNextHookEx(NULL, nCode, wParam, lParam);
    }

    // Add report to lock-free queue
    g_mouseQueue.push(report);

    // Let the event continue through the system
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Optimized keyboard hook procedure
LRESULT CALLBACK OptimizedKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    // Skip processing if in feedback prevention mode or hook code is negative
    if (nCode < 0 || g_processingEvents.load(std::memory_order_acquire) ||
        g_blockFeedback.load(std::memory_order_acquire)) {
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }

    KBDLLHOOKSTRUCT* pKeyboardStruct = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    DWORD vkCode = pKeyboardStruct->vkCode;

    // Program control keys
    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
        // F12 toggles blocking
        if (vkCode == VK_F12) {
            g_blockFeedback = !g_blockFeedback;
            std::cout << "Input blocking: " << (g_blockFeedback ? "ON" : "OFF") << std::endl;
            return 1; // Block this key
        }

        // F11 toggles profiling
        if (vkCode == VK_F11) {
            g_enableProfiling = !g_enableProfiling;
            std::cout << "Profiling: " << (g_enableProfiling ? "ON" : "OFF") << std::endl;
            return 1; // Block this key
        }

        // Escape exits the program
        if (vkCode == VK_ESCAPE) {
            g_running = false;
            std::cout << "Exiting..." << std::endl;
            return 1; // Block this key
        }
    }

    // Create and queue the keyboard report
    bool keyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

    // Skip if state hasn't changed (avoid unnecessary processing)
    if (g_keyState[vkCode] == keyDown)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    // Update key state
    g_keyState[vkCode] = keyDown;

    // Create complete keyboard report
    KeyboardReport report;
    report.timestamp = GetTickCount();

    // Set modifiers
    if (g_keyState[VK_LCONTROL] || g_keyState[VK_RCONTROL]) report.modifiers |= 0x01;
    if (g_keyState[VK_LSHIFT] || g_keyState[VK_RSHIFT]) report.modifiers |= 0x02;
    if (g_keyState[VK_LMENU] || g_keyState[VK_RMENU]) report.modifiers |= 0x04;
    if (g_keyState[VK_LWIN] || g_keyState[VK_RWIN]) report.modifiers |= 0x08;

    // Fill the active keys (simple version)
    int keyIndex = 0;
    for (int i = 0; i < 256 && keyIndex < 6; i++) {
        if (g_keyState[i] && i != VK_LCONTROL && i != VK_RCONTROL &&
            i != VK_LSHIFT && i != VK_RSHIFT && i != VK_LMENU &&
            i != VK_RMENU && i != VK_LWIN && i != VK_RWIN) {
            report.keys[keyIndex++] = static_cast<uint8_t>(i);
        }
    }

    // Add report to the queue
    g_keyboardQueue.push(report);

    // Let the event continue through the system
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// High-performance processing thread
void ProcessInputEvents() {
    // Set thread priority to time-critical for minimal latency
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // Performance monitoring
    auto lastProfileTime = std::chrono::high_resolution_clock::now();
    int frameCount = 0;
    int eventCount = 0;

    // Input buffer for SendInput
    INPUT inputBuffer[16];
    int inputCount = 0;

    // Last known mouse state to avoid redundant events
    MouseReport lastMouseState;

    while (g_running) {
        // Set processing flag to avoid feedback loops
        g_processingEvents.store(true, std::memory_order_release);

        // Clear input buffer
        inputCount = 0;
        bool didProcess = false;

        // Process all available mouse events
        MouseReport mouseReport;
        while (g_mouseQueue.pop(mouseReport)) {
            // Check if it's a movement event
            if (mouseReport.x != 0 || mouseReport.y != 0) {
                inputBuffer[inputCount].type = INPUT_MOUSE;
                inputBuffer[inputCount].mi.dx = mouseReport.x;
                inputBuffer[inputCount].mi.dy = mouseReport.y;
                inputBuffer[inputCount].mi.dwFlags = MOUSEEVENTF_MOVE;
                inputBuffer[inputCount].mi.time = 0;
                inputBuffer[inputCount].mi.dwExtraInfo = 0;
                inputCount++;
            }

            // Handle button changes efficiently
            uint8_t changedButtons = mouseReport.buttons ^ lastMouseState.buttons;

            // Left button
            if (changedButtons & 0x01) {
                inputBuffer[inputCount].type = INPUT_MOUSE;
                inputBuffer[inputCount].mi.dx = 0;
                inputBuffer[inputCount].mi.dy = 0;
                inputBuffer[inputCount].mi.dwFlags = (mouseReport.buttons & 0x01) ?
                                                     MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                inputBuffer[inputCount].mi.time = 0;
                inputBuffer[inputCount].mi.dwExtraInfo = 0;
                inputCount++;
            }

            // Right button
            if (changedButtons & 0x02) {
                inputBuffer[inputCount].type = INPUT_MOUSE;
                inputBuffer[inputCount].mi.dx = 0;
                inputBuffer[inputCount].mi.dy = 0;
                inputBuffer[inputCount].mi.dwFlags = (mouseReport.buttons & 0x02) ?
                                                     MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
                inputBuffer[inputCount].mi.time = 0;
                inputBuffer[inputCount].mi.dwExtraInfo = 0;
                inputCount++;
            }

            // Middle button
            if (changedButtons & 0x04) {
                inputBuffer[inputCount].type = INPUT_MOUSE;
                inputBuffer[inputCount].mi.dx = 0;
                inputBuffer[inputCount].mi.dy = 0;
                inputBuffer[inputCount].mi.dwFlags = (mouseReport.buttons & 0x04) ?
                                                     MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
                inputBuffer[inputCount].mi.time = 0;
                inputBuffer[inputCount].mi.dwExtraInfo = 0;
                inputCount++;
            }

            // Mouse wheel
            if (mouseReport.wheel != 0) {
                inputBuffer[inputCount].type = INPUT_MOUSE;
                inputBuffer[inputCount].mi.dx = 0;
                inputBuffer[inputCount].mi.dy = 0;
                inputBuffer[inputCount].mi.mouseData = mouseReport.wheel * WHEEL_DELTA;
                inputBuffer[inputCount].mi.dwFlags = MOUSEEVENTF_WHEEL;
                inputBuffer[inputCount].mi.time = 0;
                inputBuffer[inputCount].mi.dwExtraInfo = 0;
                inputCount++;
            }

            // Update last state
            lastMouseState = mouseReport;
            didProcess = true;
            eventCount++;

            // Send input if buffer is getting full
            if (inputCount >= 10) {
                SendInput(inputCount, inputBuffer, sizeof(INPUT));
                inputCount = 0;
            }
        }

        // Process all available keyboard events
        KeyboardReport kbReport;
        while (g_keyboardQueue.pop(kbReport)) {
            // Process each key separately for precision
            for (int i = 0; i < 6; i++) {
                if (kbReport.keys[i] == 0) continue;

                inputBuffer[inputCount].type = INPUT_KEYBOARD;
                inputBuffer[inputCount].ki.wVk = kbReport.keys[i];
                inputBuffer[inputCount].ki.wScan = 0;
                inputBuffer[inputCount].ki.dwFlags = 0;
                inputBuffer[inputCount].ki.time = 0;
                inputBuffer[inputCount].ki.dwExtraInfo = 0;
                inputCount++;

                if (inputCount >= 10) {
                    SendInput(inputCount, inputBuffer, sizeof(INPUT));
                    inputCount = 0;
                }
            }

            didProcess = true;
            eventCount++;
        }

        // Send any remaining inputs
        if (inputCount > 0) {
            SendInput(inputCount, inputBuffer, sizeof(INPUT));
        }

        // Clear processing flag
        g_processingEvents.store(false, std::memory_order_release);

        // Performance monitoring
        if (g_enableProfiling) {
            frameCount++;
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProfileTime).count();

            if (elapsed >= 1000) {
                double fps = frameCount * 1000.0 / elapsed;
                double eventsPerSec = eventCount * 1000.0 / elapsed;
                std::cout << "Performance: " << fps << " fps, "
                          << eventsPerSec << " events/sec" << std::endl;

                frameCount = 0;
                eventCount = 0;
                lastProfileTime = now;
            }
        }

        // Short sleep if no events were processed to reduce CPU usage
        if (!didProcess) {
            Sleep(POLLING_INTERVAL_MS);
        }
    }
}

// Install hooks with error handling
bool InstallHooks() {
    // Get initial cursor position
    GetCursorPos(&g_lastCursorPos);

    // Install mouse hook
    g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, OptimizedMouseProc, GetModuleHandle(NULL), 0);
    if (!g_mouseHook) {
        std::cerr << "Failed to install mouse hook. Error: " << GetLastError() << std::endl;
        return false;
    }

    // Install keyboard hook
    g_keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, OptimizedKeyboardProc, GetModuleHandle(NULL), 0);
    if (!g_keyboardHook) {
        std::cerr << "Failed to install keyboard hook. Error: " << GetLastError() << std::endl;
        UnhookWindowsHookEx(g_mouseHook);
        return false;
    }

    return true;
}

// Clean up hooks
void CleanupHooks() {
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = NULL;
    }

    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = NULL;
    }
}

// Display help
void DisplayHelp() {
    std::cout << "\n=== Optimized HID Loopback Controls ===\n";
    std::cout << "F12: Toggle input blocking (currently " << (g_blockFeedback ? "ON" : "OFF") << ")\n";
    std::cout << "F11: Toggle performance monitor (currently " << (g_enableProfiling ? "ON" : "OFF") << ")\n";
    std::cout << "ESC: Exit program\n";
    std::cout << "======================================\n\n";
}

int main() {
    std::cout << "=== High-Performance HID Loopback ===\n";
    std::cout << "This program offers optimized input redirection\n";

    // Install hooks
    if (!InstallHooks()) {
        std::cerr << "Failed to initialize. Exiting." << std::endl;
        return 1;
    }

    DisplayHelp();

    // Start input processing thread
    std::thread processThread(ProcessInputEvents);

    // Message loop
    MSG msg;
    while (g_running && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup
    CleanupHooks();

    // Wait for processing thread to finish
    g_running = false;
    if (processThread.joinable()) {
        processThread.join();
    }

    std::cout << "HID loopback terminated." << std::endl;
    return 0;
}
