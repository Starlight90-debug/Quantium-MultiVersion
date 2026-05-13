// plugin.cpp — Endweave-LL LeviLamina plugin entry point
//
// Hooks:
//   Hook_RNS        — handleRequestNetworkSettingsPacket (SB)
//   Hook_Login      — handleLoginPacket (SB)
//   Hook_Send       — NetworkHandler::send (CB all packets)
//   Hook_Disconnect — ServerNetworkHandler::onDisconnect
//
// Build: xmake build endweave (Windows x64, MSVC, C++20)

// ── LeviLamina SDK ────────────────────────────────────────────
#include <ll/api/mod/NativeMod.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/memory/Hook.h>
#include <ll/api/Logger.h>

// ── BDS fake-headers (from levilamina xmake package) ─────────
// These correspond to actual BDS 1.26.10 internal classes.
// Exact file paths: ~/.xmake/packages/levilamina/<ver>/include/mc/...
#include <mc/network/ServerNetworkHandler.h>
#include <mc/network/NetworkHandler.h>
#include <mc/network/NetworkIdentifier.h>
#include <mc/network/packet/Packet.h>
#include <mc/network/packet/RequestNetworkSettingsPacket.h>
#include <mc/network/packet/LoginPacket.h>
#include <mc/deps/core/utility/BinaryStream.h>
#include <mc/deps/core/utility/ReadOnlyBinaryStream.h>
// SubClientId is an enum usually in NetworkHandler.h or:
// #include <mc/network/SubClientId.h>

// ── Endweave ──────────────────────────────────────────────────
#include "codec.h"
#include "codec.cpp" // single-translation-unit: codec.cpp included here

#include <memory>
#include <string>
#include <sstream>
#include <vector>
#include <span>
#include <cstdint>

// =============================================================================
// EndweaveMod
// =============================================================================
namespace ew {

class Mod : public ll::mod::NativeMod {
public:
    // BDS 1.26.10 = protocol 944. Change when upgrading BDS.
    static constexpr int SERVER_PROTO = 944;

    explicit Mod(std::string name, ll::mod::Manifest manifest)
        : ll::mod::NativeMod(std::move(name), std::move(manifest)) {}

    static Mod& get() { return *inst_; }

