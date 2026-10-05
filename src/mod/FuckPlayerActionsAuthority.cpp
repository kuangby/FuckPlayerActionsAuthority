#include "mod/FuckPlayerActionsAuthority.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include "mc/entity/components/DimensionStateComponent.h"
#include "mc/entity/components/PackedItemUseLegacyInventoryTransaction.h"
#include "mc/entity/components/PlayerBlockActionData.h"
#include "mc/entity/components/PlayerBlockActions.h"
#include "mc/entity/components/ServerPlayerMovementComponent.h"
#include "mc/entity/components/player_tick_policy/ThrottledTickPolicy.h"
#include "mc/entity/systems/ServerPlayerInventoryTransactionSystem.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/network/ServerPlayerBlockUseHandler.h"
#include "mc/network/packet/InventoryPacketHandler.h"
#include "mc/network/packet/InventoryTransactionPacket.h"
#include "mc/network/packet/InventoryTransactionPacketPayload.h"
#include "mc/network/packet/LegacySetSlot.h"
#include "mc/network/packet/PlayerAuthInputPacket.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/Minecraft.h"
#include "mc/world/inventory/network/ItemStackRequestData.h"
#include "mc/world/inventory/transaction/ItemUseInventoryTransaction.h"
#include "mc/world/level/Level.h"


#ifdef LL_PLAT_C
#include "ll/api/service/Bedrock.h"
#include "mc/server/ServerInstance.h"
#include <thread>
#endif


