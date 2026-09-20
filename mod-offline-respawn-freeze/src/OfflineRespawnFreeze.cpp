/*
 * mod-offline-respawn-freeze
 *
 * Haelt die Respawn-Zeit von Kreaturen an, solange niemand spielt.
 *
 * Hintergrund: creature_respawn.respawnTime ist eine absolute Unix-Zeit. Ohne
 * Zutun laeuft die Respawnzeit daher auch dann weiter, wenn niemand spielt.
 * Das Modul gleicht zwei Spannen aus:
 *
 *   1. Server aus. Beim Start werden die offenen Zeitpunkte um die Ausfallzeit
 *      nach hinten verschoben.
 *   2. Server laeuft, aber kein menschlicher Spieler ist angemeldet. Beim
 *      Logout wird der Bestand vermerkt, beim naechsten Login um genau diese
 *      Spanne verschoben. Was die Bots waehrenddessen erlegen, bleibt
 *      unberuehrt und respawnt normal.
 *
 * GameObjects (Kraeuter, Erz, Truhen) bleiben unberuehrt, ebenso Auktionen,
 * Post, Kalender und die Weltzeit.
 *
 * Siehe docs/freeze-konzept.md fuer die ausfuehrliche Begruendung.
 */

#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

namespace
{
    constexpr char const* STATE_TABLE    = "mod_offline_respawn_freeze";
    constexpr char const* SNAPSHOT_TABLE = "mod_offline_respawn_freeze_snapshot";
    constexpr uint32 SECONDS_PER_DAY     = 86400;

    struct Settings
    {
        bool   enabled          = true;
        bool   dryRun           = false;
        bool   includeInstances = true;
        bool   trackSession     = true;
        uint32 heartbeatMs      = 60000;
        uint32 minAbsence       = 60;
        uint32 maxDowntimeDays  = 60;
        uint32 maxFutureDays    = 30;
        uint32 graceSeconds     = 300;
        uint32 maxSpawnTimeSecs = 1800;
    };

    Settings g_cfg;

    // Formatiert eine Dauer lesbar, damit die Protokollzeilen ohne Kopfrechnen
    // verstaendlich sind.
    std::string HumanDuration(int64 seconds)
    {
        if (seconds < 0)
            return "negativ";

        int64 d = seconds / SECONDS_PER_DAY;
        int64 h = (seconds % SECONDS_PER_DAY) / 3600;
        int64 m = (seconds % 3600) / 60;
        int64 s = seconds % 60;

        std::string out;
        if (d) out += std::to_string(d) + "d ";
        if (d || h) out += std::to_string(h) + "h ";
        if (d || h || m) out += std::to_string(m) + "min ";
        out += std::to_string(s) + "s";
        return out;
    }

    std::string InstanceScope(std::string const& prefix)
    {
        return g_cfg.includeInstances ? "" : Acore::StringFormat(" AND {}instanceId = 0", prefix);
    }

    // Ergebnis einer Verschiebung, ausschliesslich fuer die Protokollzeile.
    struct ShiftResult
    {
        uint32 shifted   = 0;   // tatsaechlich verschobene Eintraege
        uint32 inMemory  = 0;   // davon auf bereits geladenen Karten
        uint32 skippedLong = 0; // wegen MaxSpawnTimeSecs ausgenommen
    };

    std::string SkippedNote(ShiftResult const& res)
    {
        if (!res.skippedLong)
            return std::string();

        return Acore::StringFormat(", {} Langlaeufer ausgenommen", res.skippedLong);
    }

