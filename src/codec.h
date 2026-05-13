#pragma once
// =============================================================================
// codec.h  —  Endweave-LL shared interfaces
//
// Everything is self-contained in this one header.
// Sections:
//   1. PacketReader / PacketWriter
//   2. PacketWrapper
//   3. UserConnection + ConnectionManager
//   4. Protocol constants & mappings (exact values from Python source)
//   5. PacketTranslator + TranslatorRegistry + Pipeline
// =============================================================================

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <span>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <any>
#include <typeindex>
#include <optional>
#include <deque>
#include <mutex>
#include <stdexcept>

namespace ew {

// =============================================================================
// 1. PacketReader
// =============================================================================

class Reader {
public:
    Reader(const uint8_t* d, size_t n) noexcept : p(d), end(d+n), cur(d) {}
    explicit Reader(std::span<const uint8_t> s) noexcept : Reader(s.data(), s.size()) {}

    bool     ok()        const noexcept { return cur <= end; }
    size_t   remaining() const noexcept { return (size_t)(end - cur); }
    bool     has_more()  const noexcept { return cur < end; }
    const uint8_t* ptr() const noexcept { return cur; }

    void skip(size_t n)  { need(n); cur += n; }
    void consume_all()   { cur = end; }
    std::span<const uint8_t> rest() const noexcept { return {cur, (size_t)(end-cur)}; }

    uint8_t  byte_()     { need(1); return *cur++; }
    bool     bool_()     { return byte_() != 0; }
    int16_t  i16le()     { return le<int16_t>(); }
    uint16_t u16le()     { return le<uint16_t>(); }
    int32_t  i32le()     { return le<int32_t>(); }
    uint32_t u32le()     { return le<uint32_t>(); }
    int64_t  i64le()     { return le<int64_t>(); }
    float    f32le()     { return le<float>(); }

    int32_t  i32be()     {
        need(4);
        uint32_t v = ((uint32_t)cur[0]<<24)|((uint32_t)cur[1]<<16)|
                     ((uint32_t)cur[2]<<8)|(uint32_t)cur[3];
        cur += 4;
        return (int32_t)v;
    }

    uint32_t uvarint() {
        uint32_t r=0; int s=0;
        while(true){ uint8_t b=byte_(); r|=(uint32_t)(b&0x7F)<<s;
                     if(!(b&0x80)) break; s+=7; if(s>=35) throw std::runtime_error("uvarint OOB"); }
        return r;
    }
    int32_t  varint()   { uint32_t u=uvarint(); return (int32_t)((u>>1)^-(u&1)); }
    uint64_t uvarint64() {
        uint64_t r=0; int s=0;
        while(true){ uint8_t b=byte_(); r|=(uint64_t)(b&0x7F)<<s;
                     if(!(b&0x80)) break; s+=7; if(s>=70) throw std::runtime_error("uvarint64 OOB"); }
        return r;
    }
    int64_t  varint64() { uint64_t u=uvarint64(); return (int64_t)((u>>1)^-(u&1)); }

    std::string str() {
        uint32_t n=uvarint(); if(n>131072) throw std::runtime_error("string OOB");
        need(n); std::string s((const char*)cur,n); cur+=n; return s;
    }

    void bytes(uint8_t* out, size_t n) { need(n); memcpy(out,cur,n); cur+=n; }
    std::vector<uint8_t> bytes(size_t n) {
        need(n); std::vector<uint8_t> v(cur,cur+n); cur+=n; return v;
    }

private:
    const uint8_t* p; const uint8_t* end; const uint8_t* cur;
    void need(size_t n) { if((size_t)(end-cur)<n) throw std::runtime_error("Reader underflow"); }
    template<typename T> T le() {
        need(sizeof(T)); T v; memcpy(&v,cur,sizeof(T)); cur+=sizeof(T); return v;
    }
};

// =============================================================================
// 2. PacketWriter
// =============================================================================

class Writer {
public:
    Writer() { buf.reserve(256); }
    const std::vector<uint8_t>& data() const noexcept { return buf; }
    std::vector<uint8_t>        take()       noexcept { return std::move(buf); }

    void byte_(uint8_t v)  { buf.push_back(v); }
    void bool_(bool v)     { buf.push_back(v?1:0); }
    void i16le(int16_t v)  { le(v); }
    void u16le(uint16_t v) { le(v); }
    void i32le(int32_t v)  { le(v); }
    void u32le(uint32_t v) { le(v); }
    void i64le(int64_t v)  { le(v); }
    void f32le(float v)    { le(v); }
    void i32be(int32_t v)  {
        uint32_t u=(uint32_t)v;
        buf.push_back((u>>24)&0xFF); buf.push_back((u>>16)&0xFF);
        buf.push_back((u>>8)&0xFF);  buf.push_back(u&0xFF);
    }

