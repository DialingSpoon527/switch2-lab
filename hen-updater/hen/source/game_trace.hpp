// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <switch.h>
#include "../../common/game_arm.h"

enum GameTracePoint : u32 {
    GameTracePoint_WorkerEntered = 1,
    GameTracePoint_MonitorReady = 2,
    GameTracePoint_MemoryReady = 3,
    GameTracePoint_CycleEntered = 4,
    GameTracePoint_PackageOpened = 5,
    GameTracePoint_GateParked = 6,
    GameTracePoint_ReceiveLoop = 7,
    GameTracePoint_WaitBridgeExit = 8,
    GameTracePoint_BridgeExited = 9,
    GameTracePoint_RequestReady = 10,
    GameTracePoint_FirstSnapshotDone = 11,
    GameTracePoint_BeforeStep = 12,
    GameTracePoint_SecondSnapshotDone = 13,
    GameTracePoint_RequestInspected = 14,
    GameTracePoint_AfterStep = 15,
    GameTracePoint_InputSample = 16,
    GameTracePoint_GateRecovery = 45,
    GameTracePoint_GateRestored = 46,
    GameTracePoint_SavedPolicyReady = 47,
    GameTracePoint_FirstLaunchCapture = 48,
    GameTracePoint_PolicyCaptureAttempt = 49,
    GameTracePoint_PolicyCaptured = 50,
    GameTracePoint_PolicyCaptureFailed = 51,
    GameTracePoint_SecondMountWatchdog = 52,
    GameTracePoint_FirstMountMatched = 53,
    GameTracePoint_CandidateTitle = 54,
    GameTracePoint_ExpectedTitle = 55,
    GameTracePoint_StorageMismatch = 56,
    GameTracePoint_RTriggered = 57,
    GameTracePoint_FirstMountClosed = 58,
    GameTracePoint_Fault = 0xff,
};

struct GameTraceSnapshot {
    u32 sequence;
    u32 point;
    Result result;
    u32 value;
    u32 requests;
    u32 steps;
};
static_assert(sizeof(GameTraceSnapshot) == 24);

struct HenDebugSnapshot {
    HenGameStatus game;
    GameTraceSnapshot trace;
};
static_assert(sizeof(HenDebugSnapshot) == 48);

// The resident loop writes and reads these fields without an FS request.
class GameTrace {
  public:
    static constexpr u32 HistoryCapacity = 128;

    void write(u32 point, Result result = 0, u32 value = 0) {
        snapshot_.point = point;
        snapshot_.result = result;
        snapshot_.value = value;

        if (point == GameTracePoint_RequestReady) {
            snapshot_.requests++;
        } else if (point == GameTracePoint_BeforeStep) {
            snapshot_.steps++;
        }

        snapshot_.sequence++;

        if (important(point, value)) {
            history_[history_head_ % HistoryCapacity] = snapshot_;
            ++history_head_;

            if (history_head_ - history_tail_ > HistoryCapacity) {
                history_tail_ = history_head_ - HistoryCapacity;
            }
        }
    }

    // Called by the existing supervisor snapshot command.
    // Return the oldest undelivered milestone first.
    GameTraceSnapshot snapshot() const {
        if (history_tail_ != history_head_) {
            return history_[history_tail_++ % HistoryCapacity];
        }
        return snapshot_;
    }

    // Fault reporting needs the actual latest checkpoint.
    GameTraceSnapshot latest() const {
        return snapshot_;
    }

  private:
    static bool important(u32 point, u32 value) {
        switch (point) {
        case GameTracePoint_InputSample:
            // Keep actual R presses, not ordinary released samples.
            return value != 0 && value != 0xffffffffu;

        case GameTracePoint_GateParked:
        case GameTracePoint_ReceiveLoop:
        case GameTracePoint_RequestReady:
        case GameTracePoint_BeforeStep:
        case GameTracePoint_AfterStep:
        case GameTracePoint_RequestInspected:
        case GameTracePoint_FirstMountMatched:
        case GameTracePoint_SecondMountWatchdog:
        case GameTracePoint_GateRecovery:
        case GameTracePoint_GateRestored:
        case GameTracePoint_PackageOpened:
        case GameTracePoint_FirstLaunchCapture:
        case GameTracePoint_PolicyCaptured:
        case GameTracePoint_PolicyCaptureFailed:
        case GameTracePoint_CandidateTitle:
        case GameTracePoint_ExpectedTitle:
        case GameTracePoint_StorageMismatch:
        case GameTracePoint_RTriggered:
        case GameTracePoint_FirstMountClosed:
        case GameTracePoint_Fault:
            return true;

        default:
            return false;
        }
    }

    GameTraceSnapshot snapshot_ = {};
    GameTraceSnapshot history_[HistoryCapacity] = {};
    u32 history_head_ = 0;
    mutable u32 history_tail_ = 0;
};
