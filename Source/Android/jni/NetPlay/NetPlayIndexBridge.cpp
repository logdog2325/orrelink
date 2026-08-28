// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Android bridge for the NetPlay session index ("lobby server", NETPLAY_INDEX_URL,
// default https://lobby.dolphin-emu.org), plus the three other small xdnetplay-package bridges
// that share this file: the XD Netplay release check (second extern "C" group; Kotlin
// counterpart features/xdnetplay/UpdateCheckBridge), the cosmetic battle-style selectors
// (third group; Kotlin counterpart features/xdnetplay/BattleStyleBridge), the per-port
// user-save import (fourth group; Kotlin counterpart features/xdnetplay/SaveImportBridge), and the
// battle-format paste-time validation (fifth group; Kotlin counterpart
// features/xdnetplay/FormatBridge).
//
// Listing only. Publishing is deliberately NOT exposed here: Core's NetPlayServer
// already owns a NetPlayIndex (NetPlayServer::m_index) and publishes automatically
// from NetPlayServer::SetupIndex() whenever the NetPlay/UseIndex, NetPlay/IndexName
// and NetPlay/IndexRegion config keys are set before/while hosting. SetupIndex()
// runs at server creation for "direct" hosting and from OnTraversalStateChanged()
// once a traversal host code has been assigned, and the index entry is removed by
// NetPlayIndex's destructor when the server is torn down. A second, static index
// instance here would double-publish and would not track player count / in-game
// state, so the Android host flow publishes purely by setting those config keys
// from Kotlin (see features/xdnetplay/ui/FindBattlesActivity.kt).
//
// Kotlin counterpart: org.dolphinemu.dolphinemu.features.xdnetplay.NetPlayIndexBridge
//
// Session row encoding (String[10], see NetPlayIndexBridge.kt / LobbySession):
//   [0] name  [1] region  [2] method ("traversal"/"direct")  [3] server_id
//   [4] game  [5] version  [6] player_count  [7] port
//   [8] has_password ("1"/"0")  [9] in_game ("1"/"0")
//
// Version semantics: NetPlayIndex::Add() publishes version=Common::GetScmDescStr()
// (the scmdesc/"git describe" string of the build), and the desktop browser both
// sends it as the server-side "version" filter and greys out rows whose version
// differs from the local GetScmDescStr(). We do the same: the filter is always
// sent, and any stragglers the server returns anyway are dropped client-side, so
// only sessions hosted by the exact same fork build are ever surfaced.

#include <jni.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/Config/Config.h"
#include "Common/Version.h"
#include "Core/Config/MainSettings.h"
#include "UICommon/NetPlayIndex.h"
#include "UICommon/XDNetplay/BattleCustomizer.h"
#include "UICommon/XDNetplay/DisposableSave.h"
#include "UICommon/XDNetplay/FormatRules.h"
#include "UICommon/XDNetplay/Gen3Data.h"
#include "UICommon/XDNetplay/Gen3Mon.h"
#include "UICommon/XDNetplay/SaveImport.h"
#include "UICommon/XDNetplay/ShowdownParser.h"
#include "UICommon/XDNetplay/UpdateCheck.h"
#include "UICommon/XDNetplay/Version.h"

#include "jni/AndroidCommon/AndroidCommon.h"

namespace
{
std::mutex s_last_error_mutex;
std::string s_last_error;

void SetLastError(std::string error)
{
  std::lock_guard lock(s_last_error_mutex);
  s_last_error = std::move(error);
}

std::string GetLastError()
{
  std::lock_guard lock(s_last_error_mutex);
  return s_last_error;
}

// Battle-style helper: flatten a BattleCustomizer option table for the Kotlin
// side as [id, name, tier, portrait, ...] quads (id decimal, tier "safe"/
// "experimental", portrait "1"/"0" for model tables -- ModelHasPortrait -- and
// "-" for music/venue, where the notion does not apply). Music/venue tables
// are ordered tested-safe first, so a dropdown can insert its "Experimental"
// divider at the first tier change; model dropdowns present the portrait flag
// instead of the tier (the models are field-proven).
jobjectArray StyleTableToJava(JNIEnv* env,
                              std::span<const XDNetplay::BattleCustomizer::StyleOption> table,
                              bool model_table)
{
  std::vector<std::string> flat;
  flat.reserve(table.size() * 4);
  for (const auto& option : table)
  {
    flat.push_back(std::to_string(option.id));
    flat.push_back(option.name);
    flat.push_back(option.tier == XDNetplay::BattleCustomizer::Tier::Experimental ?
                       "experimental" :
                       "safe");
    flat.push_back(!model_table ? "-" :
                   XDNetplay::BattleCustomizer::ModelHasPortrait(option.id) ? "1" :
                                                                              "0");
  }
  return SpanToJStringArray(env, flat);
}

// Format-bridge helper: the bundled gen3data.json, parsed once and cached for
// the process (the file is static data, and the Submit Team sheet revalidates
// on every draft edit -- reparsing 70 KB of JSON per keystroke would be
// wasteful). Returns nullptr when the file is unreadable, in which case the
// validators below report "legal": a paste-time note must NEVER block, and the
// enforcing gates in shared core (TeamInjector) do their own loud refusals in
// that state.
const XDNetplay::Gen3Data* CachedGen3Data()
{
  static const std::optional<XDNetplay::Gen3Data> s_data = XDNetplay::Gen3Data::LoadBundled();
  return s_data ? &*s_data : nullptr;
}
}  // namespace