    void uvarint(uint32_t v) {
        while(v>=0x80){ buf.push_back((uint8_t)((v&0x7F)|0x80)); v>>=7; }
        buf.push_back((uint8_t)v);
    }
    void varint(int32_t v)   { uvarint((uint32_t)((v<<1)^(v>>31))); }
    void uvarint64(uint64_t v) {
        while(v>=0x80){ buf.push_back((uint8_t)((v&0x7F)|0x80)); v>>=7; }
        buf.push_back((uint8_t)v);
    }
    void varint64(int64_t v) { uvarint64((uint64_t)((v<<1)^(v>>63))); }

    void str(const std::string& s) {
        uvarint((uint32_t)s.size());
        buf.insert(buf.end(),(const uint8_t*)s.data(),(const uint8_t*)s.data()+s.size());
    }
    void bytes(const uint8_t* d, size_t n) { buf.insert(buf.end(),d,d+n); }
    void bytes(std::span<const uint8_t> s)  { buf.insert(buf.end(),s.begin(),s.end()); }
    void bytes(const std::vector<uint8_t>& v){ buf.insert(buf.end(),v.begin(),v.end()); }

private:
    std::vector<uint8_t> buf;
    template<typename T> void le(T v){ uint8_t tmp[sizeof(T)]; memcpy(tmp,&v,sizeof(T)); bytes(tmp,sizeof(T)); }
};

// =============================================================================
// 3. PacketWrapper
//    Exact C++ mirror of Python PacketWrapper.
//    pt_*  = passthrough (read from input, write to output)
//    r_*   = read only
//    w_*   = write only
// =============================================================================

struct UserConn; // forward

class Wrapper {
public:
    explicit Wrapper(std::span<const uint8_t> payload, UserConn* u=nullptr)
        : r(payload.data(),payload.size()), user(u), cancelled_(false) {}
    explicit Wrapper(const std::vector<uint8_t>& v, UserConn* u=nullptr)
        : r(v.data(),v.size()), user(u), cancelled_(false) {}

    Reader&  reader() noexcept { return r; }
    Writer&  writer() noexcept { return w; }
    UserConn* user_conn() noexcept { return user; }

    void cancel()    noexcept { cancelled_ = true; }
    bool cancelled() noexcept { return cancelled_; }
    bool has_more()  noexcept { return r.has_more(); }

    // Flush: output written so far + any unread input bytes
    std::vector<uint8_t> finish() {
        if(r.has_more()) w.bytes(r.rest());
        return w.take();
    }

    void pt_all() { if(r.has_more()){ w.bytes(r.rest()); r.consume_all(); } }

    // Typed passthroughs — mirror Python wrapper.passthrough(TYPE)
    uint8_t  pt_byte()    { auto v=r.byte_();    w.byte_(v);    return v; }
    bool     pt_bool()    { auto v=r.bool_();    w.bool_(v);    return v; }
    int16_t  pt_i16le()   { auto v=r.i16le();   w.i16le(v);   return v; }
    uint16_t pt_u16le()   { auto v=r.u16le();   w.u16le(v);   return v; }
    int32_t  pt_i32le()   { auto v=r.i32le();   w.i32le(v);   return v; }
    uint32_t pt_u32le()   { auto v=r.u32le();   w.u32le(v);   return v; }
    int64_t  pt_i64le()   { auto v=r.i64le();   w.i64le(v);   return v; }
    float    pt_f32le()   { auto v=r.f32le();   w.f32le(v);   return v; }
    int32_t  pt_i32be()   { auto v=r.i32be();   w.i32be(v);   return v; }
    uint32_t pt_uvarint() { auto v=r.uvarint();  w.uvarint(v);  return v; }
    int32_t  pt_varint()  { auto v=r.varint();   w.varint(v);   return v; }
    uint64_t pt_uv64()    { auto v=r.uvarint64();w.uvarint64(v);return v; }
    int64_t  pt_v64()     { auto v=r.varint64(); w.varint64(v); return v; }
    std::string pt_str()  { auto v=r.str();      w.str(v);      return v; }

    void pt_bytes(size_t n) { auto v=r.bytes(n); w.bytes(v.data(),v.size()); }

    // Compounds
    void pt_vec3()  { pt_f32le(); pt_f32le(); pt_f32le(); }
    void pt_vec2()  { pt_f32le(); pt_f32le(); }
    void pt_uuid()  { pt_i64le(); pt_i64le(); }

    // NetworkBlockPos (v924): varint X, uvarint Y, varint Z
    void pt_net_block_pos() { pt_varint(); pt_uvarint(); pt_varint(); }

    // BlockPos (v944): varint X, varint Y, varint Z
    void pt_block_pos() { pt_varint(); pt_varint(); pt_varint(); }

