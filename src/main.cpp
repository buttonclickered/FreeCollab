// FreeCollab - live collaborative level editing for Geometry Dash (Geode v5 / GD 2.2081)
//
// UI: in the editor, open the pause menu and press the "Collab" button (bottom right).
//     Type a room code, press Join. Everyone with the same code edits together.
//
// Rooms belong to ONE level. The first person to join a room claims it for their level
// (by level name). Anyone who joins from a different level gets that room's level opened
// (or created, if they don't have one) so the host's objects load into a matching level,
// and their own level is saved and left alone.
//
// Sync: every second we push local changes to Supabase and pull other people's changes.
// Remote changes never touch an object you currently have selected (that used to crash);
// they are held back and applied as soon as you deselect it.

#include <Geode/Geode.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <chrono>
#include <functional>
#include <random>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

// Public values (the publishable key is meant to be public). Change if you move projects.
static constexpr auto SUPABASE_URL = "https://qobayzqeiltrqairxmuj.supabase.co";
static constexpr auto SUPABASE_KEY = "sb_publishable_rAImNNuAr9h0WmUyl54DDw_I8cIfPN4";
static constexpr float SYNC_INTERVAL = 1.0f;
static constexpr size_t PULL_LIMIT = 500;

// ---------- helpers ----------

static std::string randomHex(size_t n = 16) {
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) s += hex[rng() & 0xF];
    return s;
}

static std::string sanitize(std::string const& in) {
    std::string out;
    for (char c : in) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') out += c;
    }
    return out;
}

// Escape a string for use inside a JSON "..." value.
static std::string jsonEscape(std::string const& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
                else out += static_cast<char>(c);
        }
    }
    return out;
}

static std::string baseUrl() {
    return std::string(SUPABASE_URL) + "/rest/v1/collab_objects";
}

static web::WebRequest makeRequest() {
    web::WebRequest req;
    // new sb_publishable_ keys are not JWTs: send them ONLY in the apikey header
    req.header("apikey", SUPABASE_KEY);
    req.header("Content-Type", "application/json");
    return req;
}

// A room's level is stored as one extra row in a separate "<room>~meta" room.
// "~" can't appear in a real room code, and older versions never read that room,
// so this doesn't disturb them.
static std::string metaRoom(std::string const& room) { return room + "~meta"; }
static constexpr auto META_UID = "level";

// ---------- per-object stable id ----------

class $modify(CollabObject, GameObject) {
    struct Fields {
        std::string collabId;
    };
};

static std::string& idOf(GameObject* obj) {
    return static_cast<CollabObject*>(obj)->m_fields->collabId;
}

// ---------- shared sync state (one editor open at a time) ----------

namespace {
    // a remote change we couldn't apply yet because the object is selected locally
    struct Deferred {
        std::string data;
        bool deleted = false;
    };

    struct SyncState {
        std::string clientId = randomHex(12);
        std::string room; // empty = not in a room
        long long lastSeq = 0;
        bool pushInFlight = false;
        bool pullInFlight = false;
        int joinGen = 0; // bumped on every join/leave so stale network replies are ignored
        // last time the editor's sync timer fired; if it stops, the editor is gone
        std::chrono::steady_clock::time_point lastTick{};
        // uid -> last save string we know the server has (or applied locally); "" = deleted
        std::unordered_map<std::string, std::string> synced;
        // batch currently being pushed
        std::unordered_map<std::string, std::string> pending;
        // uid -> remote change waiting for the local user to deselect the object
        std::unordered_map<std::string, Deferred> deferred;
    };
    SyncState S;
    async::TaskHolder<web::WebResponse> pushHolder;
    async::TaskHolder<web::WebResponse> pullHolder;
    async::TaskHolder<web::WebResponse> metaHolder;
    async::TaskHolder<web::WebResponse> metaWriteHolder;
}

static bool editorAlive() {
    return std::chrono::steady_clock::now() - S.lastTick < std::chrono::seconds(5);
}