    ConnMgr&  conns()    noexcept { return *conns_; }
    Pipeline& pipeline() noexcept { return *pipeline_; }
    ll::Logger& log()    noexcept { return getSelf().getLogger(); }

protected:
    bool load()    override;
    bool enable()  override;
    bool disable() override;

private:
    static Mod* inst_;
    std::unique_ptr<ConnMgr>   conns_;
    std::unique_ptr<Registry>  reg_;
    std::unique_ptr<Pipeline>  pipeline_;
};

Mod* Mod::inst_ = nullptr;

bool Mod::load() {
    inst_ = this;
    log().info("[EW] Endweave-LL loading. Server protocol: {}", SERVER_PROTO);
    return true;
}

bool Mod::enable() {
    conns_   = std::make_unique<ConnMgr>(SERVER_PROTO);
    reg_     = std::make_unique<Registry>();

    build_registry(*reg_, SERVER_PROTO,
        [this](const std::string& m) { log().warn(m); });

    pipeline_ = std::make_unique<Pipeline>(
        *reg_, *conns_,
        [this](int lv, const std::string& m) {
            switch(lv) {
            case 0:  log().debug(m); break;
            case 1:  log().info(m);  break;
            case 2:  log().warn(m);  break;
            default: log().error(m); break;
            }
        }
    );

    // Report supported versions
    auto sv = reg_->clients(SERVER_PROTO);
    std::ostringstream ss;
    for (int v : sv) ss << v << " ";
    log().info("[EW] Enabled. Supported protocols: {}", ss.str());
    return true;
}

bool Mod::disable() {
    pipeline_.reset(); reg_.reset(); conns_.reset();
    log().info("[EW] Disabled.");
    inst_ = nullptr;
    return true;
}

// =============================================================================
// Utility
// =============================================================================

static std::string addr(const NetworkIdentifier& id) {
    return id.getAddress();
}

static std::vector<uint8_t> serialize(const Packet& pkt) {
    BinaryStream bs;
    const_cast<Packet&>(pkt).write(bs);
    auto s = bs.getAndReleaseData();
    return { reinterpret_cast<const uint8_t*>(s.data()),
             reinterpret_cast<const uint8_t*>(s.data()) + s.size() };
}

static bool deserialize(Packet& pkt, const std::vector<uint8_t>& bytes) {
    std::string s(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    ReadOnlyBinaryStream rbs(s, false);
    return pkt.read(rbs) == StreamReadResult::Pass;
}

// =============================================================================
// HOOK 1 — handleRequestNetworkSettingsPacket
// First packet from any Bedrock client. Reads ClientNetworkVersion (int32_BE).
// Store client_protocol and rewrite to SERVER_PROTO so BDS accepts the connection.
// Python equivalent: base.py detect_client_protocol()
// =============================================================================
LL_AUTO_TYPE_INSTANCE_HOOK(
    Hook_RNS,
    ll::memory::HookPriority::Normal,
    ServerNetworkHandler,
    &ServerNetworkHandler::handleRequestNetworkSettingsPacket,
    void,
    const NetworkIdentifier&             id,
    const RequestNetworkSettingsPacket&  pkt)
{
    auto& mod   = Mod::get();
    auto  a     = addr(id);
    auto& conn  = mod.conns().get_or_create(a);

    int cp = pkt.mClientNetworkVersion;
    conn.client_proto = cp;
    conn.addr         = a;

    mod.log().debug("[EW] RNS {} client_proto={}", a, cp);

    if (cp != Mod::SERVER_PROTO)
        const_cast<RequestNetworkSettingsPacket&>(pkt).mClientNetworkVersion
            = Mod::SERVER_PROTO;

    origin(id, pkt);
}

// =============================================================================
// HOOK 2 — handleLoginPacket
// Login also encodes protocol version. Rewrite to SERVER_PROTO.
// Python equivalent: base.py _rewrite_login() — but here done via field mutation
// (the base translator also does this for the raw-byte path, belt-and-suspenders).
// =============================================================================
LL_AUTO_TYPE_INSTANCE_HOOK(
    Hook_Login,
    ll::memory::HookPriority::Normal,
    ServerNetworkHandler,
    &ServerNetworkHandler::handleLoginPacket,
    void,
    const NetworkIdentifier& id,
    const LoginPacket&       pkt)
{
    if (pkt.mClientNetworkVersion != Mod::SERVER_PROTO) {
        const_cast<LoginPacket&>(pkt).mClientNetworkVersion = Mod::SERVER_PROTO;
        Mod::get().log().debug("[EW] Login rewrite for {}", addr(id));
    }
    origin(id, pkt);
}

// =============================================================================
// HOOK 3 — NetworkHandler::send (ALL clientbound packets)
//
// Strategy:
//   1. Fast-path: pre-handshake (client_proto==0) or same-version → passthrough.
//   2. Serialize packet to raw bytes via BinaryStream.
//   3. Run pipeline.clientbound() — checks each translator in CB chain.
//   4a. PASSTHROUGH → forward original unchanged.
//   4b. CANCELLED   → drop packet (don't call origin).
//   4c. REWRITTEN   → deserialize patched bytes back into pkt, then forward.
//
// Python equivalent: pipeline.py on_packet_send() / PacketSendEvent handler.
//
// IMPORTANT NOTE on NetworkHandler::send signature:
//   In LL 0.13.x: void NetworkHandler::send(NetworkIdentifier const&, Packet const&, SubClientId)
//   SubClientId is typically an enum defined in mc/network/SubClientId.h or as uint8_t.
//   If your LL version differs, check the fake-headers for the exact signature.
//   Alternative hook target if send doesn't work: _sendInternal or _sortAndPacketizeEvents.
// =============================================================================
LL_AUTO_TYPE_INSTANCE_HOOK(
    Hook_Send,
    ll::memory::HookPriority::Normal,
    NetworkHandler,
    &NetworkHandler::send,
    void,
    NetworkIdentifier const& id,
    Packet const&            pkt,
    SubClientId              subId)
{
    auto& mod = Mod::get();
    auto  a   = addr(id);
    auto* c   = mod.conns().get(a);

    // Fast-path: no state or same-version
    if (!c || !c->needs_xlat()) { origin(id, pkt, subId); return; }

    uint32_t pid = (uint32_t)pkt.getId();

    auto raw = serialize(pkt);
    if (raw.empty()) { origin(id, pkt, subId); return; }

    auto res = mod.pipeline().clientbound(a, pid, raw);

    switch (res.action) {
    case PipelineResult::Action::PASSTHROUGH:
        origin(id, pkt, subId);
        break;
    case PipelineResult::Action::CANCELLED:
        mod.log().debug("[EW] CB DROP pkt={} ({})", pid, pkt::name(pid));
        // do not call origin — packet dropped
        break;
    case PipelineResult::Action::REWRITTEN:
        if (!deserialize(const_cast<Packet&>(pkt), res.payload)) {
            mod.log().warn("[EW] CB DESER FAIL pkt={} ({}), sending original",
                           pid, pkt::name(pid));
        }
        origin(id, pkt, subId);
        break;
    }
}

// =============================================================================
// HOOK 4 — onDisconnect — clean up connection state
// Python equivalent: plugin.py on_player_quit()
// =============================================================================
LL_AUTO_TYPE_INSTANCE_HOOK(
    Hook_Disconnect,
    ll::memory::HookPriority::Normal,
    ServerNetworkHandler,
    &ServerNetworkHandler::onDisconnect,
    void,
    NetworkIdentifier const& id,
    std::string const&       reason,
    bool                     hideMsg)
{
    Mod::get().conns().remove(addr(id));
    origin(id, reason, hideMsg);
}

// =============================================================================
// SERVERBOUND HOOKS (non-handshake packets)
//
// The two handshake packets (RNS + Login) are handled above via direct
// field mutation — fastest path, no serialize/deserialize needed.
//
// For other SB packets (LevelSoundEvent, ActorEvent, PlayerAction, etc.),
// BDS deserializes them BEFORE our hook fires. We must hook each individual
// handleXxxPacket() method.
//
// Template (copy-paste to add a new SB hook):
//
// #include <mc/network/packet/LevelSoundEventPacket.h>
//
// LL_AUTO_TYPE_INSTANCE_HOOK(
//     Hook_LevelSoundEventSB,
//     ll::memory::HookPriority::Normal,
//     ServerNetworkHandler,
//     &ServerNetworkHandler::handleLevelSoundEventPacket,
//     void,
//     NetworkIdentifier const& id,
//     LevelSoundEventPacket const& pkt)
// {
//     auto raw = serialize(pkt);
//     auto res = Mod::get().pipeline().serverbound(addr(id), pkt::LEVEL_SOUND_EVENT, raw);
//     if (res.action == PipelineResult::Action::CANCELLED) return;
//     if (res.action == PipelineResult::Action::REWRITTEN)
//         deserialize(const_cast<LevelSoundEventPacket&>(pkt), res.payload);
//     origin(id, pkt);
// }
//
// Priority of SB hooks for production completeness (in order of impact):
//   1. LevelSoundEventPacket    — affects sounds the server plays for events
//   2. ActorEventPacket         — affects entity animations
//   3. PlayerActionPacket       — affects block interactions (v924↔v944)
//   4. InventoryTransactionPacket — affects block interactions (v924↔v944)
//   5. LecternUpdatePacket      — affects lectern usage (v924↔v944)
//   6. BlockActorDataPacket     — affects sign/chest editing (v924↔v944)
//   7. UpdateClientOptionsPacket— strips v975 field (v944↔v975)
//   8. ClientMovementPredictionSync — strips v975 fields (v944↔v975)
//
// For a v944 (1.26.10) server with v975 (1.26.20) clients:
//   Items 7 and 8 are most important since they prevent server parse errors.
//   Items 1-6 affect gameplay correctness but not connectivity.
// =============================================================================

} // namespace ew

// =============================================================================
// LeviLamina plugin registration
// =============================================================================
LL_REGISTER_MOD(ew::Mod, ew::Mod::get());