    // Convert NetworkBlockPos → BlockPos (clientbound: v924 server → v944 client)
    void map_net_to_block() {
        int32_t x=r.varint(); w.varint(x);
        uint32_t y=r.uvarint(); w.varint((int32_t)y);   // uvarint→varint
        int32_t z=r.varint(); w.varint(z);
    }
    // Convert BlockPos → NetworkBlockPos (serverbound: v944 client → v924 server)
    void map_block_to_net() {
        int32_t x=r.varint(); w.varint(x);
        int32_t y=r.varint(); w.uvarint((uint32_t)y);   // varint→uvarint
        int32_t z=r.varint(); w.varint(z);
    }

    // ItemInstance v944 passthrough
    // wire: varint networkId, [u16 count, uvarint meta, bool hasNetId,
    //        [varint stackNetId], varint blockRtId, uvarint extraLen, bytes]
    void pt_item_v944() {
        int32_t nid=r.varint(); w.varint(nid);
        if(!nid) return;
        pt_u16le();   // count
        pt_uvarint(); // meta
        bool hn=pt_bool(); if(hn) pt_varint(); // stackNetId
        pt_varint();  // blockRuntimeId
        uint32_t el=pt_uvarint(); if(el) pt_bytes(el); // extraData
    }

    // ItemInstance v975 → v944 downgrade
    // v975 wire: int16 Id, u16 stackSize, uvarint auxValue, bool hasNetIdVariant,
    //            [uvarint variantType, varint stackNetId], uvarint blockRtId,
    //            uvarint userDataLen, bytes
    // v944 wire: varint networkId, u16 count, uvarint meta, bool hasNetId,
    //            [varint stackNetId], varint blockRtId, uvarint extraLen, bytes
    void map_item_v975_to_v944() {
        int16_t  id   = r.i16le();
        uint16_t cnt  = r.u16le();
        uint32_t aux  = r.uvarint();
        bool     hniv = r.bool_();
        uint32_t vtype=0; int32_t snid=0;
        if(hniv){ vtype=r.uvarint(); snid=r.varint(); }
        uint32_t brid = r.uvarint();
        uint32_t el   = r.uvarint();
        auto extra    = r.bytes(el);

        // Write v944
        w.varint((int32_t)id);
        if(!id) return;
        w.u16le(cnt);
        w.uvarint(aux);
        w.bool_(hniv);
        if(hniv) w.varint(snid);
        w.varint((int32_t)brid);
        w.uvarint(el);
        w.bytes(extra.data(),extra.size());
    }

    // ItemInstance v944 → v975 upgrade
    void map_item_v944_to_v975() {
        int32_t nid = r.varint(); w.i16le((int16_t)nid);
        if(!nid) return;
        uint16_t cnt  = r.u16le();  w.u16le(cnt);
        uint32_t aux  = r.uvarint();w.uvarint(aux);
        bool     hn   = r.bool_();  w.bool_(hn);
        if(hn){
            w.uvarint(0);           // variantType = 0
            int32_t snid=r.varint();w.varint(snid);
        }
        uint32_t brid=r.varint();   w.uvarint((uint32_t)brid);
        uint32_t el  =r.uvarint();  w.uvarint(el);
        auto extra=r.bytes(el);     w.bytes(extra.data(),extra.size());
    }

    // LevelSettings V944 passthrough (exact field order from Python level_settings.py)
    // V944: default_spawn_y is VAR_INT
    // V924: default_spawn_y is UVAR_INT — only difference
    void pt_level_settings_v944() {
        pt_i64le();    // seed
        pt_i16le();    // spawn_biome_type
        pt_str();      // spawn_biome_name
        pt_varint();   // spawn_dimension
        pt_varint();   // generator
        pt_varint();   // game_type
        pt_bool();     // is_hardcore
        pt_varint();   // game_difficulty
        pt_varint();   // default_spawn_x
        pt_varint();   // default_spawn_y  ← VAR_INT in V944
        pt_varint();   // default_spawn_z
        pt_bool();     // achievements_disabled
        pt_varint();   // editor_world_type
        pt_bool();     // created_in_editor
        pt_bool();     // exported_from_editor
        pt_varint();   // day_cycle_stop_time
        pt_varint();   // education_edition_offer
        pt_bool();     // education_features_enabled
        pt_str();      // education_product_id
        pt_f32le();    // rain_level
        pt_f32le();    // lightning_level
        pt_bool();     // has_confirmed_platform_locked_content
        pt_bool();     // multiplayer_intended
        pt_bool();     // lan_broadcasting_intended
        pt_varint();   // xbox_live_broadcast_setting
        pt_varint();   // platform_broadcast_setting
        pt_bool();     // commands_enabled
        pt_bool();     // texture_packs_required
        // game_rules: uvarint count, then per rule: str + bool + uvarint(type) + value
        uint32_t gr=pt_uvarint();
        for(uint32_t i=0;i<gr;++i){
            pt_str(); pt_bool();
            uint32_t t=pt_uvarint();
            if(t==1) pt_bool(); else if(t==2) pt_uvarint(); else if(t==3) pt_f32le();
        }
        // experiments: uvarint count, then per: str + bool
        uint32_t ec=pt_uvarint();
        for(uint32_t i=0;i<ec;++i){ pt_str(); pt_bool(); }
        pt_bool(); // ever_toggled
        pt_bool(); // has_bonus_chest
        pt_bool(); // start_with_map
        pt_varint(); // player_permissions
        pt_i32le();  // server_chunk_tick_range
        pt_bool(); pt_bool(); pt_bool(); pt_bool(); // locked/template bools x4
        pt_bool(); pt_bool(); pt_bool(); pt_bool(); // villagers/persona/skins/emote x4
        pt_str();    // base_game_version
        pt_i32le(); pt_i32le(); // limited_world_width, limited_world_depth
        pt_bool();   // nether_type
        pt_str();    // edu_shared_uri_button_name
        pt_str();    // edu_shared_uri_link_uri
        // override_force_experimental: OptionalType(BOOL) = bool present + [bool value]
        if(pt_bool()) pt_bool();
        pt_byte();   // chat_restriction_level
        pt_bool();   // disable_player_interactions
    }