static void resetSync() {
    pushHolder.cancel();
    pullHolder.cancel();
    S.lastSeq = 0;
    S.pushInFlight = S.pullInFlight = false;
    S.synced.clear();
    S.pending.clear();
    S.deferred.clear();
}

static void joinRoom(std::string const& room) {
    resetSync();
    S.room = room;
    Mod::get()->setSavedValue<std::string>("room-code", room);
}

static std::string levelNameOf(LevelEditorLayer* editor) {
    if (!editor || !editor->m_level) return "";
    return std::string(editor->m_level->m_levelName);
}

// ---------- apply remote changes ----------

// Is this object selected in the editor right now? Touching a selected object (deleting
// or recreating it) leaves the editor UI pointing at freed memory, which crashes.
static bool isSelected(LevelEditorLayer* editor, GameObject* obj) {
    auto ui = editor->m_editorUI;
    if (!ui) return false;
    if (ui->m_selectedObject == obj) return true;
    return ui->m_selectedObjects && ui->m_selectedObjects->containsObject(obj);
}

static std::unordered_map<std::string, GameObject*> buildIndex(LevelEditorLayer* editor) {
    std::unordered_map<std::string, GameObject*> byId;
    for (auto obj : CCArrayExt<GameObject*>(editor->m_objects)) {
        if (!idOf(obj).empty()) byId[idOf(obj)] = obj;
    }
    return byId;
}

// Apply one remote change. Returns false if it had to be held back.
static bool applyOne(
    LevelEditorLayer* editor,
    std::unordered_map<std::string, GameObject*>& byId,
    std::string const& uid, std::string const& data, bool deleted,
    bool fromDeferred = false
) {
    auto found = byId.find(uid);

    if (found == byId.end() && fromDeferred) {
        // the local user already removed it themselves; don't bring it back
        S.deferred.erase(uid);
        return true;
    }

    if (found != byId.end() && isSelected(editor, found->second)) {
        S.deferred[uid] = Deferred{data, deleted};
        return false;
    }

    S.deferred.erase(uid);

    if (found != byId.end()) {
        editor->removeObject(found->second, true); // API: removeObject(GameObject*, bool noUndo)
        byId.erase(found);
    }

    if (deleted) {
        S.synced[uid] = "";
        return true;
    }

    // API: createObjectsFromString(std::string const&, bool noUndo, bool noLimit) -> CCArray*
    auto created = editor->createObjectsFromString(data, true, true);
    if (created && created->count() > 0) {
        auto obj = static_cast<GameObject*>(created->objectAtIndex(0));
        idOf(obj) = uid;
        byId[uid] = obj;
        // store OUR normalized form so we don't echo it back as a "local change"
        S.synced[uid] = std::string(obj->getSaveString(editor));
    } else {
        S.synced[uid] = data;
    }
    return true;
}

static void flushDeferred(LevelEditorLayer* editor, std::unordered_map<std::string, GameObject*>& byId) {
    if (S.deferred.empty()) return;
    std::vector<std::string> ids;
    for (auto const& kv : S.deferred) ids.push_back(kv.first);
    for (auto const& id : ids) {
        auto it = S.deferred.find(id);
        if (it == S.deferred.end()) continue;
        Deferred d = it->second; // copy: applyOne edits the map
        applyOne(editor, byId, id, d.data, d.deleted, true);
    }
}

// Returns how many rows the server sent.
static size_t applyRemote(LevelEditorLayer* editor, matjson::Value const& rows) {
    if (!rows.isArray()) return 0;

    auto byId = buildIndex(editor);
    size_t count = 0;

    for (auto const& row : rows) {
        count++;
        auto uid = row["uid"].asString().unwrapOr("");
        auto author = row["author"].asString().unwrapOr("");
        auto data = row["data"].asString().unwrapOr("");
        bool deleted = row["deleted"].asBool().unwrapOr(false);
        long long seq = row["seq"].asInt().unwrapOr(0);
        if (seq > S.lastSeq) S.lastSeq = seq;
        if (uid.empty() || author == S.clientId) continue;

        applyOne(editor, byId, uid, data, deleted);
    }

    flushDeferred(editor, byId);
    return count;
}

