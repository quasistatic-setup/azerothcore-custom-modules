/*
 * mod-offline-respawn-freeze
 *
 * Haelt die Respawn-Zeit von Kreaturen an, solange der Server nicht laeuft.
 *
 * Hintergrund: creature_respawn.respawnTime ist eine absolute Unix-Zeit. Ohne
 * Zutun laeuft die Respawnzeit daher auch dann weiter, wenn niemand spielt.
 * Dieses Modul verschiebt beim Start die noch offenen Zeitpunkte um genau die
 * Ausfallzeit nach hinten. GameObjects (Kraeuter, Erz, Truhen) bleiben
 * unberuehrt, ebenso Auktionen, Post, Kalender und die Weltzeit.
 *
 * Siehe docs/freeze-konzept.md fuer die ausfuehrliche Begruendung.
 */

#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ScriptMgr.h"

namespace
{
    constexpr char const* STATE_TABLE = "mod_offline_respawn_freeze";
    constexpr uint32 SECONDS_PER_DAY  = 86400;

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

    // Bewusst dieser Hook: Er laeuft, nachdem die Datenbankverbindungen stehen,
    // aber bevor die erste Karte erzeugt wird. Karten lesen ihre Respawn-Zeiten
    // beim Erzeugen (Map::LoadRespawnTimes); spaeter zu schreiben haette keine
    // Wirkung mehr auf den bereits geladenen Zustand.
    void OnBeforeWorldInitialized() override
    {
        if (!_enabled)
            return;

        EnsureStateTable();
        ApplyShift();
    }

    void OnUpdate(uint32 diff) override
    {
        if (!_enabled || _heartbeatMs == 0)
            return;

        _sinceHeartbeat += diff;
        if (_sinceHeartbeat < _heartbeatMs)
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
    bool   _dryRun         = false;
    bool   _includeInstances = true;
    uint32 _heartbeatMs    = 60000;
    uint32 _minDowntime    = 60;
    uint32 _maxDowntimeDays = 60;
    uint32 _maxFutureDays  = 30;
    uint32 _graceSeconds   = 300;
    uint32 _sinceHeartbeat = 0;

    void LoadConfig()
    {
        _enabled          = sConfigMgr->GetOption<bool>  ("OfflineRespawnFreeze.Enable", true);
        _dryRun           = sConfigMgr->GetOption<bool>  ("OfflineRespawnFreeze.DryRun", false);
        _includeInstances = sConfigMgr->GetOption<bool>  ("OfflineRespawnFreeze.IncludeInstances", true);
        _minDowntime      = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.MinDowntimeSeconds", 60);
        _maxDowntimeDays  = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.MaxDowntimeDays", 60);
        _maxFutureDays    = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.MaxFutureDays", 30);

        _graceSeconds     = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.StartupGraceSeconds", 300);

        uint32 heartbeat  = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.HeartbeatSeconds", 60);
        _heartbeatMs      = heartbeat * 1000;

        if (_maxFutureDays == 0)
        {
            LOG_WARN("module", "[RespawnFreeze] MaxFutureDays ist 0; das wuerde jede Verschiebung verhindern. Verwende 30.");
            _maxFutureDays = 30;
        }
    }

    void EnsureStateTable()
    {
        // Bewusst im Code statt als SQL-Datei: Es handelt sich um eine einzige
        // Tabelle, die vorhanden sein muss, bevor irgendetwas anderes laeuft.
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `{}` ("
            "`id` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
            "`last_seen` INT UNSIGNED NOT NULL,"
            "`clean_shutdown` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`last_shift` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`last_shift_rows` INT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (`id`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci "
            "COMMENT='mod-offline-respawn-freeze: letzter bekannter Serverzeitpunkt'",
            STATE_TABLE);
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

    void ApplyShift()
    {
        uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());

        QueryResult state = CharacterDatabase.Query(
            "SELECT last_seen, clean_shutdown FROM `{}` WHERE id = 1", STATE_TABLE);

        if (!state)
        {
            LOG_INFO("module", "[RespawnFreeze] Erster Lauf, kein frueherer Zeitstempel. Es wird nichts verschoben.");
            WriteState(now, false, true);
            return;
        }

        Field* fields         = state->Fetch();
        uint32 const lastSeen = fields[0].Get<uint32>();
        bool   const wasClean = fields[1].Get<uint8>() != 0;

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
                     _heartbeatMs / 1000);

        if (downtime < static_cast<int64>(_minDowntime))
        {
            LOG_INFO("module", "[RespawnFreeze] Ausfallzeit {} liegt unter der Schwelle. Es wird nichts verschoben.",
                     HumanDuration(downtime));
            WriteState(now, false, true);
            return;
        }

        int64 const maxDowntime = static_cast<int64>(_maxDowntimeDays) * SECONDS_PER_DAY;
        if (downtime > maxDowntime)
        {
            LOG_WARN("module", "[RespawnFreeze] Ausfallzeit {} uebersteigt die Obergrenze von {} Tagen. Es wird nichts verschoben.",
                     HumanDuration(downtime), _maxDowntimeDays);
            WriteState(now, false, true);
            return;
        }

        // Zwischen dem Start des Servers und dem Augenblick, in dem tatsaechlich
        // jemand in der Welt steht, vergeht Zeit: Client starten, anmelden,
        // Charakter waehlen, Gebiet laden. Diese Spanne liefe ungebremst gegen
        // die Respawns, und die Bots toeten waehrenddessen bereits. Die Karenz
        // gleicht das aus. Sie wird erst hier aufgeschlagen, damit sie die
        // Plausibilitaetsschwellen oben nicht verfaelscht.
        int64 const shift = downtime + static_cast<int64>(_graceSeconds);

        // Die WHERE-Bedingung faengt alle Sonderfaelle ab:
        //   respawnTime > lastSeen  laesst bereits faellige Eintraege in Ruhe,
        //                           die sonst nachtraeglich in die Zukunft
        //                           wandern und damit spaeter erschienen.
        //   respawnTime < obergrenze schuetzt die Markierung "now + YEAR", mit
        //                           der Map::SaveCreatureRespawnTime Kreaturen
        //                           kennzeichnet, die vor dem Instanz-Reset
        //                           nicht wiederkehren.
        uint32 const upperBound = lastSeen + _maxFutureDays * SECONDS_PER_DAY;
        std::string const scope = _includeInstances ? "" : " AND instanceId = 0";

        QueryResult count = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM creature_respawn WHERE respawnTime > {} AND respawnTime < {}{}",
            lastSeen, upperBound, scope);