    // V924 LevelSettings: same as V944 but default_spawn_y is UVAR_INT
    // Used when reading V924 server packets.
    void pt_level_settings_v924() {
        pt_i64le(); pt_i16le(); pt_str();
        pt_varint(); pt_varint(); pt_varint();
        pt_bool(); pt_varint();
        pt_varint();     // default_spawn_x
        pt_uvarint();    // default_spawn_y ← UVAR_INT in V924
        pt_varint();     // default_spawn_z
        pt_bool(); pt_varint(); pt_bool(); pt_bool(); pt_varint();
        pt_varint(); pt_bool(); pt_str();
        pt_f32le(); pt_f32le();
        pt_bool(); pt_bool(); pt_bool(); pt_varint(); pt_varint();
        pt_bool(); pt_bool();
        uint32_t gr=pt_uvarint();
        for(uint32_t i=0;i<gr;++i){
            pt_str(); pt_bool();
            uint32_t t=pt_uvarint();
            if(t==1) pt_bool(); else if(t==2) pt_uvarint(); else if(t==3) pt_f32le();
        }
        uint32_t ec=pt_uvarint();
        for(uint32_t i=0;i<ec;++i){ pt_str(); pt_bool(); }
        pt_bool(); pt_bool(); pt_bool(); pt_varint(); pt_i32le();
        pt_bool(); pt_bool(); pt_bool(); pt_bool();
        pt_bool(); pt_bool(); pt_bool(); pt_bool();
        pt_str(); pt_i32le(); pt_i32le(); pt_bool(); pt_str(); pt_str();
        if(pt_bool()) pt_bool();
        pt_byte(); pt_bool();
    }

    // Read V924 LevelSettings, write V944 (convert spawn_y UVAR_INT → VAR_INT)
    void map_level_settings_v924_to_v944() {
        pt_i64le(); pt_i16le(); pt_str();
        pt_varint(); pt_varint(); pt_varint();
        pt_bool(); pt_varint();
        pt_varint();                             // spawn_x (same)
        uint32_t y=r.uvarint(); w.varint((int32_t)y); // Y conversion
        pt_varint();                             // spawn_z (same)
        // rest identical — passthrough
        pt_bool(); pt_varint(); pt_bool(); pt_bool(); pt_varint();
        pt_varint(); pt_bool(); pt_str();
        pt_f32le(); pt_f32le();
        pt_bool(); pt_bool(); pt_bool(); pt_varint(); pt_varint();
        pt_bool(); pt_bool();
        uint32_t gr=pt_uvarint();
        for(uint32_t i=0;i<gr;++i){
            pt_str(); pt_bool();
            uint32_t t=pt_uvarint();
            if(t==1) pt_bool(); else if(t==2) pt_uvarint(); else if(t==3) pt_f32le();
        }
        uint32_t ec=pt_uvarint();
        for(uint32_t i=0;i<ec;++i){ pt_str(); pt_bool(); }
        pt_bool(); pt_bool(); pt_bool(); pt_varint(); pt_i32le();
        pt_bool(); pt_bool(); pt_bool(); pt_bool();
        pt_bool(); pt_bool(); pt_bool(); pt_bool();
        pt_str(); pt_i32le(); pt_i32le(); pt_bool(); pt_str(); pt_str();
        if(pt_bool()) pt_bool();
        pt_byte(); pt_bool();
    }