// ---------- pull ----------

static void doPull() {
    if (S.pullInFlight) return;
    S.pullInFlight = true;

    auto room = S.room;
    auto url = fmt::format(
        "{}?room=eq.{}&seq=gt.{}&order=seq.asc&limit={}",
        baseUrl(), room, S.lastSeq, PULL_LIMIT
    );

    pullHolder.spawn(
        makeRequest().get(url),
        [room](web::WebResponse res) { // runs on the main thread
            S.pullInFlight = false;
            if (room != S.room) return; // left or switched rooms meanwhile
            if (!res.ok()) {
                log::warn("FreeCollab pull failed: {}", res.code());
                return;
            }
            // if the editor was closed while we waited, its sync timer stopped
            if (!editorAlive()) return;
            auto editor = LevelEditorLayer::get();
            if (!editor || !editor->m_editorUI) return;
            // don't touch objects during a playtest; lastSeq isn't advanced, so we re-pull later
            if (editor->m_playbackMode == PlaybackMode::Playing) return;

            auto json = res.json().unwrapOr(matjson::Value());
            auto n = applyRemote(editor, json);
            if (n >= PULL_LIMIT) doPull(); // more rows waiting (big level): keep going
        }
    );
}

// ---------- push (diff local objects vs last synced) ----------

static void doPush(LevelEditorLayer* editor) {
    if (S.pushInFlight) return;

    std::unordered_map<std::string, std::string> changes;
    std::unordered_map<std::string, bool> seen;

    for (auto obj : CCArrayExt<GameObject*>(editor->m_objects)) {
        auto& id = idOf(obj);
        if (id.empty()) id = randomHex(16);
        seen[id] = true;

        std::string data = obj->getSaveString(editor);
        auto f = S.synced.find(id);
        if (f == S.synced.end() || f->second != data) changes[id] = data;
    }
    for (auto const& [id, data] : S.synced) {
        if (!data.empty() && !seen.count(id)) changes[id] = "";
    }
    if (changes.empty()) return;

    std::string body = "[";
    bool first = true;
    for (auto const& [id, data] : changes) {
        if (!first) body += ",";
        first = false;
        body += fmt::format(
            R"({{"room":"{}","uid":"{}","data":"{}","deleted":{},"author":"{}"}})",
            jsonEscape(S.room), jsonEscape(id), jsonEscape(data),
            data.empty() ? "true" : "false", S.clientId
        );
        // the local user changed this object themselves, so their version wins
        S.deferred.erase(id);
    }
    body += "]";

    S.pushInFlight = true;
    S.pending = std::move(changes);

    auto req = makeRequest();
    req.header("Prefer", "resolution=merge-duplicates,return=minimal");
    req.bodyString(body);

    pushHolder.spawn(
        req.post(baseUrl() + "?on_conflict=room,uid"),
        [](web::WebResponse res) { // runs on the main thread
            S.pushInFlight = false;
            if (res.ok()) {
                for (auto const& [id, data] : S.pending) S.synced[id] = data;
            } else {
                log::warn("FreeCollab push failed: {}", res.code());
            }
            S.pending.clear();
        }
    );
}

// ---------- room <-> level matching ----------

// Ask the server which level a room belongs to.
// done(ok, exists, levelName)
static void fetchRoomLevel(
    std::string const& room,
    std::function<void(bool, bool, std::string)> done
) {
    auto url = fmt::format(
        "{}?room=eq.{}&uid=eq.{}&select=data&limit=1",
        baseUrl(), metaRoom(room), META_UID
    );
    metaHolder.spawn(
        makeRequest().get(url),
        [done](web::WebResponse res) {
            if (!res.ok()) {
                log::warn("FreeCollab room lookup failed: {}", res.code());
                done(false, false, "");
                return;
            }
            auto json = res.json().unwrapOr(matjson::Value());
            bool exists = false;
            std::string name;
            if (json.isArray()) {
                for (auto const& row : json) {
                    name = row["data"].asString().unwrapOr("");
                    exists = true;
                    break;
                }
            }
            done(true, exists, name);
        }
    );
}