namespace fuck_player_actions_authority {

FuckPlayerActionsAuthority& FuckPlayerActionsAuthority ::getInstance() {
    static FuckPlayerActionsAuthority instance;
    return instance;
}

std::unordered_map<std::string, int> allowMap;

LL_TYPE_INSTANCE_HOOK(
    PlayerTeleportHook,
    ll::memory::HookPriority::Normal,
    Player,
    &Player::$teleportTo,
    void,
    ::Vec3 const& pos,
    bool          shouldStopRiding,
    int           cause,
    int           sourceEntityType,
    bool          keepVelocity
) {
    origin(pos, shouldStopRiding, cause, sourceEntityType, keepVelocity);
    allowMap[this->getRealName()] = 5;
}

LL_TYPE_INSTANCE_HOOK(
    PlayerCreditHook,
    ll::memory::HookPriority::Normal,
    PlayerTickPolicy::ThrottledTickPolicy,
    &ThrottledTickPolicy ::$shouldTickPlayer,
    ::IPlayerTickPolicy::TickAction,
    uint64 const /*creditTicks*/,
    uint64 unprocessedTicksSize
) {
    return unprocessedTicksSize == 0 ? TickAction::StopProcessing : TickAction::ProcessTick;
}

LL_TYPE_INSTANCE_HOOK(
    StripOneShotActionsHook,
    ll::memory::HookPriority::Low,
    ServerNetworkHandler,
    &ServerNetworkHandler::$handle,
    void,
    ::NetworkIdentifier const&     source,
    ::PlayerAuthInputPacket const& packet
) {
#ifdef LL_PLAT_C
    if (auto serverInstance = ll::service::getServerInstance();
        !serverInstance || std::this_thread::get_id() != serverInstance->mServerInstanceThread->get_id())
        return origin(source, packet);
#endif
    auto player = thisFor<NetEventCallback>()->_getServerPlayer(source, packet.mSenderSubId);
    if (!player) return origin(source, packet);

    if (auto it = allowMap.find(player->getRealName()); it != allowMap.end()) {
        if (--it->second <= 0) allowMap.erase(it);
        return origin(source, packet);
    }

    auto comp = player->mEntityContext->tryGetComponent<ServerPlayerMovementComponent>();
    if (!comp) [[unlikely]]
        return origin(source, packet);

    // 传送/跨维度确认中（等待 HandledTeleport）：原版此时直接丢包。
    // 镜像原版两道丢包门：跨维度转移中 / 传送确认中 → 原版直接丢包
    auto dimState = player->mEntityContext->tryGetComponent<DimensionStateComponent>();
    if ((dimState && dimState->mDimensionState != DimensionStateComponent::DimensionState::Ready)
        || comp->mServerHasMovementAuthority->any()) {
        comp->mAcceptClientPosIfWithinDistanceSq->reset();
        return origin(source, packet);
    }

    auto& pkt = const_cast<PlayerAuthInputPacket&>(packet);

    // ---- 1. 剥离(必须在 origin 之前,否则出队时会二次执行)----
    std::vector<PlayerBlockActionData> blockActions;
    if (!pkt.mPlayerBlockActions->mActions->empty()) {
        blockActions = std::move(*pkt.mPlayerBlockActions->mActions);
    }
    auto itemStackRequest   = std::move(pkt.mItemStackRequest);
    auto itemUseTransaction = std::move(pkt.mItemUseTransaction);

    const bool hasOneShot = !blockActions.empty() || itemStackRequest || itemUseTransaction;
    if (hasOneShot) {
        pkt.mInputData->mContainer.set((size_t)::PlayerAuthInputPacketPayload::InputData::PerformBlockActions, false);
        pkt.mInputData->mContainer.set(
            (size_t)::PlayerAuthInputPacketPayload::InputData::PerformItemStackRequest,
            false
        );
    }

    // ---- 2. 移动数据照常入队 ----
    origin(source, packet);

    // ---- 3. 移动加速:engage 接受距离 + 折叠积压(对纯移动包也要做,不再有 early-return)----

    if (auto vehicle = player->getVehicle(); vehicle && vehicle->isPassenger(*player))
        comp->mAcceptClientPosIfWithinDistanceSq->reset();
    else comp->mAcceptClientPosIfWithinDistanceSq->emplace(25600);
    auto& q = *comp->mQueuedUpdates;
    while (q.size() > 1) {
        auto& front = q.front();
        if (!front.mTransactions->empty() || front.mInteraction->has_value()) break;
        q.pop_front();
    }

    // freeze 补偿:把 credits 抬到 >= 队列长,让 catch-up adder 放行
    if (auto mc = ll::service::getMinecraft(); mc && mc->getSimPaused()) {
        auto credits = static_cast<uint64>(q.size());
        if (comp->mPlayerTickCredits < credits) comp->mPlayerTickCredits = credits;
    }


    // ---- 4. 立即执行剥离出的一次性内容(原 3a/3b,不变)----
    if (!blockActions.empty() || itemStackRequest) {
        PlayerBlockActions actions{};
        *actions.mActions = std::move(blockActions);
        ServerPlayerBlockUseHandler::onBeforeMovementSimulation(
            *player,
            actions,
            std::move(itemStackRequest),
            *mTextFilteringProcessor
        );
    }
    if (itemUseTransaction) {
        ::InventoryTransactionPacket txPacket{
            ::InventoryTransactionPacketPayload{
                                                std::make_unique<::ItemUseInventoryTransaction>(*itemUseTransaction->mTransaction),
                                                false
            }
        };
        *txPacket.mLegacyRequestId    = *itemUseTransaction->mID;
        *txPacket.mLegacySetItemSlots = *itemUseTransaction->mSlots;
        ::ServerPlayerInventoryTransactionSystem::transactInventoryPacket(
            txPacket,
            *player,
            player->getLevel().getBlockPalette()
        );
    }
}

bool FuckPlayerActionsAuthority ::load() {
    getSelf().getLogger().debug("Loading...");
    // Code for loading the mod goes here.
    return true;
}

bool FuckPlayerActionsAuthority ::enable() {
    getSelf().getLogger().debug("Enabling...");
    PlayerTeleportHook::hook();
    PlayerCreditHook::hook();
    StripOneShotActionsHook::hook();
    // Code for enabling the mod goes here.
    return true;
}

bool FuckPlayerActionsAuthority ::disable() {
    getSelf().getLogger().debug("Disabling...");
    // Code for disabling the mod goes here.
    return true;
}

} // namespace fuck_player_actions_authority

LL_REGISTER_MOD(
    fuck_player_actions_authority ::FuckPlayerActionsAuthority,
    fuck_player_actions_authority ::FuckPlayerActionsAuthority ::getInstance()
);
