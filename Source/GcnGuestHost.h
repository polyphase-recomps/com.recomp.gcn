/**
 * @file GcnGuestHost.h
 * @brief Runs a GameCube game translated by wasm2c (Source/Guest/<name>) inside the engine.
 *
 * Implements the runtime's host interface (Source/Gcn/gcn_platform.h) on engine and C
 * library services. The game runs on a thread of its own (its guest threads are
 * coroutines on it), at the same time as the engine: free running, it paces itself at
 * 60 Hz; GcnPlayer picks up its newest frame each tick. Scripts that read or write game
 * memory first hold the game at the end of its current frame (Hold), and it stays parked
 * until GcnPlayer's next tick releases it, so their accesses never race the game. One
 * game at a time.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Gcn/gcn_platform.h"

struct GcnwModule;

namespace GcnGuestHost
{
enum class State
{
    Stopped,
    Running,
    Exited,  // main() returned
    Crashed, // trap or fatal error (see the log)
};

// discPath: the game's disc image; saveDir: folder for the memory card image.
bool Start(const GcnwModule* module, const std::string& discPath, const std::string& saveDir);
// Stops the game thread and frees the guest.
void Stop();
State GetState();
const GcnwModule* GetModule();

// Main thread: the game runs by itself at 60 Hz (on), or only through StepFrame (off: paused).
void SetFreeRun(bool on);
// Main thread: lets the game run until it shows its next frame (or `timeoutMs` passes).
// Returns true when the game is parked again (frame finished).
bool StepFrame(int timeoutMs);
// Main thread: parks the game at the end of its current frame (waits up to timeoutMs) and
// keeps it there until ReleaseHold; true once parked. The script accessors below call it.
bool Hold(int timeoutMs);
void ReleaseHold();

// Main thread: controller state for the next frames.
void SetPad(int port, const GcnPad& pad);
// Script overlay: these buttons are held on port 0 for the next `frames` frames.
void HoldButtons(uint16_t buttons, int frames);
// Rumble the game asked for, per port.
bool GetRumble(int port);

// Main thread: the newest frame if it changed since lastSerial (RGBA8, width x height), drawn
// at `scale` x the game's resolution (mod settings "Resolution").
bool GetFrame(uint32_t& lastSerial, const uint8_t*& rgba, int& width, int& height, int* scale = nullptr);

// Main thread: queued audio (interleaved stereo 16-bit, host order) and its rate.
uint32_t PeekAudio(const int16_t*& frames, uint32_t maxFrames, uint32_t& rate);
void ConsumeAudio(uint32_t frames);

// Main thread: writes the game's queued log lines to the engine log.
void FlushLog();

// ---- scripts (main thread; each call holds the game parked, see Hold) -------------------
// Game memory, big-endian like the console: address from a symbol name (game globals,
// mods' published variables) or a number. Bytes outside main RAM read as 0.
bool Resolve(const std::string& name, uint32_t& addr, uint32_t& size, int& type, int& count, int& stride);
uint32_t Read(uint32_t addr, int bytes);
void Write(uint32_t addr, uint32_t value, int bytes);
std::string ReadString(uint32_t addr, uint32_t maxBytes);

struct BridgeVar
{
    std::string name, help;
    int type, count;
};
struct BridgeRequestInfo
{
    std::string name, help;
};
struct BridgeEvent
{
    std::string name;
    std::vector<int> args;
};
std::vector<BridgeVar> BridgeVariables();
std::vector<BridgeRequestInfo> BridgeRequests();
// Queues a mod request (runs at the start of the game's next frame); id, 0 if refused.
int BridgeRequest(const std::string& name, const std::vector<int>& args);
// True once request `id` ran, with its result (each result is handed out once).
bool BridgeResult(int id, int& result);
// Events mods emitted since the last call.
std::vector<BridgeEvent> BridgeEvents();
uint32_t FrameCount();
}
