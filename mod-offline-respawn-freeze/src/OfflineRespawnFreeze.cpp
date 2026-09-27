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
 *      Logout werden die offenen Respawns geparkt und ihre Restzeit vermerkt,
 *      beim naechsten Login erhalten sie genau diese Restzeit zurueck. Was die
 *      Bots waehrenddessen erlegen, bleibt unberuehrt und respawnt normal.
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

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace
{
    constexpr char const* STATE_TABLE    = "mod_offline_respawn_freeze";
    constexpr char const* SNAPSHOT_TABLE = "mod_offline_respawn_freeze_snapshot";
    constexpr uint32 SECONDS_PER_DAY     = 86400;

    // Parkzeit waehrend der Abwesenheit. Bewusst gleich der Jahresmarkierung,
    // auf die Map::SaveCreatureRespawnTime in Instanzen mit Reset-Periode ohnehin
    // kuerzt; so ergibt sich in beiden Faellen derselbe Wert.
    constexpr uint32 PARK_SECONDS        = 365 * SECONDS_PER_DAY;

    // Schnappschuesse werden in Bloecken geschrieben, nicht Zeile fuer Zeile.
    constexpr std::size_t INSERT_CHUNK   = 500;

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

    // Schlachtfelder und Arenen werden nach dem Spiel ohnehin verworfen.
    bool InScope(Map const* map)
    {
        if (map->IsBattlegroundOrArena())
            return false;

        return g_cfg.includeInstances || map->GetInstanceId() == 0;
    }

    // Langlaeufer laufen nach echter Zeit weiter. Ein seltener Spawn mit 42
    // Stunden wuerde sonst aus Kalenderzeit Spielzeit machen und bei wenigen
    // Stunden Spiel am Tag ueber Wochen ausbleiben. Dasselbe gilt fuer
    // Event-NPCs, deren Ereignis ohnehin nach Kalender laeuft.
    bool IsLongRunner(uint32 guid)
    {
        if (!g_cfg.maxSpawnTimeSecs)
            return false;

        CreatureData const* data = sObjectMgr->GetCreatureData(guid);
        return data && data->spawntimesecs > g_cfg.maxSpawnTimeSecs;
    }

    // Ergebnis eines Durchgangs, ausschliesslich fuer die Protokollzeile.
    struct ShiftResult
    {
        uint32 shifted     = 0; // tatsaechlich bearbeitete Eintraege
        uint32 inMemory    = 0; // davon auf bereits geladenen Karten
        uint32 skippedLong = 0; // wegen MaxSpawnTimeSecs ausgenommen
        uint32 changed     = 0; // waehrend der Abwesenheit von aussen veraendert
    };

    std::string SkippedNote(ShiftResult const& res)
    {
        std::string out;
        if (res.skippedLong)
            out += Acore::StringFormat(", {} Langlaeufer ausgenommen", res.skippedLong);
        if (res.changed)
            out += Acore::StringFormat(", {} zwischenzeitlich veraendert", res.changed);
        return out;
    }

    // Startpfad. Beim Start ist noch keine Karte erzeugt, daher rein ueber die
    // Datenbank. Der Aufrufer liefert ein SELECT mit den Spalten guid,
    // instanceId, mapId, respawnTime.
    ShiftResult ShiftRespawnTimes(std::string const& select, int64 shift)
    {
        ShiftResult res;

        QueryResult rows = CharacterDatabase.Query(select);
        if (!rows)
            return res;

        bool const apply = !g_cfg.dryRun;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        do
        {
            Field* f = rows->Fetch();
            uint32 const guid       = f[0].Get<uint32>();
            uint32 const instanceId = f[1].Get<uint32>();
            uint32 const oldTime    = f[3].Get<uint32>();

            if (IsLongRunner(guid))
            {
                ++res.skippedLong;
                continue;
            }

            if (apply)
                trans->Append("UPDATE creature_respawn SET respawnTime = {} WHERE guid = {} AND instanceId = {}",
                              oldTime + static_cast<uint32>(shift), guid, instanceId);

            ++res.shifted;
        }
        while (rows->NextRow());

        if (apply)
            CharacterDatabase.CommitTransaction(trans);

        return res;
    }

    // Setzt geparkte Eintraege auf "jetzt plus Restzeit", ohne geladene Karten.
    // Nur fuer den Start, wenn der Sitzungsanker abgeschaltet wurde.
    void RestoreParkedInDb(uint32 now)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append(
            "UPDATE creature_respawn r JOIN `{}` s ON s.guid = r.guid AND s.instanceId = r.instanceId "
            "SET r.respawnTime = {} + s.remaining WHERE r.respawnTime = s.parkedTime",
            SNAPSHOT_TABLE, now);
        trans->Append("DELETE FROM `{}`", SNAPSHOT_TABLE);
        trans->Append("UPDATE `{}` SET absence_start = 0 WHERE id = 1", STATE_TABLE);
        CharacterDatabase.CommitTransaction(trans);
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

    // MySQL 8 kennt kein ADD COLUMN IF NOT EXISTS; fuer bestehende
    // Installationen werden Spalten daher gezielt nachgezogen.
    void EnsureColumn(char const* table, char const* column, char const* definition)
    {
        QueryResult hasColumn = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM information_schema.COLUMNS "
            "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}' AND COLUMN_NAME = '{}'",
            table, column);

        if (hasColumn && (*hasColumn)[0].Get<uint64>() == 0)
        {
            CharacterDatabase.DirectExecute("ALTER TABLE `{}` ADD COLUMN `{}` {}", table, column, definition);
            LOG_INFO("module", "[RespawnFreeze] Tabelle {} um {} erweitert.", table, column);
        }
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

        EnsureColumn(STATE_TABLE, "absence_start", "INT UNSIGNED NOT NULL DEFAULT 0");

        // Geparkter Bestand zwischen dem Logout des letzten Menschen und dem
        // naechsten Login. respawnTime ist der urspruengliche Zeitpunkt,
        // remaining die Restzeit beim Logout, parkedTime der Parkwert, an dem
        // der Login einen unveraenderten Eintrag wiedererkennt.
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `{}` ("
            "`guid` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`respawnTime` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`mapId` SMALLINT UNSIGNED NOT NULL DEFAULT 0,"
            "`instanceId` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`remaining` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`parkedTime` INT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (`guid`, `instanceId`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci "
            "COMMENT='mod-offline-respawn-freeze: geparkter Bestand beim Logout des letzten Spielers'",
            SNAPSHOT_TABLE);

        EnsureColumn(SNAPSHOT_TABLE, "remaining", "INT UNSIGNED NOT NULL DEFAULT 0");
        EnsureColumn(SNAPSHOT_TABLE, "parkedTime", "INT UNSIGNED NOT NULL DEFAULT 0");
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

        if (absenceStart > 0 && !g_cfg.dryRun)
        {
            // Vermerk aus der Zeit vor dem Parken: ohne Restzeiten ist er nicht
            // mehr auswertbar. Die Eintraege selbst wurden damals nicht
            // veraendert und laufen einfach weiter.
            QueryResult legacy = CharacterDatabase.Query(
                "SELECT COUNT(*) FROM `{}` WHERE parkedTime = 0", SNAPSHOT_TABLE);

            if (legacy && (*legacy)[0].Get<uint64>() > 0)
            {
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                trans->Append("DELETE FROM `{}`", SNAPSHOT_TABLE);
                trans->Append("UPDATE `{}` SET absence_start = 0 WHERE id = 1", STATE_TABLE);
                CharacterDatabase.CommitTransaction(trans);
                absenceStart = 0;
                LOG_INFO("module", "[RespawnFreeze] Alter Vermerk ohne Parkzeiten verworfen.");
            }
            // Wurde der Sitzungsanker abgeschaltet, waehrend einer stand, blieben
            // die geparkten Mobs sonst ein Jahr lang aus. Sie erhalten ihre
            // Restzeit ab jetzt zurueck.
            else if (!g_cfg.trackSession)
            {
                RestoreParkedInDb(now);
                absenceStart = 0;
                LOG_INFO("module", "[RespawnFreeze] Sitzungsanker ist abgeschaltet; geparkte Respawns laufen ab jetzt mit ihrer Restzeit weiter.");
            }
        }

        // Ein gesetzter Anker bedeutet: Der letzte Mensch war beim Herunterfahren
        // bereits abgemeldet, sein Bestand ist geparkt. Die pauschale Karenz
        // entfaellt dann, weil der Login jedem geparkten Eintrag seine exakte
        // Restzeit zurueckgibt.
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
        // gleicht die Karenz das pauschal aus; mit Anker ist der Bestand des
        // Spielers geparkt und braucht sie nicht. Sie wird erst hier
        // aufgeschlagen, damit sie die Plausibilitaetsschwellen oben nicht
        // verfaelscht.
        int64 const shift = downtime + (anchored ? 0 : static_cast<int64>(g_cfg.graceSeconds));

        //   respawnTime > lastSeen   laesst bereits faellige Eintraege in Ruhe,
        //                            die sonst nachtraeglich in die Zukunft
        //                            wandern und damit spaeter erschienen.
        //   respawnTime < obergrenze schuetzt die Markierung "now + YEAR", mit
        //                            der Map::SaveCreatureRespawnTime Kreaturen
        //                            kennzeichnet, die vor dem Instanz-Reset
        //                            nicht wiederkehren, und die geparkten
        //                            Eintraege.
        //   NOT EXISTS               haelt geparkte Eintraege auch dann heraus,
        //                            wenn der Server laenger ohne Spieler lief.
        uint32 const upperBound = lastSeen + g_cfg.maxFutureDays * SECONDS_PER_DAY;

        std::string const select = Acore::StringFormat(
            "SELECT r.guid, r.instanceId, r.mapId, r.respawnTime FROM creature_respawn r "
            "WHERE r.respawnTime > {} AND r.respawnTime < {}{} AND NOT EXISTS ("
            "SELECT 1 FROM `{}` s WHERE s.guid = r.guid AND s.instanceId = r.instanceId AND s.parkedTime = r.respawnTime)",
            lastSeen, upperBound, InstanceScope("r."), SNAPSHOT_TABLE);

        ShiftResult const res = ShiftRespawnTimes(select, shift);

        std::string const grace = anchored
            ? std::string()
            : Acore::StringFormat(", zuzueglich {} Karenz", HumanDuration(g_cfg.graceSeconds));

        LOG_INFO("module", "[RespawnFreeze] Ausfallzeit {}{}, ergibt {} Verschiebung. {} Eintraege {}{}{}.",
                 HumanDuration(downtime), grace, HumanDuration(shift), res.shifted,
                 g_cfg.dryRun ? "waeren betroffen" : "verschoben",
                 SkippedNote(res), g_cfg.includeInstances ? "" : ", offene Welt");

        if (g_cfg.dryRun)
        {
            LOG_INFO("module", "[RespawnFreeze] Probelauf aktiv: es wird nichts geschrieben. Zum Scharfschalten OfflineRespawnFreeze.DryRun = 0 setzen.");
            return;
        }

        if (anchored)
            LOG_INFO("module", "[RespawnFreeze] Bestand vom letzten Logout ist geparkt und erhaelt beim Login seine Restzeit zurueck.");

        FinishStartup(now, absenceStart, shift, res.shifted);
    }

    // Schreibt den Zustand fort. Der Anker wandert um dieselbe Spanne mit, damit
    // die beim Login gemeldete Abwesenheit nur die Zeit mit laufendem Server
    // enthaelt und MaxDowntimeDays nicht doppelt zaehlt.
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
    struct ParkedEntry
    {
        uint32 guid;
        uint32 instanceId;
        uint32 mapId;
        uint32 respawnTime;
        uint32 remaining;
        uint32 parkedTime;
    };

    // Parkt alle offenen Respawns weit in der Zukunft und vermerkt ihre
    // Restzeit.
    //
    // Ein blosses Vermerken reicht nicht: Die Respawn-Queue der Karte
    // (Map::ProcessRespawns) laesst faellige Kreaturen erscheinen, sobald ihr
    // Grid geladen ist, und loescht dabei den Eintrag. Grids bleiben nach dem
    // Logout noch Minuten geladen, in der Naehe von Bots dauerhaft. Beim Login
    // waere dann nichts mehr zu verschieben.
    //
    // Quellen sind fuer geladene Karten deren Speicherabbild, fuer alle
    // anderen die Datenbank. Kreaturen, die gerade als Leiche liegen, haben
    // noch keinen Eintrag; sie werden ueber das Kreaturobjekt erfasst.
    void Freeze()
    {
        uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());

        if (g_cfg.dryRun)
        {
            LOG_INFO("module", "[RespawnFreeze] Probelauf: Logout des letzten Spielers, es wird nichts geparkt.");
            return;
        }

        uint32 const upperBound = now + g_cfg.maxFutureDays * SECONDS_PER_DAY;
        uint32 const parkUntil  = now + PARK_SECONDS;

        // Erst sammeln, dann schreiben: DoForAllMaps haelt die Sperre des
        // MapMgr, und SaveCreatureRespawnTime veraendert die Tabelle, ueber
        // die gerade gelaufen wird.
        std::vector<Map*> maps;
        sMapMgr->DoForAllMaps([&maps](Map* map)
        {
            if (InScope(map))
                maps.push_back(map);
        });

        ShiftResult res;
        std::vector<ParkedEntry> parked;
        std::set<std::pair<uint32, uint32>> loaded;

        auto isOpen = [&](uint32 guid, time_t respawnTime)
        {
            if (respawnTime <= static_cast<time_t>(now) || respawnTime >= static_cast<time_t>(upperBound))
                return false;

            if (IsLongRunner(guid))
            {
                ++res.skippedLong;
                return false;
            }

            return true;
        };

        for (Map* map : maps)
        {
            loaded.emplace(map->GetId(), map->GetInstanceId());

            std::map<uint32, time_t> open;
            for (auto const& [guid, respawnTime] : map->GetCreatureRespawnTimes())
                if (isOpen(guid, respawnTime))
                    open.emplace(guid, respawnTime);

            for (auto const& [guid, creature] : map->GetCreatureBySpawnIdStore())
            {
                if (!creature || creature->IsAlive() || open.count(guid))
                    continue;

                CreatureData const* data = sObjectMgr->GetCreatureData(guid);
                if (!data || !data->dbData)
                    continue;

                if (isOpen(guid, creature->GetRespawnTime()))
                    open.emplace(guid, creature->GetRespawnTime());
            }

            for (auto const& [guid, respawnTime] : open)
            {
                time_t stored = parkUntil;
                map->SaveCreatureRespawnTime(guid, stored);

                auto bounds = map->GetCreatureBySpawnIdStore().equal_range(guid);
                for (auto itr = bounds.first; itr != bounds.second; ++itr)
                    if (Creature* creature = itr->second)
                        if (!creature->IsAlive())
                            creature->SetRespawnTime(static_cast<uint32>(stored - now));

                parked.push_back({ guid, map->GetInstanceId(), map->GetId(), static_cast<uint32>(respawnTime),
                                   static_cast<uint32>(respawnTime - now), static_cast<uint32>(stored) });
                ++res.inMemory;
            }
        }

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        // Karten, die gerade nicht geladen sind, lesen ihre Zeiten beim
        // naechsten Erzeugen aus der Datenbank; dort genuegt das UPDATE.
        if (QueryResult rows = CharacterDatabase.Query(
                "SELECT guid, instanceId, mapId, respawnTime FROM creature_respawn "
                "WHERE respawnTime > {} AND respawnTime < {}{}",
                now, upperBound, InstanceScope("")))
        {
            do
            {
                Field* f = rows->Fetch();
                uint32 const guid        = f[0].Get<uint32>();
                uint32 const instanceId  = f[1].Get<uint32>();
                uint32 const mapId       = f[2].Get<uint32>();
                uint32 const respawnTime = f[3].Get<uint32>();

                if (loaded.count({ mapId, instanceId }) || !isOpen(guid, respawnTime))
                    continue;

                trans->Append("UPDATE creature_respawn SET respawnTime = {} WHERE guid = {} AND instanceId = {} AND respawnTime = {}",
                              parkUntil, guid, instanceId, respawnTime);

                parked.push_back({ guid, instanceId, mapId, respawnTime, respawnTime - now, parkUntil });
            }
            while (rows->NextRow());
        }

        // Kein DELETE vorab: Steht noch ein Vermerk ohne Login dazwischen, sind
        // dessen Eintraege bereits geparkt und kaemen hier nicht mehr vor.
        // Sie muessen im Vermerk bleiben, sonst erhielten sie nie ihre
        // Restzeit zurueck.
        for (std::size_t start = 0; start < parked.size(); start += INSERT_CHUNK)
        {
            std::string values;
            std::size_t const end = std::min(parked.size(), start + INSERT_CHUNK);
            for (std::size_t i = start; i < end; ++i)
            {
                ParkedEntry const& e = parked[i];
                if (!values.empty())
                    values += ',';
                values += Acore::StringFormat("({},{},{},{},{},{})",
                                              e.guid, e.respawnTime, e.mapId, e.instanceId, e.remaining, e.parkedTime);
            }

            trans->Append("REPLACE INTO `{}` (guid, respawnTime, mapId, instanceId, remaining, parkedTime) VALUES {}",
                          SNAPSHOT_TABLE, values);
        }

        trans->Append("UPDATE `{}` SET absence_start = IF(absence_start = 0, {}, absence_start) WHERE id = 1", STATE_TABLE, now);
        CharacterDatabase.CommitTransaction(trans);

        res.shifted = static_cast<uint32>(parked.size());
        LOG_INFO("module", "[RespawnFreeze] Letzter Spieler abgemeldet. {} Respawn-Zeiten geparkt, davon {} auf geladenen Karten{}.",
                 res.shifted, res.inMemory, SkippedNote(res));
    }

    // Gibt jedem geparkten Eintrag seine Restzeit zurueck, gerechnet ab jetzt.
    // Wer den Parkwert nicht mehr traegt, wurde zwischenzeitlich von aussen
    // veraendert (etwa per GM-Befehl) und bleibt unberuehrt.
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

        // Nach sehr langer Abwesenheit erwartet niemand mehr einen bestimmten
        // Mob; dann erscheint alles Geparkte sofort, als waere die Zeit
        // normal gelaufen.
        int64 const maxAbsence = static_cast<int64>(g_cfg.maxDowntimeDays) * SECONDS_PER_DAY;
        bool const tooLong = absence > maxAbsence;

        if (tooLong)
            LOG_WARN("module", "[RespawnFreeze] Abwesenheit {} uebersteigt die Obergrenze von {} Tagen. Geparkte Respawns erscheinen sofort.",
                     HumanDuration(absence), g_cfg.maxDowntimeDays);

        ShiftResult res;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        if (QueryResult rows = CharacterDatabase.Query(
                "SELECT guid, instanceId, mapId, remaining, parkedTime FROM `{}`", SNAPSHOT_TABLE))
        {
            do
            {
                Field* f = rows->Fetch();
                uint32 const guid       = f[0].Get<uint32>();
                uint32 const instanceId = f[1].Get<uint32>();
                uint32 const mapId      = f[2].Get<uint32>();
                uint32 const remaining  = f[3].Get<uint32>();
                uint32 const parkedTime = f[4].Get<uint32>();

                uint32 const target = tooLong ? now : now + remaining;

                if (Map* map = sMapMgr->FindMap(mapId, instanceId))
                {
                    if (map->GetCreatureRespawnTime(guid) != static_cast<time_t>(parkedTime))
                    {
                        ++res.changed;
                        continue;
                    }

                    time_t stored = target;
                    map->SaveCreatureRespawnTime(guid, stored);

                    // Tote Kreaturobjekte, auch Leichen, tragen den Parkwert
                    // selbst. Kreaturen, die Bots seither erlegt haben, tragen
                    // einen kleineren Wert und bleiben unberuehrt.
                    auto bounds = map->GetCreatureBySpawnIdStore().equal_range(guid);
                    for (auto itr = bounds.first; itr != bounds.second; ++itr)
                        if (Creature* creature = itr->second)
                            if (!creature->IsAlive() && creature->GetRespawnTime() >= static_cast<time_t>(parkedTime))
                                creature->SetRespawnTime(stored > now ? static_cast<uint32>(stored - now) : 0);

                    ++res.inMemory;
                }
                else
                {
                    trans->Append("UPDATE creature_respawn SET respawnTime = {} WHERE guid = {} AND instanceId = {} AND respawnTime = {}",
                                  target, guid, instanceId, parkedTime);
                }

                ++res.shifted;
            }
            while (rows->NextRow());
        }

        trans->Append("DELETE FROM `{}`", SNAPSHOT_TABLE);
        if (res.shifted)
            trans->Append("UPDATE `{}` SET absence_start = 0, last_shift = {}, last_shift_rows = {} WHERE id = 1",
                          STATE_TABLE, std::max<int64>(absence, 0), res.shifted);
        else
            trans->Append("UPDATE `{}` SET absence_start = 0 WHERE id = 1", STATE_TABLE);
        CharacterDatabase.CommitTransaction(trans);

        LOG_INFO("module", "[RespawnFreeze] Abwesenheit {}. {} Respawn-Zeiten mit ihrer Restzeit fortgesetzt, davon {} auf geladenen Karten{}.",
                 HumanDuration(absence), res.shifted, res.inMemory, SkippedNote(res));
    }
};

void AddOfflineRespawnFreezeScripts()
{
    new OfflineRespawnFreezeWorldScript();
    new OfflineRespawnFreezePlayerScript();
}