    // Kern beider Pfade. Der Aufrufer liefert ein SELECT mit den Spalten
    // guid, instanceId, mapId, respawnTime.
    //
    // touchLoadedMaps  Beim Login sind Karten geladen. Ein reines UPDATE auf
    //                  creature_respawn bliebe dann wirkungslos, weil die Karte
    //                  ihre Zeiten nur beim Erzeugen liest. Map::SaveCreature-
    //                  RespawnTime pflegt Karte, Respawn-Queue und Datenbank in
    //                  einem Zug; zusaetzlich braucht der Legacy-Pfad das
    //                  Kreaturobjekt selbst, das seinen Timer in m_respawnTime
    //                  mitfuehrt (Creature::Update, DeathState::Dead).
    // updateSnapshot   Beim Start wandert der gemerkte Bestand mit, sonst
    //                  passt er beim naechsten Login nicht mehr zum Istzustand.
    ShiftResult ShiftRespawnTimes(std::string const& select, int64 shift, bool touchLoadedMaps, bool updateSnapshot)
    {
        ShiftResult res;

        QueryResult rows = CharacterDatabase.Query(select);
        if (!rows)
            return res;

        time_t const now = GameTime::GetGameTime().count();
        bool const apply = !g_cfg.dryRun;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        do
        {
            Field* f = rows->Fetch();
            uint32 const guid       = f[0].Get<uint32>();
            uint32 const instanceId = f[1].Get<uint32>();
            uint32 const mapId      = f[2].Get<uint32>();
            uint32 const oldTime    = f[3].Get<uint32>();

            // Langlaeufer laufen nach echter Zeit weiter. Ein seltener Spawn mit
            // 42 Stunden wuerde sonst aus Kalenderzeit Spielzeit machen und bei
            // wenigen Stunden Spiel am Tag ueber Wochen ausbleiben. Dasselbe gilt
            // fuer Event-NPCs, deren Ereignis ohnehin nach Kalender laeuft.
            if (g_cfg.maxSpawnTimeSecs)
            {
                CreatureData const* data = sObjectMgr->GetCreatureData(guid);
                if (data && data->spawntimesecs > g_cfg.maxSpawnTimeSecs)
                {
                    ++res.skippedLong;
                    continue;
                }
            }

            uint32 newTime = oldTime + static_cast<uint32>(shift);
            bool handledInMemory = false;

            if (apply && touchLoadedMaps)
            {
                if (Map* map = sMapMgr->FindMap(mapId, instanceId))
                {
                    time_t stored = static_cast<time_t>(newTime);
                    map->SaveCreatureRespawnTime(guid, stored);
                    newTime = static_cast<uint32>(stored);

                    if (stored > now)
                    {
                        uint32 const remaining = static_cast<uint32>(stored - now);
                        auto bounds = map->GetCreatureBySpawnIdStore().equal_range(guid);
                        for (auto itr = bounds.first; itr != bounds.second; ++itr)
                            if (Creature* creature = itr->second)
                                if (!creature->IsAlive())
                                    creature->SetRespawnTime(remaining);
                    }

                    handledInMemory = true;
                    ++res.inMemory;
                }
            }

            if (apply && !handledInMemory)
                trans->Append("UPDATE creature_respawn SET respawnTime = {} WHERE guid = {} AND instanceId = {}",
                              newTime, guid, instanceId);

            if (apply && updateSnapshot)
                trans->Append("UPDATE `{}` SET respawnTime = {} WHERE guid = {} AND instanceId = {}",
                              SNAPSHOT_TABLE, newTime, guid, instanceId);

            ++res.shifted;
        }
        while (rows->NextRow());

        if (apply)
            CharacterDatabase.CommitTransaction(trans);

        return res;
    }

    // Zaehlt die angemeldeten Menschen. Botsitzungen tragen kein Socket und
    // melden sich ueber WorldSession::IsBot(); sie zaehlen hier nicht mit.
    uint32 CountHumanPlayers(Player const* excluded)
    {
        uint32 count = 0;
        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
        {
            if (!session || session->IsBot())
                continue;

            Player* player = session->GetPlayer();
            if (!player || player == excluded)
                continue;

            ++count;
        }

        return count;
    }

    bool IsHuman(Player const* player)
    {
        return player && player->GetSession() && !player->GetSession()->IsBot();
    }
}