// Claim a room for a level. If someone already claimed it, the server keeps theirs.
static void claimRoom(std::string const& room, std::string const& levelName) {
    auto body = fmt::format(
        R"([{{"room":"{}","uid":"{}","data":"{}","deleted":false,"author":"{}"}}])",
        jsonEscape(metaRoom(room)), META_UID, jsonEscape(levelName), S.clientId
    );
    auto req = makeRequest();
    req.header("Prefer", "resolution=ignore-duplicates,return=minimal");
    req.bodyString(body);
    metaWriteHolder.spawn(
        req.post(baseUrl() + "?on_conflict=room,uid"),
        [](web::WebResponse res) {
            if (!res.ok()) log::warn("FreeCollab room claim failed: {}", res.code());
        }
    );
}

// Leave this level (saving it first) and open the level the room belongs to,
// creating it if this player doesn't have one with that name yet.
static void openRoomLevel(std::string const& room, std::string const& levelName) {
    auto manager = LocalLevelManager::get();
    if (!manager || !manager->m_localLevels) {
        Notification::create("FreeCollab: couldn't access your levels", NotificationIcon::Error)->show();
        return;
    }

    // keep whatever the player was working on
    if (auto editor = LevelEditorLayer::get()) editor->saveLevel();

    GJGameLevel* target = nullptr;
    for (auto lvl : CCArrayExt<GJGameLevel*>(manager->m_localLevels)) {
        if (std::string(lvl->m_levelName) == levelName) {
            target = lvl;
            break;
        }
    }

    bool created = false;
    if (!target) {
        target = GJGameLevel::create();
        target->m_levelName = levelName;
        target->m_levelType = GJLevelType::Editor;
        manager->m_localLevels->insertObject(target, 0);
        created = true;
    }

    // Make sure nothing from the level we're leaving gets pushed into the room.
    // The new editor joins the saved room by itself once it has checked the level matches.
    ++S.joinGen;
    S.room.clear();
    resetSync();
    Mod::get()->setSavedValue<std::string>("room-code", room);

    Notification::create(
        (created ? "FreeCollab: created level \"" : "FreeCollab: opening level \"") + levelName + "\"",
        NotificationIcon::Info
    )->show();

    CCDirector::get()->replaceScene(
        CCTransitionFade::create(0.5f, LevelEditorLayer::scene(target, false))
    );
}

// Player pressed Join.
static void beginJoin(std::string const& room) {
    auto myLevel = levelNameOf(LevelEditorLayer::get());
    int gen = ++S.joinGen;
    Notification::create("FreeCollab: checking room...", NotificationIcon::Loading)->show();

    fetchRoomLevel(room, [room, myLevel, gen](bool ok, bool exists, std::string roomLevel) {
        if (gen != S.joinGen) return; // superseded
        if (!ok) {
            Notification::create("FreeCollab: couldn't reach the server", NotificationIcon::Error)->show();
            return;
        }
        if (!exists) {
            // brand new room: it belongs to the level we're in now
            joinRoom(room);
            claimRoom(room, myLevel);
            Notification::create("FreeCollab: created room " + room, NotificationIcon::Success)->show();
            return;
        }
        if (roomLevel.empty() || roomLevel == myLevel) {
            joinRoom(room);
            Notification::create("FreeCollab: joined room " + room, NotificationIcon::Success)->show();
            return;
        }
        // the room is for a different level
        openRoomLevel(room, roomLevel);
    });
}

// Editor just opened and we remember a room: rejoin it only if this is the room's level.
static void autoJoin(std::string const& room, std::string const& myLevel) {
    int gen = ++S.joinGen;
    fetchRoomLevel(room, [room, myLevel, gen](bool ok, bool exists, std::string roomLevel) {
        if (gen != S.joinGen) return;
        if (!ok) {
            Notification::create("FreeCollab: couldn't check room (offline?)", NotificationIcon::Error)->show();
            return;
        }
        if (exists && (roomLevel.empty() || roomLevel == myLevel)) {
            joinRoom(room);
            Notification::create("FreeCollab: joined room " + room, NotificationIcon::Success)->show();
        } else if (exists) {
            Notification::create(
                "FreeCollab: room " + room + " is for level \"" + roomLevel + "\" - press Collab > Join to open it",
                NotificationIcon::Info
            )->show();
        }
    });
}