extern "C" {

// Blocking HTTP call — must be invoked off the Android main thread
// (Dispatchers.IO on the Kotlin side).
JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_NetPlayIndexBridge_nativeListSessions(
    JNIEnv* env, jobject, jstring jname, jstring jregion, jstring jgame_id)
{
  std::map<std::string, std::string> filters;

  const std::string name = GetJString(env, jname);
  const std::string region = GetJString(env, jregion);
  const std::string game_id = GetJString(env, jgame_id);

  if (!name.empty())
    filters["name"] = name;
  if (!region.empty())
    filters["region"] = region;
  if (!game_id.empty())
    filters["game"] = game_id;

  // Only sessions from the same build are joinable; mirror the desktop browser's
  // "Hide Incompatible Sessions" behaviour, permanently enabled.
  filters["version"] = Common::GetScmDescStr();

  NetPlayIndex client;
  auto sessions = client.List(filters);
  if (!sessions)
  {
    SetLastError(client.GetLastError());
    return nullptr;
  }

  // Belt and suspenders: drop anything the server returned with a foreign version.
  std::vector<NetPlaySession> compatible;
  compatible.reserve(sessions->size());
  for (auto& session : *sessions)
  {
    if (session.version == Common::GetScmDescStr())
      compatible.push_back(std::move(session));
  }

  jclass string_array_class = env->FindClass("[Ljava/lang/String;");
  if (!string_array_class)
  {
    SetLastError("JNI_CLASS_LOOKUP_FAILED");
    return nullptr;
  }

  jobjectArray result =
      env->NewObjectArray(static_cast<jsize>(compatible.size()), string_array_class, nullptr);
  if (!result)
  {
    SetLastError("JNI_ALLOC_FAILED");
    return nullptr;
  }

  for (jsize i = 0; i < static_cast<jsize>(compatible.size()); i++)
  {
    const NetPlaySession& session = compatible[i];
    const std::vector<std::string> fields{
        session.name,
        session.region,
        session.method,
        session.server_id,
        session.game_id,
        session.version,
        std::to_string(session.player_count),
        std::to_string(session.port),
        session.has_password ? "1" : "0",
        session.in_game ? "1" : "0",
    };
    jobjectArray row = SpanToJStringArray(env, fields);
    env->SetObjectArrayElement(result, i, row);
    env->DeleteLocalRef(row);
  }

  return result;
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_NetPlayIndexBridge_nativeGetLastError(
    JNIEnv* env, jobject)
{
  return ToJString(env, GetLastError());
}

// The version string the index stores for this build (Common::GetScmDescStr()).
JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_NetPlayIndexBridge_nativeGetScmVersion(
    JNIEnv* env, jobject)
{
  return ToJString(env, Common::GetScmDescStr());
}

// A lobby "password" does not gate the netplay server itself; it merely XOR-encrypts
// the published server_id (traversal code or IP). Decrypting it with the correct
// password (checksum-verified) is all a joiner needs. Returns null on a wrong
// password or corrupt ID.
JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_NetPlayIndexBridge_nativeDecryptServerId(
    JNIEnv* env, jobject, jstring jserver_id, jstring jpassword)
{
  NetPlaySession session;
  session.server_id = GetJString(env, jserver_id);

  const std::optional<std::string> decrypted = session.DecryptID(GetJString(env, jpassword));
  if (!decrypted)
    return nullptr;

  return ToJString(env, *decrypted);
}

// Region codes accepted by the index, flattened as [code0, label0, code1, label1, ...].
JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_NetPlayIndexBridge_nativeGetRegions(JNIEnv* env,
                                                                                      jobject)
{
  std::vector<std::string> flat;
  for (const auto& [code, label] : NetPlayIndex::GetRegions())
  {
    flat.push_back(code);
    flat.push_back(label);
  }
  return SpanToJStringArray(env, flat);
}

// The XD Netplay release this build belongs to, for the hub's version line.
JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_UpdateCheckBridge_nativeGetVersion(JNIEnv* env,
                                                                                     jobject)
{
  return ToJString(env, XDNetplay::VERSION);
}

// Blocking HTTP call -- must be invoked off the Android main thread (Dispatchers.IO on the
// Kotlin side). Flattened as [status, latest_tag, release_name, html_url, published_at, message];
// status is one of "uptodate" / "update" / "unknown" / "network", matching UpdateStatus.
JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_UpdateCheckBridge_nativeCheckForUpdate(
    JNIEnv* env, jobject)
{
  const XDNetplay::UpdateCheckResult result = XDNetplay::CheckForUpdate();

  const char* status = "unknown";
  switch (result.status)
  {
  case XDNetplay::Status::UpToDate:
    status = "uptodate";
    break;
  case XDNetplay::Status::UpdateAvailable:
    status = "update";
    break;
  case XDNetplay::Status::NetworkError:
    status = "network";
    break;
  case XDNetplay::Status::Unknown:
    break;
  }

  const std::vector<std::string> fields{
      status,
      result.latest_tag,
      result.release_name,
      result.html_url,
      result.published_at,
      result.message,
  };
  return SpanToJStringArray(env, fields);
}

// ---------------------------------------------------------------------------
// Battle-style bridge (Kotlin counterpart: features/xdnetplay/BattleStyleBridge)
// ---------------------------------------------------------------------------
//
// The cosmetic battle-style selectors: the option tables live ONLY in
// UICommon/XDNetplay/BattleCustomizer (Kotlin never duplicates them), and the
// four host selections live in the MAIN_XD_STYLE_* config keys that
// BattleCustomizer::ConfigSelection reads when the host assembles the
// "$OrreLink Battle Style" AR code. Nothing here touches AR codes -- the code
// block is assembled in shared core at host time (nativeStartGame ->
// BattleCustomizer::PrepareForStart and the TeamData arrival hook).
//
// Table encoding: flat [id, name, tier, portrait, ...] quads with id decimal,
// tier "safe" or "experimental" and portrait "1"/"0" (models) or "-" (music/
// venue). Music/venue tables are ordered tested-safe first, so a dropdown can
// insert its "Experimental" divider at the first tier change; model dropdowns
// show "(no portrait)" from the portrait flag instead of any tier marking.
//
// Selection encoding ("which", shared with BattleStyleBridge.kt):
//   0 = host's own model   1 = guest-model fallback   2 = music   3 = venue
// A selection of 0 is "Game default" (the AR section is absent, never a write
// of the vanilla value). Setters validate against the same tables the host
// assembly validates against: an unknown id is stored as 0, never clamped.

JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_BattleStyleBridge_nativeGetModelTable(JNIEnv* env,
                                                                                        jobject)
{
  return StyleTableToJava(env, XDNetplay::BattleCustomizer::ModelTable(), true);
}

JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_BattleStyleBridge_nativeGetMusicTable(JNIEnv* env,
                                                                                        jobject)
{
  return StyleTableToJava(env, XDNetplay::BattleCustomizer::MusicTable(), false);
}

JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_BattleStyleBridge_nativeGetVenueTable(JNIEnv* env,
                                                                                        jobject)
{
  return StyleTableToJava(env, XDNetplay::BattleCustomizer::VenueTable(), false);
}

JNIEXPORT jint JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_BattleStyleBridge_nativeGetSelection(JNIEnv*,
                                                                                       jobject,
                                                                                       jint which)
{
  switch (which)
  {
  case 0:
    return Config::Get(Config::MAIN_XD_STYLE_HOST_MODEL);
  case 1:
    return Config::Get(Config::MAIN_XD_STYLE_GUEST_MODEL);
  case 2:
    return Config::Get(Config::MAIN_XD_STYLE_MUSIC);
  case 3:
    return Config::Get(Config::MAIN_XD_STYLE_VENUE);
  default:
    return 0;
  }
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_BattleStyleBridge_nativeSetSelection(JNIEnv*,
                                                                                       jobject,
                                                                                       jint which,
                                                                                       jint id)
{
  // The dropdowns only offer table entries, but validate anyway so a bad value
  // can never reach the config keys the host assembles codes from: anything
  // not in the matching table becomes 0 ("Game default"), never a clamp.
  const Config::Info<int>* key = nullptr;
  bool valid = false;
  switch (which)
  {
  case 0:
    key = &Config::MAIN_XD_STYLE_HOST_MODEL;
    valid = XDNetplay::BattleCustomizer::IsValidModelId(id);
    break;
  case 1:
    key = &Config::MAIN_XD_STYLE_GUEST_MODEL;
    valid = XDNetplay::BattleCustomizer::IsValidModelId(id);
    break;
  case 2:
    key = &Config::MAIN_XD_STYLE_MUSIC;
    valid = XDNetplay::BattleCustomizer::IsValidMusicId(id);
    break;
  case 3:
    key = &Config::MAIN_XD_STYLE_VENUE;
    valid = XDNetplay::BattleCustomizer::IsValidVenueId(id);
    break;
  default:
    return;
  }
  Config::SetBaseOrCurrent(*key, valid ? id : 0);
  Config::Save();
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_BattleStyleBridge_nativePrepareForBoot(JNIEnv*,
                                                                                         jobject)
{
  // Solo boot path: netplay hosting runs this from nativeStartGame, but a solo
  // boot bypasses that entirely -- without this call a solo session never gets
  // the Battle Style block at all (the first field test found exactly that).
  // Cleanup rides the core-state hook PrepareForStart registers.
  XDNetplay::BattleCustomizer::PrepareForStart();

  // Crash self-heal for the disposable netplay saves (mirrors the desktop
  // EnsureGbaConfig hook): if a crashed hosted session left a
  // <save>.netplayorig stash behind, the socket save still holds the session's
  // disposable -- put the real import back so this solo boot plays the user's
  // own save again (DisposableSave.h). No-op while a room is open.
  XDNetplay::DisposableSave::HealLeftoverSession();
}

// ---------------------------------------------------------------------------
// Save-import bridge (Kotlin counterpart: features/xdnetplay/SaveImportBridge)
// ---------------------------------------------------------------------------
//
// Opt-in import of a user's own Gen 3 save over a GBA socket's bundled
// team-editor save. ALL of the logic lives in UICommon/XDNetplay/SaveImport,
// shared verbatim with the desktop launcher: the validation (size, structure,
// checksums, save-game vs port-ROM match), the refusals while emulation or a
// hosted room owns the save, the once-only <save>.preimport backup, the
// tmp/readback/rename write, and the per-port ImportedSave2/3 config
// bookkeeping. This bridge only ferries a REAL file path in -- the Kotlin
// side copies the SAF content:// pick to one first, exactly like the ROM
// picker, because the core reads plain files -- and the user-displayable
// outcome back.
//
// Outcome encoding (String[2], shared with SaveImportBridge.kt):
//   [0] "1" success / "0" refusal
//   [1] user-displayable message (import success/refusal text from the core;
//       empty on a successful restore, where Kotlin shows its own string)
// device is 1 (GBA port 2, your side) or 2 (GBA port 3, the guest slot),
// matching TeamRole.deviceNumber and the desktop launcher.

JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_SaveImportBridge_nativeImportUserSave(
    JNIEnv* env, jobject, jstring jsource_path, jint device)
{
  std::string status;
  std::string error;
  const bool ok =
      XDNetplay::SaveImport::ImportUserSave(GetJString(env, jsource_path), device, &status, &error);
  const std::vector<std::string> outcome{ok ? "1" : "0", ok ? status : error};
  return SpanToJStringArray(env, outcome);
}

JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_SaveImportBridge_nativeRestoreDefaultSave(
    JNIEnv* env, jobject, jint device)
{
  std::string error;
  const bool ok = XDNetplay::SaveImport::RestoreDefaultSave(device, &error);
  const std::vector<std::string> outcome{ok ? "1" : "0", ok ? std::string{} : error};
  return SpanToJStringArray(env, outcome);
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_SaveImportBridge_nativeHealLeftoverSession(
    JNIEnv*, jobject)
{
  // Boundary heal for a killed session's leftovers, callable from the screens
  // that read the socket saves (launcher checklist, team editor). Mirrors the
  // desktop launcher's showEvent heal; no-op unless leftovers exist, refuses
  // while a room or emulation is live.
  XDNetplay::DisposableSave::HealLeftoverSession();
}

// ---------------------------------------------------------------------------
// Format bridge (Kotlin counterpart: features/xdnetplay/FormatBridge)
// ---------------------------------------------------------------------------
//
// PASTE-TIME validation for the one-tap FORMAT pick (Free / Orre Colosseum /
// OU; only Orre Colosseum has a legality layer).
// The ruleset lives ONLY in UICommon/XDNetplay/FormatRules -- the same code
// the enforcing gates run (the host gate in nativeHost, the guest-submission
// gate in the host's TeamInjector) -- so a note shown here and a refusal shown
// there can never disagree. These entry points are advisory by contract: the
// Kotlin callers render the result as a non-blocking "note:" and still allow
// saving/pasting; only the gates block. The callers also check the local
// Format key BEFORE calling, so with Format = Free no validation call is ever
// made.
//
// Both return "" for "no complaint" (legal, or nothing parseable/readable to
// judge) and otherwise one human-readable reason from FormatRules, e.g.
// "banned species: Kyogre" / "duplicate item: Leftovers (x2)".

// Showdown-text form (the Submit Team sheet's draft). Shared core's own
// parser + Gen3Data resolution, so names resolve exactly as MonFactory will
// resolve them at build time. A pokepast.es LINK parses to no sets and gets no
// note -- it cannot be inspected without fetching; the host's gate still
// enforces on the fetched text.
JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_FormatBridge_nativeValidateShowdown(
    JNIEnv* env, jobject, jstring jtext)
{
  const XDNetplay::Gen3Data* data = CachedGen3Data();
  if (data == nullptr)
    return ToJString(env, "");

  const XDNetplay::FormatRules::Verdict verdict = XDNetplay::FormatRules::ValidateSets(
      Config::Get(Config::MAIN_XD_FORMAT), XDNetplay::ShowdownParser::ParseTeam(GetJString(env, jtext)),
      *data);
  return ToJString(env, verdict.ok ? std::string{} : verdict.reason);
}

// Built-mon form (the team editor's in-memory party): parallel arrays of
// INTERNAL (Hoenn) species ids and held-item ids, exactly as Kotlin's Gen3Mon
// carries them; FormatRules maps internal ids to National dex numbers before
// the ban list applies, and item/species display names in the reason come
// from Gen3Data.
JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_FormatBridge_nativeValidateParty(
    JNIEnv* env, jobject, jintArray jspecies, jintArray jitems, jintArray jlevels)
{
  const XDNetplay::Gen3Data* data = CachedGen3Data();
  if (data == nullptr)
    return ToJString(env, "");

  const jsize species_len = env->GetArrayLength(jspecies);
  const jsize items_len = env->GetArrayLength(jitems);
  const jsize count = std::min(species_len, items_len);
  std::vector<jint> species(static_cast<size_t>(std::max<jsize>(count, 0)));
  std::vector<jint> items(species.size());
  if (count > 0)
  {
    env->GetIntArrayRegion(jspecies, 0, count, species.data());
    env->GetIntArrayRegion(jitems, 0, count, items.data());
  }

  // Only the two FormatRules inputs are populated; pid stays 0, so a
  // species-0 entry reads as an empty slot (Gen3Mon::IsEmpty) and is skipped
  // rather than misjudged.
  // Levels ride a third parallel array (0 = unknown, skipped by the level
  // rule) so the Limited formats can warn about over-level mons at paste time.
  std::vector<jint> levels(species.size());
  if (count > 0 && jlevels != nullptr && env->GetArrayLength(jlevels) >= count)
    env->GetIntArrayRegion(jlevels, 0, count, levels.data());

  std::vector<XDNetplay::Gen3Mon> party(species.size());
  for (size_t i = 0; i < species.size(); i++)
  {
    party[i].species = static_cast<u32>(species[i]);
    party[i].held_item = static_cast<u32>(items[i]);
    party[i].level = static_cast<u32>(std::max<jint>(levels[i], 0));
  }

  const XDNetplay::FormatRules::Verdict verdict = XDNetplay::FormatRules::ValidateParty(
      Config::Get(Config::MAIN_XD_FORMAT), party, *data);
  return ToJString(env, verdict.ok ? std::string{} : verdict.reason);
}

}  // extern "C"