    // Read V944 LevelSettings, write V924 (convert spawn_y VAR_INT → UVAR_INT)
    void map_level_settings_v944_to_v924() {
        pt_i64le(); pt_i16le(); pt_str();
        pt_varint(); pt_varint(); pt_varint();
        pt_bool(); pt_varint();
        pt_varint();                             // spawn_x
        int32_t y=r.varint(); w.uvarint((uint32_t)y); // Y conversion
        pt_varint();                             // spawn_z
        pt_bool(); pt_varint(); pt_bool(); pt_bool(); pt_varint();
        pt_varint(); pt_bool(); pt_str();
        pt_f32le(); pt_f32le();
        pt_bool(); pt_bool(); pt_bool(); pt_varint(); pt_varint();
        pt_bool(); pt_bool();
        uint32_t gr=pt_uvarint();
        for(uint32_t i=0;i<gr;++i){
            pt_str(); pt_bool();
            uint32_t t=pt_uvarint();
            if(t==1) pt_bool(); else if(t==2) pt_uvarint(); else if(t==3) pt_f32le();
        }
        uint32_t ec=pt_uvarint();
        for(uint32_t i=0;i<ec;++i){ pt_str(); pt_bool(); }
        pt_bool(); pt_bool(); pt_bool(); pt_varint(); pt_i32le();
        pt_bool(); pt_bool(); pt_bool(); pt_bool();
        pt_bool(); pt_bool(); pt_bool(); pt_bool();
        pt_str(); pt_i32le(); pt_i32le(); pt_bool(); pt_str(); pt_str();
        if(pt_bool()) pt_bool();
        pt_byte(); pt_bool();
    }

private:
    Reader r; Writer w; UserConn* user; bool cancelled_;
};

// =============================================================================
// 4. UserConn + ConnectionManager
// =============================================================================

class Translator; // forward

struct UserConn {
    std::string addr;
    int  client_proto  = 0;   // 0 until RequestNetworkSettings received
    int  server_proto  = 0;
    bool warned_no_chain = false;

    // Cached translation pipeline, built after client_proto is known
    std::vector<Translator*> sb_chain;  // serverbound
    std::vector<Translator*> cb_chain;  // clientbound
    bool chain_built = false;

    bool needs_xlat() const noexcept { return client_proto && client_proto != server_proto; }

    // Type-keyed per-connection state (Python: dict[type, object])
    template<typename T> void  put(T v) { storage[typeid(T)] = std::move(v); }
    template<typename T> T*    get()    { auto it=storage.find(typeid(T)); return it==storage.end()?nullptr:std::any_cast<T>(&it->second); }
    void clear() { storage.clear(); }

private:
    std::unordered_map<std::type_index, std::any> storage;
};

class ConnMgr {
public:
    explicit ConnMgr(int srv) : srv_(srv) {}