class OfflineRespawnFreezeWorldScript : public WorldScript
{
public:
    OfflineRespawnFreezeWorldScript() : WorldScript("OfflineRespawnFreezeWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
        WORLDHOOK_ON_UPDATE,
        WORLDHOOK_ON_AFTER_UNLOAD_ALL_MAPS
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }

    // Bewusst dieser Hook: Er laeuft, nachdem die Datenbankverbindungen stehen
    // und sObjectMgr die Spawndaten kennt, aber bevor die erste Karte erzeugt
    // wird. Karten lesen ihre Respawn-Zeiten beim Erzeugen
    // (Map::LoadRespawnTimes); spaeter zu schreiben haette keine Wirkung mehr
    // auf den bereits geladenen Zustand.
    void OnBeforeWorldInitialized() override
    {
        if (!_enabled)
            return;

        EnsureTables();
        ApplyDowntimeShift();
    }

    void OnUpdate(uint32 diff) override
    {
        if (!_enabled || g_cfg.heartbeatMs == 0)
            return;

        _sinceHeartbeat += diff;
        if (_sinceHeartbeat < g_cfg.heartbeatMs)
            return;

        _sinceHeartbeat = 0;
        // Asynchron: Der Weltticker darf hierfuer nicht warten.
        WriteState(static_cast<uint32>(GameTime::GetGameTime().count()), false, false);
    }

    // Letzter Hook vor dem Ende des Prozesses, der nach MapMgr::UnloadAll()
    // laeuft. Zu diesem Zeitpunkt hat der Core alle offenen Respawn-Zeiten
    // geschrieben, der Zeitstempel passt also exakt zum Datenbestand.
    void OnAfterUnloadAllMaps() override
    {
        if (!_enabled)
            return;

        uint32 now = static_cast<uint32>(GameTime::GetGameTime().count());
        WriteState(now, true, true);
        LOG_INFO("module", "[RespawnFreeze] Sauberes Herunterfahren vermerkt.");
    }

private:
    bool   _enabled        = true;
    uint32 _sinceHeartbeat = 0;

    void LoadConfig()
    {
        g_cfg.enabled          = sConfigMgr->GetOption<bool>  ("OfflineRespawnFreeze.Enable", true);
        g_cfg.dryRun           = sConfigMgr->GetOption<bool>  ("OfflineRespawnFreeze.DryRun", false);
        g_cfg.includeInstances = sConfigMgr->GetOption<bool>  ("OfflineRespawnFreeze.IncludeInstances", true);
        g_cfg.trackSession     = sConfigMgr->GetOption<bool>  ("OfflineRespawnFreeze.FreezeWhileNoPlayerOnline", true);
        g_cfg.minAbsence       = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.MinDowntimeSeconds", 60);
        g_cfg.maxDowntimeDays  = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.MaxDowntimeDays", 60);
        g_cfg.maxFutureDays    = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.MaxFutureDays", 30);
        g_cfg.graceSeconds     = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.StartupGraceSeconds", 300);
        g_cfg.maxSpawnTimeSecs = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.MaxSpawnTimeSecs", 1800);

        uint32 heartbeat  = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.HeartbeatSeconds", 60);
        g_cfg.heartbeatMs = heartbeat * 1000;

        if (g_cfg.maxFutureDays == 0)
        {
            LOG_WARN("module", "[RespawnFreeze] MaxFutureDays ist 0; das wuerde jede Verschiebung verhindern. Verwende 30.");
            g_cfg.maxFutureDays = 30;
        }

        _enabled = g_cfg.enabled;
    }