        uint32 const affected = count ? (*count)[0].Get<uint64>() : 0;

        LOG_INFO("module", "[RespawnFreeze] Ausfallzeit {}, zuzueglich {} Karenz, ergibt {} Verschiebung. {} Eintraege betroffen{}.",
                 HumanDuration(downtime), HumanDuration(_graceSeconds), HumanDuration(shift),
                 affected, _includeInstances ? "" : ", offene Welt");

        if (_dryRun)
        {
            LOG_INFO("module", "[RespawnFreeze] Probelauf aktiv: es wird nichts geschrieben. Zum Scharfschalten OfflineRespawnFreeze.DryRun = 0 setzen.");
            WriteState(now, false, true);
            return;
        }

        if (affected == 0)
        {
            LOG_INFO("module", "[RespawnFreeze] Keine offenen Respawn-Zeiten zu verschieben.");
            WriteState(now, false, true);
            return;
        }

        // Synchron, damit die Aenderung nachweislich abgeschlossen ist, bevor
        // die erste Karte ihre Respawn-Zeiten liest.
        CharacterDatabase.DirectExecute(
            "UPDATE creature_respawn SET respawnTime = respawnTime + {} "
            "WHERE respawnTime > {} AND respawnTime < {}{}",
            shift, lastSeen, upperBound, scope);

        LOG_INFO("module", "[RespawnFreeze] {} Respawn-Zeiten um {} verschoben.",
                 affected, HumanDuration(shift));

        CharacterDatabase.DirectExecute(
            "INSERT INTO `{}` (id, last_seen, clean_shutdown, last_shift, last_shift_rows) "
            "VALUES (1, {}, 0, {}, {}) "
            "ON DUPLICATE KEY UPDATE last_seen = VALUES(last_seen), clean_shutdown = 0, "
            "last_shift = VALUES(last_shift), last_shift_rows = VALUES(last_shift_rows)",
            STATE_TABLE, now, shift, affected);
    }
};

void AddOfflineRespawnFreezeScripts()
{
    new OfflineRespawnFreezeWorldScript();
}