    UserConn& get_or_create(const std::string& addr) {
        std::lock_guard lk(mu_);
        auto [it,ins] = map_.emplace(addr, UserConn{});
        if(ins){ it->second.addr=addr; it->second.server_proto=srv_; }
        return it->second;
    }
    UserConn* get(const std::string& addr) {
        std::lock_guard lk(mu_);
        auto it=map_.find(addr); return it==map_.end()?nullptr:&it->second;
    }
    void remove(const std::string& addr) {
        std::lock_guard lk(mu_);
        auto it=map_.find(addr); if(it!=map_.end()){ it->second.clear(); map_.erase(it); }
    }

private:
    int srv_; std::mutex mu_; std::unordered_map<std::string,UserConn> map_;
};

// =============================================================================
// 5. Protocol constants — EXACT values from Python source
//    Source: endstone_endweave/codec/types/enums.py
//            endstone_endweave/protocol/packet_ids.py
//            endstone_endweave/protocol/mappings/*.py
// =============================================================================

namespace pkt {
    // Packet IDs (exact, from packet_ids.py)
    constexpr uint32_t LOGIN                    = 1;
    constexpr uint32_t RESOURCE_PACK_STACK      = 7;
    constexpr uint32_t START_GAME               = 11;
    constexpr uint32_t ADD_PLAYER               = 12;
    constexpr uint32_t ADD_ACTOR                = 13;
    constexpr uint32_t ADD_ITEM_ACTOR           = 15;
    constexpr uint32_t UPDATE_BLOCK             = 21;
    constexpr uint32_t TILE_EVENT               = 26;
    constexpr uint32_t ACTOR_EVENT              = 27;
    constexpr uint32_t INVENTORY_TRANSACTION    = 30;
    constexpr uint32_t PLAYER_EQUIPMENT         = 31;   // MobEquipment
    constexpr uint32_t PLAYER_ACTION            = 36;
    constexpr uint32_t SET_ACTOR_DATA           = 39;
    constexpr uint32_t ANIMATE                  = 44;
    constexpr uint32_t CONTAINER_OPEN           = 46;
    constexpr uint32_t INVENTORY_SLOT           = 50;
    constexpr uint32_t CRAFTING_DATA            = 52;
    constexpr uint32_t BLOCK_ACTOR_DATA         = 56;
    constexpr uint32_t MAP_DATA                 = 67;
    constexpr uint32_t AVAILABLE_COMMANDS       = 76;
    constexpr uint32_t COMMAND_REQUEST          = 77;
    constexpr uint32_t COMMAND_BLOCK_UPDATE     = 78;
    constexpr uint32_t LEGACY_TELEMETRY_EVENT   = 65;
    constexpr uint32_t PLAY_SOUND               = 86;   // NOT 87!
    constexpr uint32_t STOP_SOUND               = 87;
    constexpr uint32_t STRUCTURE_BLOCK_UPDATE   = 90;
    constexpr uint32_t PLAYER_ENCHANT_OPTIONS   = 146;
    constexpr uint32_t PACKET_VIOLATION_WARNING = 156;
    constexpr uint32_t ADD_VOLUME_ENTITY        = 166;
    constexpr uint32_t UPDATE_SUB_CHUNK_BLOCKS  = 172;
    constexpr uint32_t LECTERN_UPDATE           = 125;
    constexpr uint32_t ANVIL_DAMAGE             = 141;
    constexpr uint32_t LEVEL_SOUND_EVENT        = 123;
    constexpr uint32_t NETWORK_SETTINGS         = 143;
    constexpr uint32_t REQUEST_NETWORK_SETTINGS = 193;
    constexpr uint32_t UPDATE_BLOCK_SYNCED      = 110;
    constexpr uint32_t OPEN_SIGN                = 303;
    constexpr uint32_t BOOK_EDIT                = 97;
    constexpr uint32_t CAMERA_AIM_ASSIST_PRESETS= 320;   // NOT 317!
    constexpr uint32_t GRAPHICS_PARAMETER_OVERRIDE=331;  // NOT 322!
    constexpr uint32_t SERVERBOUND_DATA_STORE   = 332;
    constexpr uint32_t CLIENTBOUND_DATA_STORE   = 330;
    constexpr uint32_t BIOME_DEFINITION_LIST    = 122;
    constexpr uint32_t CAMERA_INSTRUCTION       = 300;
    constexpr uint32_t SERVER_SCRIPT_DEBUG_DRAWER=328;
    constexpr uint32_t CLIENT_MOVEMENT_PREDICTION_SYNC=322;  // NOT same as GRAPHICS!
    constexpr uint32_t UPDATE_CLIENT_OPTIONS    = 323;
    constexpr uint32_t VOXEL_SHAPES             = 337;
    constexpr uint32_t CLIENTBOUND_DD_SHOW      = 333; // CLIENTBOUND_DATA_DRIVEN_UI_SHOW_SCREEN
    constexpr uint32_t CLIENTBOUND_DD_CLOSE     = 334; // CLIENTBOUND_DATA_DRIVEN_UI_CLOSE_ALL_SCREENS
    constexpr uint32_t LOCATOR_BAR              = 341;
    constexpr uint32_t PARTY_CHANGED            = 342;
    constexpr uint32_t SERVERBOUND_DIAGNOSTICS  = 315;
    constexpr uint32_t RESOURCE_PACKS_READY_FOR_VALIDATION = 340;
    constexpr uint32_t SERVERBOUND_DD_CLOSED    = 343; // SERVERBOUND_DATA_DRIVEN_SCREEN_CLOSED
    constexpr uint32_t SYNC_WORLD_CLOCKS           = 344;
    constexpr uint32_t CLIENTBOUND_ATTR_LAYER_SYNC = 345; // CLIENTBOUND_ATTRIBUTE_LAYER_SYNC
    constexpr uint32_t PLAYER_CLIENT_INPUT_PERMISSIONS = 196;
    constexpr uint32_t CAMERA_SPLINE            = 338;