    void EnsureTables()
    {
        // Bewusst im Code statt als SQL-Datei: Es handelt sich um zwei Tabellen,
        // die vorhanden sein muessen, bevor irgendetwas anderes laeuft.
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `{}` ("
            "`id` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
            "`last_seen` INT UNSIGNED NOT NULL,"
            "`clean_shutdown` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`last_shift` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`last_shift_rows` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`absence_start` INT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (`id`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci "
            "COMMENT='mod-offline-respawn-freeze: letzter bekannter Serverzeitpunkt'",
            STATE_TABLE);

        // MySQL 8 kennt kein ADD COLUMN IF NOT EXISTS; fuer bestehende
        // Installationen wird die Spalte daher gezielt nachgezogen.
        QueryResult hasColumn = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM information_schema.COLUMNS "
            "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}' AND COLUMN_NAME = 'absence_start'",
            STATE_TABLE);

        if (hasColumn && (*hasColumn)[0].Get<uint64>() == 0)
        {
            CharacterDatabase.DirectExecute("ALTER TABLE `{}` ADD COLUMN `absence_start` INT UNSIGNED NOT NULL DEFAULT 0", STATE_TABLE);
            LOG_INFO("module", "[RespawnFreeze] Zustandstabelle um absence_start erweitert.");
        }

