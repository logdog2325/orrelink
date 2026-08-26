// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <jni.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/TraversalClient.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayProto.h"
#include "Core/NetPlayCommon.h"
#include "Core/NetPlayServer.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#endif
#include "UICommon/GameFile.h"
#include "UICommon/XDNetplay/BattleCustomizer.h"
#include "UICommon/XDNetplay/DisposableSave.h"
#include "UICommon/XDNetplay/Gen3Save.h"
#include "UICommon/XDNetplay/PartyBundle.h"
#include "UICommon/XDNetplay/TeamInjector.h"

#include "jni/AndroidCommon/AndroidCommon.h"
#include "jni/AndroidCommon/IDCache.h"
#include "jni/NetPlay/NetPlayUICallbacks.h"

static NetPlay::NetPlayUICallbacks* GetUICallbacksPointer(JNIEnv* env, jobject obj)
{
  return reinterpret_cast<NetPlay::NetPlayUICallbacks*>(
      env->GetLongField(obj, IDCache::GetNetPlayUICallbacksPointer()));
}

static NetPlay::NetPlayClient* GetClientPointer(JNIEnv* env, jobject obj)
{
  return reinterpret_cast<NetPlay::NetPlayClient*>(
      env->GetLongField(obj, IDCache::GetNetPlayClientPointer()));
}

static NetPlay::NetPlayServer* GetServerPointer(JNIEnv* env, jobject obj)
{
  return reinterpret_cast<NetPlay::NetPlayServer*>(
      env->GetLongField(obj, IDCache::GetNetPlayServerPointer()));
}

// The save file this player's OWN GBA slot (port 2, device 1) will play with:
// the same ROM resolution as SaveImport/TeamInjector (this socket's ROM,
// falling back to the other socket's -- a launcher-made setup points both at
// one dump), then the same GetSavePath netplay's SyncSaveData reads. Empty
// when the setup is not complete enough to have one.
static std::string OwnGbaSavePath()
{
#ifdef HAS_LIBMGBA
  std::string rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[1]);
  if (rom.empty() || !File::Exists(rom))
    rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[2]);
  if (rom.empty() || !File::Exists(rom))
    return {};
  return HW::GBA::Core::GetSavePath(rom, 1);
#else
  return {};
#endif
}