// ---------- editor hook: runs the sync loop ----------

class $modify(CollabEditor, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool p1) {
        if (!LevelEditorLayer::init(level, p1)) return false;

        // start fresh each time the editor opens
        resetSync();
        S.room.clear();
        S.lastTick = std::chrono::steady_clock::now();

        this->schedule(schedule_selector(CollabEditor::collabTick), SYNC_INTERVAL);

        // remember the last room, but only rejoin it if this is that room's level
        auto saved = Mod::get()->getSavedValue<std::string>("room-code", "");
        if (!saved.empty() && level) autoJoin(saved, std::string(level->m_levelName));
        return true;
    }

    void collabTick(float) {
        S.lastTick = std::chrono::steady_clock::now();
        if (S.room.empty()) return;
        // don't touch objects while the level is being play-tested
        if (this->m_playbackMode == PlaybackMode::Playing) return;

        doPush(this);
        doPull();

        // apply held-back remote changes as soon as the object isn't selected any more
        if (!S.deferred.empty()) {
            auto byId = buildIndex(this);
            flushDeferred(this, byId);
        }
    }
};

// ---------- UI: popup with room code ----------

class CollabPopup : public Popup {
protected:
    TextInput* m_input = nullptr;

    bool init() {
        if (!Popup::init(300.f, 180.f)) return false;
        this->setTitle("FreeCollab");

        auto info = CCLabelBMFont::create(
            S.room.empty() ? "Not in a room" : ("In room: " + S.room).c_str(), "chatFont.fnt"
        );
        info->setScale(0.8f);
        m_mainLayer->addChildAtPosition(info, Anchor::Center, {0, 40});

        m_input = TextInput::create(200.f, "room code", "bigFont.fnt");
        m_input->setFilter("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_");
        m_input->setMaxCharCount(32);
        m_input->setString(S.room);
        m_mainLayer->addChildAtPosition(m_input, Anchor::Center, {0, 5});

        auto joinBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Join"), this, menu_selector(CollabPopup::onJoin)
        );
        m_buttonMenu->addChildAtPosition(joinBtn, Anchor::Bottom, {-50, 30});

        auto leaveBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Leave"), this, menu_selector(CollabPopup::onLeave)
        );
        m_buttonMenu->addChildAtPosition(leaveBtn, Anchor::Bottom, {50, 30});

        return true;
    }

    void onJoin(CCObject*) {
        auto room = sanitize(m_input->getString());
        if (room.empty()) {
            Notification::create("Type a room code first", NotificationIcon::Error)->show();
            return;
        }
        beginJoin(room);
        this->onClose(nullptr);
    }

    void onLeave(CCObject*) {
        ++S.joinGen; // ignore any join still in progress
        resetSync();
        S.room.clear();
        // stop rejoining this room automatically next time the editor opens
        Mod::get()->setSavedValue<std::string>("room-code", "");
        Notification::create("FreeCollab: left the room", NotificationIcon::Info)->show();
        this->onClose(nullptr);
    }

public:
    static CollabPopup* create() {
        auto ret = new CollabPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------- UI: "Collab" button in the editor pause menu ----------

class $modify(CollabPause, EditorPauseLayer) {
    bool init(LevelEditorLayer* editor) {
        if (!EditorPauseLayer::init(editor)) return false;

        auto spr = ButtonSprite::create("Collab");
        spr->setScale(0.8f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(CollabPause::onCollab));

        auto menu = CCMenu::create();
        menu->addChild(btn);
        auto win = CCDirector::get()->getWinSize();
        menu->setPosition({win.width - 55.f, 30.f}); // bottom right; change if it overlaps
        this->addChild(menu);
        return true;
    }

    void onCollab(CCObject*) {
        CollabPopup::create()->show();
    }
};