        // Bestand beim Logout des letzten Menschen. Die Spalten spiegeln
        // creature_respawn, damit der Vergleich beim Login exakt bleibt.
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `{}` ("
            "`guid` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`respawnTime` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`mapId` SMALLINT UNSIGNED NOT NULL DEFAULT 0,"
            "`instanceId` INT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (`guid`, `instanceId`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci "
            "COMMENT='mod-offline-respawn-freeze: Bestand beim Logout des letzten Spielers'",
            SNAPSHOT_TABLE);
    }

    void WriteState(uint32 lastSeen, bool cleanShutdown, bool synchronous)
    {
        std::string sql = Acore::StringFormat(
            "INSERT INTO `{}` (id, last_seen, clean_shutdown) VALUES (1, {}, {}) "
            "ON DUPLICATE KEY UPDATE last_seen = VALUES(last_seen), clean_shutdown = VALUES(clean_shutdown)",
            STATE_TABLE, lastSeen, cleanShutdown ? 1 : 0);

        if (synchronous)
            CharacterDatabase.DirectExecute(sql);
        else
            CharacterDatabase.Execute(sql);
    }

    void ApplyDowntimeShift()
    {
        uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());

        QueryResult state = CharacterDatabase.Query(
            "SELECT last_seen, clean_shutdown, absence_start FROM `{}` WHERE id = 1", STATE_TABLE);

        if (!state)
        {
            LOG_INFO("module", "[RespawnFreeze] Erster Lauf, kein frueherer Zeitstempel. Es wird nichts verschoben.");
            WriteState(now, false, true);
            return;
        }

        Field* fields         = state->Fetch();
        uint32 const lastSeen = fields[0].Get<uint32>();
        bool   const wasClean = fields[1].Get<uint8>() != 0;
        uint32 absenceStart   = fields[2].Get<uint32>();

        // Wurde der Sitzungsanker abgeschaltet, waehrend einer stand, bliebe er
        // sonst samt Schnappschuss dauerhaft liegen.
        if (!g_cfg.trackSession && absenceStart > 0 && !g_cfg.dryRun)
        {
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            trans->Append("DELETE FROM `{}`", SNAPSHOT_TABLE);
            trans->Append("UPDATE `{}` SET absence_start = 0 WHERE id = 1", STATE_TABLE);
            CharacterDatabase.CommitTransaction(trans);
            absenceStart = 0;
            LOG_INFO("module", "[RespawnFreeze] Sitzungsanker ist abgeschaltet; alter Anker verworfen.");
        }

        // Ein gesetzter Anker bedeutet: Der letzte Mensch war beim Herunterfahren
        // bereits abgemeldet. Dann uebernimmt der Login-Pfad die Spanne zwischen
        // Serverstart und Login, und die pauschale Karenz entfaellt.
        bool const anchored = g_cfg.trackSession && absenceStart > 0;

        // Gegen eine zurueckgestellte Systemuhr: signed rechnen, sonst laeuft
        // die vorzeichenlose Subtraktion in einen Ueberlauf.
        int64 const downtime = static_cast<int64>(now) - static_cast<int64>(lastSeen);

        if (downtime < 0)
        {
            LOG_WARN("module", "[RespawnFreeze] Letzter Zeitstempel liegt in der Zukunft (Systemuhr?). Es wird nichts verschoben.");
            WriteState(now, false, true);
            return;
        }

        if (!wasClean)
            LOG_WARN("module", "[RespawnFreeze] Kein sauberes Herunterfahren erkannt. Es gilt das letzte Lebenszeichen; die Abweichung betraegt hoechstens {} Sekunden.",
                     g_cfg.heartbeatMs / 1000);

        if (downtime < static_cast<int64>(g_cfg.minAbsence))
        {
            LOG_INFO("module", "[RespawnFreeze] Ausfallzeit {} liegt unter der Schwelle. Es wird nichts verschoben.",
                     HumanDuration(downtime));
            FinishStartup(now, absenceStart, 0, 0);
            return;
        }

        int64 const maxDowntime = static_cast<int64>(g_cfg.maxDowntimeDays) * SECONDS_PER_DAY;
        if (downtime > maxDowntime)
        {
            LOG_WARN("module", "[RespawnFreeze] Ausfallzeit {} uebersteigt die Obergrenze von {} Tagen. Es wird nichts verschoben.",
                     HumanDuration(downtime), g_cfg.maxDowntimeDays);
            FinishStartup(now, absenceStart, 0, 0);
            return;
        }

        // Zwischen dem Start des Servers und dem Augenblick, in dem tatsaechlich
        // jemand in der Welt steht, vergeht Zeit: Client starten, anmelden,
        // Charakter waehlen, Gebiet laden. Diese Spanne liefe ungebremst gegen
        // die Respawns, und die Bots toeten waehrenddessen bereits. Ohne Anker
        // gleicht die Karenz das pauschal aus; mit Anker rechnet der Login-Pfad
        // sie genau ab. Sie wird erst hier aufgeschlagen, damit sie die
        // Plausibilitaetsschwellen oben nicht verfaelscht.
        int64 const shift = downtime + (anchored ? 0 : static_cast<int64>(g_cfg.graceSeconds));

        //   respawnTime > lastSeen   laesst bereits faellige Eintraege in Ruhe,
        //                            die sonst nachtraeglich in die Zukunft
        //                            wandern und damit spaeter erschienen.
        //   respawnTime < obergrenze schuetzt die Markierung "now + YEAR", mit
        //                            der Map::SaveCreatureRespawnTime Kreaturen
        //                            kennzeichnet, die vor dem Instanz-Reset
        //                            nicht wiederkehren.
        uint32 const upperBound = lastSeen + g_cfg.maxFutureDays * SECONDS_PER_DAY;

        std::string const select = Acore::StringFormat(
            "SELECT guid, instanceId, mapId, respawnTime FROM creature_respawn "
            "WHERE respawnTime > {} AND respawnTime < {}{}",
            lastSeen, upperBound, InstanceScope(""));

        // Beim Start ist noch keine Karte erzeugt, daher rein ueber die
        // Datenbank. Der gemerkte Bestand wandert mit, damit der Login-Pfad ihn
        // spaeter noch wiedererkennt.
        ShiftResult const res = ShiftRespawnTimes(select, shift, false, anchored);

        std::string const grace = anchored
            ? std::string()
            : Acore::StringFormat(", zuzueglich {} Karenz", HumanDuration(g_cfg.graceSeconds));

        LOG_INFO("module", "[RespawnFreeze] Ausfallzeit {}{}, ergibt {} Verschiebung. {} Eintraege verschoben{}{}.",
                 HumanDuration(downtime), grace, HumanDuration(shift), res.shifted,
                 SkippedNote(res), g_cfg.includeInstances ? "" : ", offene Welt");

        if (g_cfg.dryRun)
        {
            LOG_INFO("module", "[RespawnFreeze] Probelauf aktiv: es wird nichts geschrieben. Zum Scharfschalten OfflineRespawnFreeze.DryRun = 0 setzen.");
            return;
        }

        if (anchored)
            LOG_INFO("module", "[RespawnFreeze] Anker vom letzten Logout steht. Die Spanne bis zum Login wird beim Anmelden verrechnet.");

        FinishStartup(now, absenceStart, shift, res.shifted);
    }

    // Schreibt den Zustand fort. Der Anker wandert um dieselbe Spanne mit, sonst
    // wuerde die Ausfallzeit beim Login ein zweites Mal gezaehlt.
    void FinishStartup(uint32 now, uint32 absenceStart, int64 shift, uint32 rows)
    {
        if (g_cfg.dryRun)
            return;

        uint32 const movedAnchor = absenceStart ? static_cast<uint32>(absenceStart + shift) : 0;

        // Ohne verschobene Eintraege bleiben die Diagnosefelder des letzten
        // wirksamen Laufs stehen; nur Zeitstempel und Anker werden fortgeschrieben.
        if (!rows)
        {
            CharacterDatabase.DirectExecute(
                "INSERT INTO `{}` (id, last_seen, clean_shutdown, absence_start) VALUES (1, {}, 0, {}) "
                "ON DUPLICATE KEY UPDATE last_seen = VALUES(last_seen), clean_shutdown = 0, "
                "absence_start = VALUES(absence_start)",
                STATE_TABLE, now, movedAnchor);
            return;
        }

        CharacterDatabase.DirectExecute(
            "INSERT INTO `{}` (id, last_seen, clean_shutdown, last_shift, last_shift_rows, absence_start) "
            "VALUES (1, {}, 0, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE last_seen = VALUES(last_seen), clean_shutdown = 0, "
            "last_shift = VALUES(last_shift), last_shift_rows = VALUES(last_shift_rows), "
            "absence_start = VALUES(absence_start)",
            STATE_TABLE, now, shift, rows, movedAnchor);
    }
};