extern "C" {

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeSendMessage(JNIEnv* env,
                                                                                 jobject obj,
                                                                                 jstring jmessage)
{
  if (auto* client = GetClientPointer(env, obj))
    client->SendChatMessage(GetJString(env, jmessage));
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeSubmitTeam(JNIEnv* env,
                                                                                jobject obj,
                                                                                jstring jteam,
                                                                                jstring jname,
                                                                                jint jmodel)
{
  // XD Netplay: hand this player's Showdown team -- plus the in-game trainer
  // name they want to play under and their cosmetic trainer-model pick -- to
  // the host, which writes the team into the GBA save it syncs at start and
  // folds the model into the "$OrreLink Battle Style" AR block. All three
  // travel in ONE TeamData string; the payload grammar lives in
  // UICommon/XDNetplay/TeamInjector.h. An empty name means "keep the host
  // save's existing name"; model <= 0 means "no preference" (no Model: header
  // at all -- the host's Guest-model fallback dropdown wins).
  if (auto* client = GetClientPointer(env, obj))
  {
    client->SendTeamSubmission(XDNetplay::BuildTeamSubmissionPayload(
        GetJString(env, jteam), GetJString(env, jname),
        jmodel > 0 ? std::optional<int>(jmodel) : std::nullopt));
  }
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeSubmitSaveBundle(
    JNIEnv* env, jobject obj, jint jmodel)
{
  // XD Netplay "Use my save": submit the party FROM THIS PLAYER'S OWN SAVE --
  // real mon bytes, real trainer identity -- instead of a Showdown paste.
  // Reads the local port-2 save (their imported or team-editor slot), extracts
  // the fixed-size party bundle (UICommon/XDNetplay/PartyBundle.h documents
  // the layout and why the trainer identity must travel with the party), and
  // sends it as a "SaveBundle:" TeamData payload. No Name header is ever sent
  // for a bundle -- the save's real trainer name wins (renaming would split
  // the trainer from the mons' OT copies and make the party disobedient) --
  // but the cosmetic model pick stays meaningful and rides exactly as for a
  // Showdown submission (<= 0 means "no preference"). The host validates the
  // bundle strictly before writing anything.
  //
  // Returns "" on success or a user-displayable reason on failure (the Kotlin
  // side shows it as a local chat line; nothing was sent in that case).
  auto* client = GetClientPointer(env, obj);
  if (!client)
    return ToJString(env, "not connected to a room");

  const std::string save_path = OwnGbaSavePath();
  if (save_path.empty())
    return ToJString(env, "no GBA ROM is configured, so there is no save to read");
  if (!File::Exists(save_path))
  {
    return ToJString(env,
                     "your GBA slot has no save yet -- open the Team Editor or import a save first");
  }

  std::string error;
  const auto save = XDNetplay::LoadSaveFile(save_path, &error);
  if (!save)
    return ToJString(env, error);

  const auto bundle = XDNetplay::PartyBundle::Extract(*save, &error);
  if (!bundle)
    return ToJString(env, error);

  client->SendTeamSubmission(XDNetplay::BuildBundleSubmissionPayload(
      *bundle, jmodel > 0 ? std::optional<int>(jmodel) : std::nullopt));
  return ToJString(env, "");
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeSetHostInputAuthority(
    JNIEnv* env, jobject obj, jboolean enable)
{
  if (auto* server = GetServerPointer(env, obj))
    server->SetHostInputAuthority(static_cast<bool>(enable));
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeAdjustClientPadBufferSize(
    JNIEnv* env, jobject obj, jint buffer)
{
  if (auto* client = GetClientPointer(env, obj))
    client->AdjustPadBufferSize(static_cast<u32>(buffer));
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeAdjustServerPadBufferSize(
    JNIEnv* env, jobject obj, jint buffer)
{
  // This is the host's own buffer control in the room UI, i.e. a manual edit:
  // it takes the automatic sizer out of the loop so the typed value sticks.
  if (auto* server = GetServerPointer(env, obj))
    server->SetPadBufferSizeManual(static_cast<u32>(buffer));
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeSetAutoPadBuffer(
    JNIEnv* env, jobject obj, jboolean enable)
{
  if (auto* server = GetServerPointer(env, obj))
    server->SetAutoPadBufferEnabled(static_cast<bool>(enable));
}

JNIEXPORT jlong JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeCreateUICallbacks(
    JNIEnv* env, jobject obj, jobjectArray jgame_files)
{
  std::vector<std::shared_ptr<const UICommon::GameFile>> games;
  const jsize count = env->GetArrayLength(jgame_files);
  games.reserve(count);
  for (jsize i = 0; i < count; i++)
  {
    jobject jgame_file = env->GetObjectArrayElement(jgame_files, i);
    auto* game_ptr = reinterpret_cast<std::shared_ptr<const UICommon::GameFile>*>(
        env->GetLongField(jgame_file, IDCache::GetGameFilePointer()));
    if (game_ptr)
      games.push_back(*game_ptr);
    env->DeleteLocalRef(jgame_file);
  }

  return reinterpret_cast<jlong>(new NetPlay::NetPlayUICallbacks(obj, std::move(games)));
}

JNIEXPORT jlong JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeJoin(JNIEnv* env, jobject obj)
{
  auto* ui = GetUICallbacksPointer(env, obj);

  const std::string traversal_host = Config::Get(Config::NETPLAY_TRAVERSAL_SERVER);
  const u16 traversal_port = Config::Get(Config::NETPLAY_TRAVERSAL_PORT);
  const std::string nickname = Config::Get(Config::NETPLAY_NICKNAME);

  std::string host_ip;
  u16 host_port;
  bool is_traversal;

  // When hosting, join our own server on localhost
  if (auto* server = GetServerPointer(env, obj))
  {
    host_ip = "127.0.0.1";
    host_port = server->GetPort();
    is_traversal = false;
  }
  else
  {
    const std::string traversal_choice = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE);
    is_traversal = traversal_choice == "traversal";
    host_ip = is_traversal ? Config::Get(Config::NETPLAY_HOST_CODE) :
                             Config::Get(Config::NETPLAY_ADDRESS);
    host_port = Config::Get(Config::NETPLAY_CONNECT_PORT);
  }

  auto client = std::make_unique<NetPlay::NetPlayClient>(
      host_ip, host_port, ui, nickname,
      NetPlay::NetTraversalConfig{is_traversal, traversal_host, traversal_port});

  if (!client->IsConnected())
    return 0;

  return reinterpret_cast<jlong>(client.release());
}

JNIEXPORT jlong JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeHost(JNIEnv* env, jobject obj)
{
  auto* ui = GetUICallbacksPointer(env, obj);

  const std::string traversal_choice = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE);
  const bool is_traversal = traversal_choice == "traversal";
  const bool use_upnp = Config::Get(Config::NETPLAY_USE_UPNP);
  const std::string traversal_host = Config::Get(Config::NETPLAY_TRAVERSAL_SERVER);
  const u16 traversal_port = Config::Get(Config::NETPLAY_TRAVERSAL_PORT);
  const u16 traversal_port_alt = Config::Get(Config::NETPLAY_TRAVERSAL_PORT_ALT);

  const u16 host_port = is_traversal ? Config::Get(Config::NETPLAY_LISTEN_PORT) :
                                       Config::Get(Config::NETPLAY_HOST_PORT);

  // FORMAT host gate (mirrors the desktop MainWindow::NetPlayHost hook): with
  // the Format pick on Orre Colosseum, the host's port-2 party and port-3
  // guest-fallback party must both be legal before a room can open. Runs
  // BEFORE the disposable-save swap so a refusal leaves nothing swapped, and
  // ships its reason through the same connection-error surface the FRLG/
  // disposable hosting refusal uses. Format = Free: one int compare, hosting
  // untouched.
  if (std::string format_reason; !XDNetplay::ValidateHostPartiesForFormat(&format_reason))
  {
    if (ui)
    {
      ui->OnConnectionError("Cannot host an Orre Colosseum room: " + format_reason +
                            " - fix the team, or switch the Format back to Free");
    }
    return 0;
  }

  // Disposable host saves (mirrors the desktop MainWindow::NetPlayHost hook):
  // if a GBA port holds a user-imported save, swap in a rebuilt save carrying
  // only its party and trainer identity BEFORE the server exists -- netplay
  // syncs the host's GBA socket saves to every client, and a guest can join
  // (and submit a team into port 3) the moment the room is up, so this must
  // precede both. The original returns when the room closes
  // (NetPlayUICallbacks::OnRoomClosed). Refusal -- e.g. a FireRed/LeafGreen
  // import no disposable can be built from -- aborts hosting loudly through
  // the existing connection-error surface rather than silently syncing the
  // complete save file.
  if (std::string swap_error; !XDNetplay::DisposableSave::PrepareForHosting(&swap_error))
  {
    if (ui)
      ui->OnConnectionError("Cannot host: " + swap_error);
    return 0;
  }

  auto server = std::make_unique<NetPlay::NetPlayServer>(
      host_port, use_upnp, ui,
      NetPlay::NetTraversalConfig{is_traversal, traversal_host, traversal_port,
                                  traversal_port_alt});

  if (!server->is_connected)
    return 0;

  const std::string network_mode = Config::Get(Config::NETPLAY_NETWORK_MODE);
  const bool host_input_authority = network_mode == "hostinputauthority" || network_mode == "golf";
  server->SetHostInputAuthority(host_input_authority);
  server->AdjustPadBufferSize(Config::Get(Config::NETPLAY_BUFFER_SIZE));

  // Session-boundary scrub for the cosmetic battle-style feature (mirrors the
  // desktop EnsureGbaConfig hook): remove any "$OrreLink Battle Style" block a
  // crashed session left orphaned in the local GXXE01.ini, and start with a
  // clean guest-model stash for the room this server is about to run.
  XDNetplay::BattleCustomizer::BeginSession();

  return reinterpret_cast<jlong>(server.release());
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeChangeGame(JNIEnv* env,
                                                                                jobject obj,
                                                                                jobject jgame_file)
{
  auto* server = GetServerPointer(env, obj);
  if (!server)
    return;

  const auto& game_file = *reinterpret_cast<std::shared_ptr<const UICommon::GameFile>*>(
      env->GetLongField(jgame_file, IDCache::GetGameFilePointer()));

  server->ChangeGame(game_file->GetSyncIdentifier(), game_file->GetLongName());
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeDoAllPlayersHaveGame(
    JNIEnv* env, jobject obj)
{
  if (auto* client = GetClientPointer(env, obj))
    return static_cast<jboolean>(client->DoAllPlayersHaveGame());
  return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeStartGame(JNIEnv* env,
                                                                               jobject obj)
{
  auto* server = GetServerPointer(env, obj);
  if (!server)
    return;

  // Assign the XD GBA-vs-GBA ports before starting. Dolphin's netplay config
  // loader rebuilds every SI channel and every GBA ROM path at boot purely from
  // the session's pad_map + gba_config (NetPlayConfigLoader.cpp:136-208),
  // ignoring each machine's local SIDevice/GBA settings. The Android UI never
  // populated those (grep of Source/Android for SetPadMapping/SetGBAConfig is
  // empty), so without this every channel collapsed to SIDEVICE_NONE and the
  // GBA ROM paths were blanked -- the "config wipe" that made hosted battles
  // fail while solo boot worked. Mirror the desktop "Assign Ports" wiring
  // (NetPlayDialog.cpp:340-341) for our fixed two-player layout:
  //   port 1 (idx0) = host GC controller -> pad_map[0] = host pid, gba off
  //   port 2 (idx1) = host GBA           -> pad_map[1] = host pid, gba on
  //   port 3 (idx2) = guest GBA          -> pad_map[2] = guest pid, gba on
  //   port 4 (idx3) = unused             -> pad_map[3] = 0
  // A slot becomes a GBA iff gba_config[i].enabled && pad_map[i] > 0
  // (NetPlayConfigLoader.cpp:144-147); a mapped-but-not-GBA slot owned by the
  // local player becomes its GC controller, and one owned by a remote player a
  // remote controller. The host is always pid 1 (its loopback client connects
  // first, Client::IsHost() == pid==1) and XD is a strict two-player game, so
  // the guest is pid 2. This runs on every start, which also re-applies the
  // mapping after a disconnect/reconnect (which wipes it, NetPlayServer.cpp:
  // 581-590, and refills the slot as a plain controller, not a GBA).
  constexpr NetPlay::PlayerId HOST_PID = 1;
  constexpr NetPlay::PlayerId GUEST_PID = 2;

  NetPlay::PadMappingArray pad_map{};
  pad_map[0] = HOST_PID;
  pad_map[1] = HOST_PID;
  pad_map[2] = GUEST_PID;
  pad_map[3] = 0;

  NetPlay::GBAConfigArray gba_config{};
  gba_config[1].enabled = true;
  gba_config[2].enabled = true;

  // Sync the host's two GBA team saves to the guest so both sides emulate
  // identical teams (SyncSaves, default true), and give each player only their
  // own GBA window instead of both (HideRemoteGBAs, default false). Set both
  // explicitly so a stale user config can't break the session.
  Config::SetBaseOrCurrent(Config::NETPLAY_SAVEDATA_LOAD, true);
  Config::SetBaseOrCurrent(Config::NETPLAY_HIDE_REMOTE_GBAS, true);

  // Cheats are off by default. Sync the enabled-code list from host to guest so
  // the HOST'S choice governs both sides: if the host enabled the $XD OU Fixes
  // code (OU format) it carries to the guest; if the host left cheats off, both
  // run clean. Never force cheats on here -- a host/guest mismatch is exactly
  // what desyncs turn 1, so the host is the single source of truth.
  Config::SetBaseOrCurrent(Config::NETPLAY_SYNC_CODES, true);

  // SetGBAConfig(update_rom=true) reads MAIN_GBA_ROM_PATHS[1]/[2] (pointed at
  // the shared Emerald dump by the launcher) to fill each enabled slot's
  // hash/title, which the guest hash-matches against its identical local copy.
  // Order relative to SetPadMapping does not matter (each broadcasts its own
  // packet), but both must precede RequestStartGame.
  server->SetPadMapping(pad_map);
  server->SetGBAConfig(gba_config, /*update_rom=*/true);

  // Rebuild the "$OrreLink Battle Style" cosmetic AR block from the host's
  // launcher selections plus the guest's submitted model, and force cheats on
  // when (and only when) the block is non-empty so it actually loads and
  // ships (mirrors the desktop ApplyStartForcing hook). Must precede
  // RequestStartGame: that is what snapshots MAIN_ENABLE_CHEATS
  // (SetupNetSettings) and re-reads the local GXXE01.ini off disk (SyncCodes).
  XDNetplay::BattleCustomizer::PrepareForStart();

  server->RequestStartGame();
}

JNIEXPORT jint JNICALL Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeGetPort(
    JNIEnv* env, jobject obj)
{
  if (auto* server = GetServerPointer(env, obj))
    return static_cast<jint>(server->GetPort());
  return 0;
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeGetExternalIpAddress(
    JNIEnv* env, jobject)
{
  std::string ip = NetPlay::GetExternalIPAddress();
  if (ip.empty())
    return nullptr;
  return ToJString(env, ip);
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeReconnectTraversal(JNIEnv*,
                                                                                        jobject)
{
  if (Common::g_TraversalClient)
    Common::g_TraversalClient->ReconnectToServer();
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeReleaseUICallbacks(
    JNIEnv*, jobject, jlong pointer)
{
  delete reinterpret_cast<NetPlay::NetPlayUICallbacks*>(pointer);
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeReleaseBootSessionData(
    JNIEnv*, jobject, jlong pointer)
{
  delete reinterpret_cast<BootSessionData*>(pointer);
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeReleaseClient(JNIEnv*, jobject,
                                                                                   jlong pointer)
{
  delete reinterpret_cast<NetPlay::NetPlayClient*>(pointer);
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_netplay_NetplaySession_nativeReleaseServer(JNIEnv*, jobject,
                                                                                   jlong pointer)
{
  delete reinterpret_cast<NetPlay::NetPlayServer*>(pointer);
}

}  // extern "C"
