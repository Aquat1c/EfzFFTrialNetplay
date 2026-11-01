#pragma once

#include "rollback_session.h"
#include "rollback_types.h"
#include "trial_protocol.h"

#include <windows.h>

namespace thv::rollback
{

struct HostCallbacks
{
    int(__cdecl* advanceBattleTick)(HWND hWnd) = nullptr;
    int(__cdecl* advanceLoadingTick)(HWND hWnd) = nullptr;
    int(__cdecl* advanceCharSelectTick)(HWND hWnd) = nullptr;
    int(__cdecl* advanceMenuTick)(HWND hWnd) = nullptr;
    int(__cdecl* advanceTitleTick)(HWND hWnd) = nullptr;
    HWND gameWindow = nullptr;
};

class Engine
{
public:
    void Reset();
    void SetHostCallbacks(const HostCallbacks& callbacks);

    void MainTick();
    void OnBattleFrameBoundary(HWND gameWindow);
    void SetGameWindow(HWND gameWindow);

    void SetInputDelay(int frames);
    void SetPingMs(std::uint32_t pingMs);
    void SetPredictionWindow(int frames);
    void SetLocalSide(int side);
    void SetRngSeed(std::uint32_t seed);
    void PushLocalInput(std::uint8_t wireMask);
    void PushRemoteInput(std::uint8_t wireMask, std::uint8_t systemState = 0);
    void ImportWireHistories(
        const std::vector<std::uint8_t>& local,
        const std::vector<std::uint8_t>& remote,
        std::uint32_t simFrame);
    void BeginMatchOnScreenEntry();
    void FillSyncDataPacket(proto::SyncDataPacket* out) const;
    bool ApplySyncDataPacket(const proto::SyncDataPacket& packet);

    const Session& GetSession() const;
    Status BuildStatus() const;

    bool IsInputAuthorityActive() const;
    bool TryGetWireMaskForPlayerSide(int playerSide, std::uint8_t* outMask) const;
    std::uint8_t GetMergedSystemState() const;
    void AcknowledgeSystemStateApplied(std::uint8_t state);
    bool IsDrivingCatchUp() const;
    int GetLastMainTickIterations() const;

private:
    int WaitRemote();
    int AdjustPrediction();
    int AdvanceSimple(int frameBudget);
    int AdvanceBatch(int frameBudget);
    int AdvanceUntilInput(int frameBudget);
    void BatchRender(int frameCount);
    bool SaveInputs();
    bool SeedBattleEntrySnapshot();
    bool ApplyPendingPostMatchEsc();
    bool ApplyPendingGameplayEsc();
    void ReseedMatchRng();
    bool AdvanceOne();
    void HandleMemorialTransition(int newMemorial, int oldMemorial);
    void ServiceDesyncDump();
    // False means the ring invariant broke; the caller must not continue the match.
    bool ReloadSavedState(std::uint32_t restoreFrame);
    void ApplyReloadedState(const RollbackFrameSlot& slot);
    void MergeDelayMaps(std::uint32_t restoreFrame, std::uint32_t targetFrame);
    bool ServicePendingRollback();
    void ServiceConfirmedFrames();
    void ServiceConfirmedRecorderFrames();
    bool ApplyConfirmedPracticeSystemState();
    // Local-only shortcuts: they never enter systemState or spectator records.
    void ServiceLiveDelayHotkeys(int memorialScreen);
    void ResetBattleRollbackState(std::uint32_t startFrame);
    void EnsureInputsCapturedForFrame(bool requireRemoteInput);
    bool CaptureAndAppendLocalInput();
    // Must not sample a second physical/system input in the same presentation slot.
    bool AppendRepeatedLocalInput();
    bool AppendLocalInputWithProvenance(std::uint8_t wireMask, std::uint8_t systemState);
    bool StampCurrentFrameProvenance();
    bool RequireRemoteInput() const;
    // Battle plus loading substates 5/7/10 (round post) and 2 (next-round intro).
    bool IsMatchRollbackActive() const;
    bool IsPredictionScreen() const;
    void EmitPerformanceSample(std::uint64_t mainTickUs, int iterations);

    struct PerformanceWindow
    {
        std::uint32_t startMs = 0;
        std::uint64_t mainCalls = 0;
        std::uint64_t mainTotalUs = 0;
        std::uint64_t mainMaxUs = 0;
        std::uint64_t zeroWorkCalls = 0;
        std::uint64_t saveCalls = 0;
        std::uint64_t saveTotalUs = 0;
        std::uint64_t saveMaxUs = 0;
        std::uint64_t hashCalls = 0;
        std::uint64_t hashTotalUs = 0;
        std::uint64_t hashMaxUs = 0;
        std::uint64_t nativeCalls = 0;
        std::uint64_t nativeTotalUs = 0;
        std::uint64_t nativeMaxUs = 0;
        std::uint64_t hiddenNativeCalls = 0;
        std::uint64_t hiddenNativeUs = 0;
        std::uint64_t visibleNativeCalls = 0;
        std::uint64_t visibleNativeUs = 0;
        std::uint64_t slowHiddenCalls = 0;
        std::uint64_t predictedFrames = 0;
        std::uint64_t confirmedFrames = 0;
        std::uint64_t transitionVetoes = 0;
        int maxBatch = 0;
    };

    Session session_{};
    HostCallbacks host_{};
    HWND lastGameWindow_ = nullptr;
    bool drivingCatchUp_ = false;
    TransportPhase phase_ = TransportPhase::Idle;
    std::uint32_t lastBatchFrames_ = 0;
    std::uint32_t lastReloadFrame_ = 0;
    std::uint32_t rollbacksTotal_ = 0;
    std::uint32_t mispredictsTotal_ = 0;
    static constexpr std::uint32_t kNoFrame = 0xFFFFFFFFu;
    std::uint32_t pendingRollbackFrame_ = kNoFrame;
    std::uint32_t rollbackReplayTarget_ = 0;
    std::uint32_t transitionBlockedFrame_ = kNoFrame;
    std::uint32_t nextHashFrame_ = 0;
    std::uint32_t nextSpectatorFrame_ = 0;
    int lastMainTickIterations_ = 0;
    bool gameplayEscTransitionActive_ = false;
    bool localEscDown_ = false;
    bool remoteEscDown_ = false;
    bool pendingLocalEsc_ = false;
    bool pendingRemoteEsc_ = false;
    std::uint32_t pendingLocalEscFrame_ = kNoFrame;
    std::uint32_t pendingRemoteEscFrame_ = kNoFrame;
    bool increaseDelayKeyDown_ = false;
    bool decreaseDelayKeyDown_ = false;
    PerformanceWindow perf_{};
};

}