class OfflineRespawnFreezePlayerScript : public PlayerScript
{
public:
    OfflineRespawnFreezePlayerScript() : PlayerScript("OfflineRespawnFreezePlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LOGOUT
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!g_cfg.enabled || !g_cfg.trackSession || !IsHuman(player))
            return;

        // Nur der erste Mensch taut die Welt wieder auf.
        if (CountHumanPlayers(player) > 0)
            return;

        Release();
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!g_cfg.enabled || !g_cfg.trackSession || !IsHuman(player))
            return;

        // Der Hook laeuft, solange die eigene Sitzung noch in der Liste steht.
        if (CountHumanPlayers(player) > 0)
            return;

        Freeze();
    }

private:
    // Merkt sich den Bestand. creature_respawn ist ein laufendes Abbild des
    // Speicherzustands, weil Map::SaveCreatureRespawnTime jede Aenderung sofort
    // schreibt; der Schnappschuss ist damit exakt.
    void Freeze()
    {
        uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());

        if (g_cfg.dryRun)
        {
            LOG_INFO("module", "[RespawnFreeze] Probelauf: Logout des letzten Spielers, es wird nichts vermerkt.");
            return;
        }

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append("DELETE FROM `{}`", SNAPSHOT_TABLE);
        trans->Append("INSERT INTO `{}` (guid, respawnTime, mapId, instanceId) "
                      "SELECT guid, respawnTime, mapId, instanceId FROM creature_respawn WHERE respawnTime > {}{}",
                      SNAPSHOT_TABLE, now, InstanceScope(""));
        trans->Append("UPDATE `{}` SET absence_start = {} WHERE id = 1", STATE_TABLE, now);
        CharacterDatabase.CommitTransaction(trans);

        LOG_INFO("module", "[RespawnFreeze] Letzter Spieler abgemeldet. Bestand vermerkt; offene Respawns ruhen bis zum naechsten Login.");
    }

    // Verschiebt die vermerkten Eintraege um die Abwesenheit. Was die Bots
    // zwischenzeitlich erlegt haben, steht nicht im Schnappschuss oder traegt
    // eine andere respawnTime und bleibt damit unberuehrt.
    void Release()
    {
        uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());

        QueryResult state = CharacterDatabase.Query(
            "SELECT absence_start FROM `{}` WHERE id = 1", STATE_TABLE);

        if (!state)
            return;

        uint32 const absenceStart = (*state)[0].Get<uint32>();
        if (absenceStart == 0)
            return;

        int64 const absence = static_cast<int64>(now) - static_cast<int64>(absenceStart);

        if (absence < 0)
        {
            LOG_WARN("module", "[RespawnFreeze] Anker liegt in der Zukunft (Systemuhr?). Es wird nichts verschoben.");
            Clear();
            return;
        }

        if (absence < static_cast<int64>(g_cfg.minAbsence))
        {
            LOG_DEBUG("module", "[RespawnFreeze] Abwesenheit {} liegt unter der Schwelle. Es wird nichts verschoben.",
                      HumanDuration(absence));
            Clear();
            return;
        }

        int64 const maxAbsence = static_cast<int64>(g_cfg.maxDowntimeDays) * SECONDS_PER_DAY;
        if (absence > maxAbsence)
        {
            LOG_WARN("module", "[RespawnFreeze] Abwesenheit {} uebersteigt die Obergrenze von {} Tagen. Es wird nichts verschoben.",
                     HumanDuration(absence), g_cfg.maxDowntimeDays);
            Clear();
            return;
        }

        uint32 const upperBound = absenceStart + g_cfg.maxFutureDays * SECONDS_PER_DAY;

        // Der Vergleich auf gleiche respawnTime ist der eigentliche Filter: Wurde
        // derselbe Spawn waehrend der Abwesenheit erneut erlegt, steht dort ein
        // anderer Zeitpunkt, und der Eintrag gehoert den Bots.
        std::string const select = Acore::StringFormat(
            "SELECT r.guid, r.instanceId, r.mapId, r.respawnTime FROM creature_respawn r "
            "JOIN `{}` s ON s.guid = r.guid AND s.instanceId = r.instanceId AND s.respawnTime = r.respawnTime "
            "WHERE r.respawnTime > {} AND r.respawnTime < {}{}",
            SNAPSHOT_TABLE, absenceStart, upperBound, InstanceScope("r."));

        ShiftResult const res = ShiftRespawnTimes(select, absence, true, false);

        LOG_INFO("module", "[RespawnFreeze] Abwesenheit {}. {} Respawn-Zeiten verschoben, davon {} auf geladenen Karten{}.",
                 HumanDuration(absence), res.shifted, res.inMemory, SkippedNote(res));

        if (g_cfg.dryRun)
        {
            LOG_INFO("module", "[RespawnFreeze] Probelauf aktiv: es wird nichts geschrieben.");
            return;
        }

        Clear(absence, res.shifted);
    }

    void Clear(int64 shift = 0, uint32 rows = 0)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append("DELETE FROM `{}`", SNAPSHOT_TABLE);

        if (rows)
            trans->Append("UPDATE `{}` SET absence_start = 0, last_shift = {}, last_shift_rows = {} WHERE id = 1",
                          STATE_TABLE, shift, rows);
        else
            trans->Append("UPDATE `{}` SET absence_start = 0 WHERE id = 1", STATE_TABLE);

        CharacterDatabase.CommitTransaction(trans);
    }
};

void AddOfflineRespawnFreezeScripts()
{
    new OfflineRespawnFreezeWorldScript();
    new OfflineRespawnFreezePlayerScript();
}