    // Lookup name for logging
    inline const char* name(uint32_t id) noexcept {
        switch(id){
        case 1:   return "Login";
        case 11:  return "StartGame";
        case 12:  return "AddPlayer";
        case 13:  return "AddActor";
        case 15:  return "AddItemActor";
        case 21:  return "UpdateBlock";
        case 26:  return "TileEvent";
        case 27:  return "ActorEvent";
        case 30:  return "InventoryTransaction";
        case 31:  return "PlayerEquipment";
        case 36:  return "PlayerAction";
        case 39:  return "SetActorData";
        case 50:  return "InventorySlot";
        case 52:  return "CraftingData";
        case 56:  return "BlockActorData";
        case 86:  return "PlaySound";
        case 110: return "UpdateBlockSynced";
        case 123: return "LevelSoundEvent";
        case 125: return "LecternUpdate";
        case 156: return "PacketViolationWarning";
        case 193: return "RequestNetworkSettings";
        case 322: return "ClientMovementPredictionSync";
        case 323: return "UpdateClientOptions";
        case 337: return "VoxelShapes";
        case 341: return "LocatorBar";
        default:  return "?";
        }
    }
}

// IdShift — exact port of Python MappingData.inserted() / IdShift
struct IdShift {
    uint32_t at, n;
    constexpr uint32_t up  (uint32_t v) const noexcept { return v>=at ? v+n : v; }
    constexpr uint32_t down(uint32_t v) const noexcept {
        if(v>=at+n) return v-n; if(v>=at) return at; return v;
    }
};
constexpr IdShift inserted(uint32_t n, uint32_t at) noexcept { return {at,n}; }

// Mapping data — exact from Python protocol/mappings/*.py
// ActorDataIDs::HEARTBEAT_SOUND_EVENT = 126  (from enums.py line 140)
// AIM_ASSIST_PRIORITY_PRESET_ID = 136, CATEGORY_ID = 137, ACTOR_ID = 138

struct Mapping {
    IdShift                      sound;
    uint32_t                     sound_key;         // ActorData key for heartbeat sound
    std::optional<IdShift>       actor_event;
    std::optional<IdShift>       note_instrument;   // NoteBlockInstrument
    std::unordered_set<uint32_t> dropped_data_keys; // ActorData keys to filter
};

// v859↔v860: empty protocol (only login version differs, handled by base)
// v860↔v898: 12 new sounds at 566, 1 new ActorEvent at 80 (KINETIC_DAMAGE_DEALT)
inline Mapping map_v860_v898() { return { inserted(12,566), 126, inserted(1,80) }; }
// v898↔v924: 19 new sounds at 578, drop AimAssist keys 136/137/138
inline Mapping map_v898_v924() { return { inserted(19,578), 126, {}, {}, {136,137,138} }; }
// v924↔v944: 2 new sounds at 597, NoteBlockInstrument 4 new at 16
inline Mapping map_v924_v944() { return { inserted(2,597), 126, {}, inserted(4,16) }; }
// v944↔v975: 2 new sounds at 599
inline Mapping map_v944_v975() { return { inserted(2,599), 126 }; }

// =============================================================================
// 6. PacketTranslator + TranslatorRegistry + Pipeline
// =============================================================================

using HandlerFn = std::function<void(Wrapper&)>;
enum class Dir { SB, CB };

class Translator {
public:
    Translator(int srv, int cli, std::string name, bool base=false)
        : srv_(srv), cli_(cli), name_(std::move(name)), base_(base) {}
    virtual ~Translator()=default;

    int  srv()  const noexcept { return srv_; }
    int  cli()  const noexcept { return cli_; }
    const std::string& name() const noexcept { return name_; }
    bool is_base() const noexcept { return base_; }
    void set_base(bool v) noexcept { base_=v; }

    void on_cb(uint32_t id, HandlerFn fn)  { cb_[id]=std::move(fn); }
    void on_sb(uint32_t id, HandlerFn fn)  { sb_[id]=std::move(fn); }
    void cancel_cb(uint32_t id) { cancel_cb_.insert(id); }
    void cancel_sb(uint32_t id) { cancel_sb_.insert(id); }

    virtual void init(UserConn&) {}

    bool has_work(Dir d, uint32_t id) const noexcept {
        if(d==Dir::CB) return cb_.count(id)||cancel_cb_.count(id);
        return sb_.count(id)||cancel_sb_.count(id);
    }

    // Returns false = packet dropped
    bool apply(Dir d, uint32_t id, Wrapper& w) const {
        if(d==Dir::CB){
            if(cancel_cb_.count(id)){ w.cancel(); return false; }
            auto it=cb_.find(id); if(it!=cb_.end()){ it->second(w); return !w.cancelled(); }
        } else {
            if(cancel_sb_.count(id)){ w.cancel(); return false; }
            auto it=sb_.find(id); if(it!=sb_.end()){ it->second(w); return !w.cancelled(); }
        }
        return true;
    }

protected:
    int srv_,cli_; std::string name_; bool base_;
    std::unordered_map<uint32_t,HandlerFn> cb_,sb_;
    std::unordered_set<uint32_t> cancel_cb_,cancel_sb_;
};

class Registry {
public:
    void add_base(std::shared_ptr<Translator> t){ t->set_base(true); base_.push_back(std::move(t)); }
    void add(std::shared_ptr<Translator> t)     { all_[key(t->srv(),t->cli())]=t; cache_.clear(); }

    const std::vector<std::shared_ptr<Translator>>& base() const noexcept { return base_; }

    // BFS: find chain from client → server
    std::optional<std::vector<Translator*>> path(int srv, int cli) {
        if(srv==cli) return std::vector<Translator*>{};
        uint64_t k=key(srv,cli);
        auto it=cache_.find(k); if(it!=cache_.end()) return it->second;
        auto r=bfs(srv,cli); cache_[k]=r; return r;
    }

    std::vector<int> clients(int srv) {
        std::vector<int> r{srv};
        for(auto&[k,t]:all_){
            int c=(int)(k&0xFFFFFFFF);
            if(c!=srv && path(srv,c).has_value()) r.push_back(c);
        }
        return r;
    }

private:
    std::vector<std::shared_ptr<Translator>> base_;
    std::unordered_map<uint64_t,std::shared_ptr<Translator>> all_;
    std::unordered_map<uint64_t,std::optional<std::vector<Translator*>>> cache_;

