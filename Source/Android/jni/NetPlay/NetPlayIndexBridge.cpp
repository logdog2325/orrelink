// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Android bridge for the NetPlay session index ("lobby server", NETPLAY_INDEX_URL,
// default https://lobby.dolphin-emu.org), plus the XD Netplay release check that shares this
// file because it is the other stateless xdnetplay-package bridge (see the second extern "C"
// group at the bottom; Kotlin counterpart features/xdnetplay/UpdateCheckBridge).
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

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Common/Version.h"
#include "UICommon/NetPlayIndex.h"
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

}  // extern "C"