    static uint64_t key(int s,int c){ return ((uint64_t)(uint32_t)s<<32)|(uint32_t)c; }

    std::optional<std::vector<Translator*>> bfs(int srv, int cli){
        std::unordered_map<int,std::vector<Translator*>> adj;
        for(auto&[k,t]:all_) adj[t->cli()].push_back(t.get());
        std::unordered_map<int,bool> vis; vis[cli]=true;
        using St=std::pair<int,std::vector<Translator*>>;
        std::deque<St> q; q.push_back({cli,{}});
        while(!q.empty()){
            auto[cur,path]=q.front(); q.pop_front();
            for(auto* t:adj[cur]){
                int nxt=t->srv(); auto np=path; np.push_back(t);
                if(nxt==srv) return np;
                if(!vis[nxt]){ vis[nxt]=true; q.push_back({nxt,std::move(np)}); }
            }
        }
        return std::nullopt;
    }
};

// Result of pipeline processing
struct PipelineResult {
    enum class Action { PASSTHROUGH, REWRITTEN, CANCELLED } action;
    std::vector<uint8_t> payload;
};

using LogFn = std::function<void(int/*level*/,const std::string&)>;

class Pipeline {
public:
    Pipeline(Registry& reg, ConnMgr& conns, LogFn log)
        : reg_(reg), conns_(conns), log_(std::move(log)) {}

    PipelineResult serverbound(const std::string& addr, uint32_t id,
                               std::span<const uint8_t> raw) {
        auto& conn = conns_.get_or_create(addr);
        ensure_chain(conn);
        if(!conn.needs_xlat()) return {PipelineResult::Action::PASSTHROUGH,{}};
        return run(Dir::SB, id, raw, conn.sb_chain, conn);
    }

    PipelineResult clientbound(const std::string& addr, uint32_t id,
                               std::span<const uint8_t> raw) {
        auto* conn=conns_.get(addr);
        if(!conn||!conn->chain_built||!conn->needs_xlat())
            return {PipelineResult::Action::PASSTHROUGH,{}};
        return run(Dir::CB, id, raw, conn->cb_chain, *conn);
    }

private:
    Registry& reg_; ConnMgr& conns_; LogFn log_;

    void log(int lv, const std::string& m){ if(log_) log_(lv,m); }

    void ensure_chain(UserConn& conn){
        if(conn.chain_built) return;

        std::vector<Translator*> base;
        for(auto& b:reg_.base()) base.push_back(b.get());

        if(!conn.client_proto){ conn.sb_chain=base; return; }
        if(!conn.needs_xlat()){
            conn.sb_chain=conn.cb_chain=base; conn.chain_built=true; return;
        }

        auto opt=reg_.path(conn.server_proto,conn.client_proto);
        if(!opt){
            if(!conn.warned_no_chain){
                conn.warned_no_chain=true;
                log(2,"[EW] No chain: srv="+std::to_string(conn.server_proto)+
                      " cli="+std::to_string(conn.client_proto)+" "+conn.addr);
            }
            conn.sb_chain=conn.cb_chain=base; conn.chain_built=true; return;
        }

        auto& chain=*opt;
        for(auto* t:chain) t->init(conn);

        conn.sb_chain=base;
        conn.sb_chain.insert(conn.sb_chain.end(),chain.begin(),chain.end());

        conn.cb_chain=base;
        conn.cb_chain.insert(conn.cb_chain.end(),chain.rbegin(),chain.rend());
        conn.chain_built=true;

        log(1,"[EW] Chain built: "+std::to_string(conn.client_proto)+"<->"+
             std::to_string(conn.server_proto)+" hops="+std::to_string(chain.size())+" "+conn.addr);
    }

    PipelineResult run(Dir dir, uint32_t id, std::span<const uint8_t> raw,
                       const std::vector<Translator*>& chain, UserConn& conn){
        std::vector<uint8_t> cur(raw.begin(),raw.end());
        bool changed=false;
        for(auto* t:chain){
            if(!t->has_work(dir,id)) continue;
            Wrapper w(cur,&conn);
            if(!t->apply(dir,id,w)){
                log(0,"[EW] DROP pkt="+std::to_string(id)+"("+std::string(pkt::name(id))+") by "+t->name());
                return {PipelineResult::Action::CANCELLED,{}};
            }
            auto next=w.finish();
            if(next!=cur) changed=true;
            cur=std::move(next);
        }
        if(!changed) return {PipelineResult::Action::PASSTHROUGH,{}};
        log(0,"[EW] XLAT pkt="+std::to_string(id)+"("+std::string(pkt::name(id))+")");
        return {PipelineResult::Action::REWRITTEN,std::move(cur)};
    }
};

} // namespace ew
