// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/NetPlay/NetPlayDialog.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "Common/HttpRequest.h"

#ifdef HAS_LIBMGBA
#include <fmt/ranges.h>
#endif

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/TraversalClient.h"
#include "Core/NetPlayCommon.h"

#include "Core/Boot/Boot.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#endif
#include "Core/IOS/FS/FileSystem.h"
#include "Core/NetPlayServer.h"
#include "Core/SyncIdentifier.h"
#include "Core/System.h"

#include "DolphinQt/NetPlay/ChunkedProgressDialog.h"
#include "DolphinQt/NetPlay/ClickBlurLabel.h"
#include "DolphinQt/NetPlay/GameDigestDialog.h"
#include "DolphinQt/NetPlay/GameListDialog.h"
#include "DolphinQt/NetPlay/PadMappingDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/QtUtils/RunOnObject.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/Settings/GameCubePane.h"
#include "DolphinQt/XDNetplay/XDNetplayConfig.h"

#include "UICommon/DiscordPresence.h"
#include "UICommon/GameFile.h"
#include "UICommon/XDNetplay/BattleCustomizer.h"
#include "UICommon/XDNetplay/DisposableSave.h"
#include "UICommon/XDNetplay/FormatRules.h"
#include "UICommon/XDNetplay/Gen3Data.h"
#include "UICommon/XDNetplay/Gen3Save.h"
#include "UICommon/XDNetplay/PartyBundle.h"
#include "UICommon/XDNetplay/ShowdownParser.h"
#include "UICommon/XDNetplay/TeamInjector.h"
#include "UICommon/UICommon.h"

#include "VideoCommon/NetPlayChatUI.h"
#include "VideoCommon/NetPlayGolfUI.h"

namespace
{
QString InetAddressToString(const Common::TraversalInetAddress& addr)
{
  QString ip;

  if (addr.isIPV6)
  {
    ip = QStringLiteral("IPv6-Not-Implemented");
  }
  else
  {
    const auto ipv4 = reinterpret_cast<const u8*>(addr.address);
    ip = QString::number(ipv4[0]);
    for (u32 i = 1; i != 4; ++i)
    {
      ip += QStringLiteral(".");
      ip += QString::number(ipv4[i]);
    }
  }

  return QStringLiteral("%1:%2").arg(ip, QString::number(ntohs(addr.port)));
}
}  // namespace

NetPlayDialog::NetPlayDialog(const GameListModel& game_list_model,
                             StartGameCallback start_game_callback, QWidget* parent)
    : QDialog(parent), m_game_list_model(game_list_model),
      m_start_game_callback(std::move(start_game_callback))
{
  setWindowTitle(tr("NetPlay"));
  setWindowIcon(Resources::GetAppIcon());

  m_pad_mapping = new PadMappingDialog(this);
  m_game_digest_dialog = new GameDigestDialog(this);
  m_chunked_progress_dialog = new ChunkedProgressDialog(this);

  ResetExternalIP();
  CreateChatLayout();
  CreatePlayersLayout();
  CreateMainLayout();
  LoadSettings();
  ConnectWidgets();

  const auto& settings = Settings::Instance().GetQSettings();

  restoreGeometry(settings.value(QStringLiteral("netplaydialog/geometry")).toByteArray());
  m_splitter->restoreState(settings.value(QStringLiteral("netplaydialog/splitter")).toByteArray());
}

NetPlayDialog::~NetPlayDialog()
{
  auto& settings = Settings::Instance().GetQSettings();

  settings.setValue(QStringLiteral("netplaydialog/geometry"), saveGeometry());
  settings.setValue(QStringLiteral("netplaydialog/splitter"), m_splitter->saveState());
}

void NetPlayDialog::CreateMainLayout()
{
  m_main_layout = new QGridLayout;
  m_game_button = new QPushButton;
  m_start_button = new QPushButton(tr("Start"));
  m_buffer_size_box = new QSpinBox;
  m_buffer_label = new QLabel(tr("Buffer:"));
  m_auto_buffer_box = new QCheckBox(tr("Auto"));
  m_auto_buffer_box->setToolTip(
      tr("Size the netplay buffer automatically from the measured ping, and keep it sized as the "
         "connection changes.\nA 200 ms connection needs about 12 frames of buffer; the old fixed "
         "default of 5 stutters badly on anything transatlantic.\nTyping a buffer value by hand "
         "turns this off, and your value is kept."));
  m_quit_button = new QPushButton(tr("Quit"));
  m_splitter = new QSplitter(Qt::Horizontal);
  m_menu_bar = new QMenuBar(this);

  m_data_menu = m_menu_bar->addMenu(tr("Data"));
  m_data_menu->setToolTipsVisible(true);

  m_savedata_none_action = m_data_menu->addAction(tr("No Save Data"));
  m_savedata_none_action->setToolTip(
      tr("Netplay will start without any save data, and any created save data will be discarded at "
         "the end of the Netplay session."));
  m_savedata_none_action->setCheckable(true);
  m_savedata_load_only_action = m_data_menu->addAction(tr("Load Host's Save Data Only"));
  m_savedata_load_only_action->setToolTip(tr(
      "Netplay will start using the Host's save data, but any save data created or modified during "
      "the Netplay session will be discarded at the end of the session."));
  m_savedata_load_only_action->setCheckable(true);
  m_savedata_load_and_write_action = m_data_menu->addAction(tr("Load and Write Host's Save Data"));
  m_savedata_load_and_write_action->setToolTip(
      tr("Netplay will start using the Host's save data, and any save data created or modified "
         "during the Netplay session will remain in the Host's local saves."));
  m_savedata_load_and_write_action->setCheckable(true);

  m_savedata_style_group = new QActionGroup(this);
  m_savedata_style_group->setExclusive(true);
  m_savedata_style_group->addAction(m_savedata_none_action);
  m_savedata_style_group->addAction(m_savedata_load_only_action);
  m_savedata_style_group->addAction(m_savedata_load_and_write_action);

  m_data_menu->addSeparator();

  m_savedata_all_wii_saves_action = m_data_menu->addAction(tr("Use All Wii Save Data"));
  m_savedata_all_wii_saves_action->setToolTip(tr(
      "If checked, all Wii saves will be used instead of only the save of the game being started. "
      "Useful when switching games mid-session. Has no effect if No Save Data is selected."));
  m_savedata_all_wii_saves_action->setCheckable(true);

  m_data_menu->addSeparator();

  m_sync_codes_action = m_data_menu->addAction(tr("Sync AR/Gecko Codes"));
  m_sync_codes_action->setCheckable(true);
  m_strict_settings_sync_action = m_data_menu->addAction(tr("Strict Settings Sync"));
  m_strict_settings_sync_action->setToolTip(
      tr("This will sync additional graphics settings, and force everyone to the same internal "
         "resolution.\nMay prevent desync in some games that use EFB reads. Please ensure everyone "
         "uses the same video backend."));
  m_strict_settings_sync_action->setCheckable(true);

  m_network_menu = m_menu_bar->addMenu(tr("Network"));
  m_network_menu->setToolTipsVisible(true);
  m_fixed_delay_action = m_network_menu->addAction(tr("Fair Input Delay"));
  m_fixed_delay_action->setToolTip(
      tr("Each player sends their own inputs to the game, with equal buffer size for all players, "
         "configured by the host.\nSuitable for competitive games where fairness and minimal "
         "latency are most important."));
  m_fixed_delay_action->setCheckable(true);
  m_host_input_authority_action = m_network_menu->addAction(tr("Host Input Authority"));
  m_host_input_authority_action->setToolTip(
      tr("Host has control of sending all inputs to the game, as received from other players, "
         "giving the host zero latency but increasing latency for others.\nSuitable for casual "
         "games with 3+ players, possibly on unstable or high latency connections."));
  m_host_input_authority_action->setCheckable(true);
  m_golf_mode_action = m_network_menu->addAction(tr("Golf Mode"));
  m_golf_mode_action->setToolTip(
      tr("Identical to Host Input Authority, except the \"Host\" (who has zero latency) can be "
         "switched at any time.\nSuitable for turn-based games with timing-sensitive controls, "
         "such as golf."));
  m_golf_mode_action->setCheckable(true);

  m_network_mode_group = new QActionGroup(this);
  m_network_mode_group->setExclusive(true);
  m_network_mode_group->addAction(m_fixed_delay_action);
  m_network_mode_group->addAction(m_host_input_authority_action);
  m_network_mode_group->addAction(m_golf_mode_action);
  m_fixed_delay_action->setChecked(true);

  m_game_digest_menu = m_menu_bar->addMenu(tr("Checksum"));
  m_game_digest_menu->addAction(tr("Current game"), this, [this] {
    Settings::Instance().GetNetPlayServer()->ComputeGameDigest(m_current_game_identifier);
  });
  m_game_digest_menu->addAction(tr("Other game..."), this, [this] {
    GameListDialog gld(m_game_list_model, this);

    if (gld.exec() != QDialog::Accepted)
      return;
    Settings::Instance().GetNetPlayServer()->ComputeGameDigest(
        gld.GetSelectedGame().GetSyncIdentifier());
  });
  m_game_digest_menu->addAction(tr("SD Card"), this, [] {
    Settings::Instance().GetNetPlayServer()->ComputeGameDigest(
        NetPlay::NetPlayClient::GetSDCardIdentifier());
  });

  m_other_menu = m_menu_bar->addMenu(tr("Other"));
  m_record_input_action = m_other_menu->addAction(tr("Record Inputs"));
  m_record_input_action->setCheckable(true);
  m_golf_mode_overlay_action = m_other_menu->addAction(tr("Show Golf Mode Overlay"));
  m_golf_mode_overlay_action->setCheckable(true);
  m_hide_remote_gbas_action = m_other_menu->addAction(tr("Hide Remote GBAs"));
  m_hide_remote_gbas_action->setCheckable(true);

  m_game_button->setDefault(false);
  m_game_button->setAutoDefault(false);

  m_savedata_load_only_action->setChecked(true);
  m_sync_codes_action->setChecked(true);

  m_main_layout->setMenuBar(m_menu_bar);

  m_main_layout->addWidget(m_game_button, 0, 0, 1, -1);
  m_main_layout->addWidget(m_splitter, 1, 0, 1, -1);

  m_splitter->addWidget(m_chat_box);
  m_splitter->addWidget(m_players_box);

  auto* options_widget = new QGridLayout;

  options_widget->addWidget(m_start_button, 0, 0, Qt::AlignVCenter);
  options_widget->addWidget(m_buffer_label, 0, 1, Qt::AlignVCenter);
  options_widget->addWidget(m_buffer_size_box, 0, 2, Qt::AlignVCenter);
  options_widget->addWidget(m_auto_buffer_box, 0, 3, Qt::AlignVCenter);
  options_widget->addWidget(m_quit_button, 0, 4, Qt::AlignVCenter | Qt::AlignRight);
  options_widget->setColumnStretch(4, 1000);

  m_main_layout->addLayout(options_widget, 2, 0, 1, -1, Qt::AlignRight);
  m_main_layout->setRowStretch(1, 1000);

  setLayout(m_main_layout);
}

void NetPlayDialog::CreateChatLayout()
{
  m_chat_box = new QGroupBox(tr("Chat"));
  m_chat_edit = new QTextBrowser;
  m_chat_type_edit = new QLineEdit;
  m_chat_send_button = new QPushButton(tr("Send"));

  // This button will get re-enabled when something gets entered into the chat box
  m_chat_send_button->setEnabled(false);
  m_chat_send_button->setDefault(false);
  m_chat_send_button->setAutoDefault(false);

  m_chat_edit->setReadOnly(true);

  auto* layout = new QGridLayout;

  // XD Netplay: a joiner can hand their own team to the host, which writes it
  // into the save it syncs at start. Hidden for the host (who edits its saves
  // directly in the team editor) and shown once connected as a client.
  m_submit_team_button = new QPushButton(tr("Submit Team..."));
  m_submit_team_button->setToolTip(
      tr("Send your own Showdown team to the host, so you play your own Pokémon.\n"
         "Applies to the next battle the host starts."));
  m_submit_team_button->setDefault(false);
  m_submit_team_button->setAutoDefault(false);
  m_submit_team_button->hide();

  layout->addWidget(m_chat_edit, 0, 0, 1, -1);
  layout->addWidget(m_chat_type_edit, 1, 0);
  layout->addWidget(m_chat_send_button, 1, 1);
  layout->addWidget(m_submit_team_button, 2, 0, 1, -1);

  m_chat_box->setLayout(layout);
}

void NetPlayDialog::CreatePlayersLayout()
{
  m_players_box = new QGroupBox(tr("Players"));
  m_room_box = new QComboBox;
  m_hostcode_label = new ClickBlurLabel;
  m_hostcode_action_button = new QPushButton(tr("Copy"));
  m_players_list = new QTableWidget;
  m_kick_button = new QPushButton(tr("Kick Player"));
  m_assign_ports_button = new QPushButton(tr("Assign Controller Ports"));

  m_players_list->setTabKeyNavigation(false);
  m_players_list->setColumnCount(5);
  m_players_list->verticalHeader()->hide();
  m_players_list->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_players_list->horizontalHeader()->setStretchLastSection(true);
  m_players_list->horizontalHeader()->setHighlightSections(false);

  for (int i = 0; i < 4; i++)
    m_players_list->horizontalHeader()->setSectionResizeMode(i, QHeaderView::ResizeToContents);

  auto* layout = new QGridLayout;

  layout->addWidget(m_room_box, 0, 0);
  layout->addWidget(m_hostcode_label, 0, 1);
  layout->addWidget(m_hostcode_action_button, 0, 2);
  layout->addWidget(m_players_list, 1, 0, 1, -1);
  layout->addWidget(m_kick_button, 2, 0, 1, -1);
  layout->addWidget(m_assign_ports_button, 3, 0, 1, -1);

  m_players_box->setLayout(layout);
}

void NetPlayDialog::ConnectWidgets()
{
  // Players
  connect(m_room_box, &QComboBox::currentIndexChanged, this, &NetPlayDialog::UpdateGUI);
  connect(m_hostcode_action_button, &QPushButton::clicked, [this] {
    if (m_is_copy_button_retry)
      Common::g_TraversalClient->ReconnectToServer();
    else
      QApplication::clipboard()->setText(m_hostcode_label->text());
  });
  connect(m_players_list, &QTableWidget::itemSelectionChanged, [this] {
    const int row = m_players_list->currentRow();
    m_kick_button->setEnabled(row > 0 &&
                              !m_players_list->currentItem()->data(Qt::UserRole).isNull());
  });
  connect(m_kick_button, &QPushButton::clicked, [this] {
    const auto id = m_players_list->currentItem()->data(Qt::UserRole).toInt();
    Settings::Instance().GetNetPlayServer()->KickPlayer(id);
  });
  connect(m_assign_ports_button, &QPushButton::clicked, [this] {
    m_pad_mapping->exec();

    Settings::Instance().GetNetPlayServer()->SetPadMapping(m_pad_mapping->GetGCPadArray());
    Settings::Instance().GetNetPlayServer()->SetGBAConfig(m_pad_mapping->GetGBAArray(), true);
    Settings::Instance().GetNetPlayServer()->SetWiimoteMapping(m_pad_mapping->GetWiimoteArray());
  });

  // Chat
  connect(m_chat_send_button, &QPushButton::clicked, this, &NetPlayDialog::OnChat);
  connect(m_chat_type_edit, &QLineEdit::returnPressed, this, &NetPlayDialog::OnChat);
  connect(m_submit_team_button, &QPushButton::clicked, this, &NetPlayDialog::OnSubmitTeam);
  connect(m_chat_type_edit, &QLineEdit::textChanged, this,
          [this] { m_chat_send_button->setEnabled(!m_chat_type_edit->text().isEmpty()); });

  // Other
  connect(m_buffer_size_box, &QSpinBox::valueChanged, [this](int value) {
    if (value == m_buffer_size)
      return;

    const auto client = Settings::Instance().GetNetPlayClient();
    const auto server = Settings::Instance().GetNetPlayServer();
    if (server && !m_host_input_authority)
    {
      // A hand-typed value always wins over the automatic sizer, which would
      // otherwise overwrite it within a few seconds. Untick the box first so
      // the host can SEE why auto stopped -- SetPadBufferSizeManual would turn
      // it off silently otherwise.
      if (m_auto_buffer_box->isChecked())
        m_auto_buffer_box->setChecked(false);
      server->SetPadBufferSizeManual(value);
    }
    else
    {
      client->AdjustPadBufferSize(value);
    }
  });

  connect(m_auto_buffer_box, &QCheckBox::toggled, this, [this](bool checked) {
    if (const auto server = Settings::Instance().GetNetPlayServer())
      server->SetAutoPadBufferEnabled(checked);
  });

  const auto hia_function = [this](bool enable) {
    if (m_host_input_authority != enable)
    {
      const auto server = Settings::Instance().GetNetPlayServer();
      if (server)
        server->SetHostInputAuthority(enable);
    }
  };

  connect(m_host_input_authority_action, &QAction::toggled, this,
          [hia_function] { hia_function(true); });
  connect(m_golf_mode_action, &QAction::toggled, this, [hia_function] { hia_function(true); });
  connect(m_fixed_delay_action, &QAction::toggled, this, [hia_function] { hia_function(false); });

  connect(m_start_button, &QPushButton::clicked, this, &NetPlayDialog::OnStart);
  connect(m_quit_button, &QPushButton::clicked, this, &NetPlayDialog::reject);

  connect(m_game_button, &QPushButton::clicked, [this] {
    GameListDialog gld(m_game_list_model, this);
    if (gld.exec() == QDialog::Accepted)
    {
      Settings& settings = Settings::Instance();

      const UICommon::GameFile& game = gld.GetSelectedGame();
      const std::string netplay_name = m_game_list_model.GetNetPlayName(game);

      settings.GetNetPlayServer()->ChangeGame(game.GetSyncIdentifier(), netplay_name);
      Settings::GetQSettings().setValue(QStringLiteral("netplay/hostgame"),
                                        QString::fromStdString(netplay_name));
    }
  });

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State state) {
    if (isVisible())
    {
      GameStatusChanged(state != Core::State::Uninitialized);
      if ((state == Core::State::Uninitialized || state == Core::State::Stopping) &&
          !m_got_stop_request)
      {
        Settings::Instance().GetNetPlayClient()->RequestStopGame();
      }
      if (state == Core::State::Uninitialized)
        DisplayMessage(tr("Stopped game"), "red");
    }
  });

  // SaveSettings() - Save Hosting-Dialog Settings

  connect(m_buffer_size_box, &QSpinBox::valueChanged, this, &NetPlayDialog::SaveSettings);
  connect(m_auto_buffer_box, &QCheckBox::toggled, this, &NetPlayDialog::SaveSettings);
  connect(m_savedata_none_action, &QAction::toggled, this, &NetPlayDialog::SaveSettings);
  connect(m_savedata_load_only_action, &QAction::toggled, this, &NetPlayDialog::SaveSettings);
  connect(m_savedata_load_and_write_action, &QAction::toggled, this, &NetPlayDialog::SaveSettings);
  connect(m_savedata_all_wii_saves_action, &QAction::toggled, this, &NetPlayDialog::SaveSettings);
  connect(m_sync_codes_action, &QAction::toggled, this, &NetPlayDialog::SaveSettings);
  connect(m_record_input_action, &QAction::toggled, this, &NetPlayDialog::SaveSettings);
  connect(m_strict_settings_sync_action, &QAction::toggled, this, &NetPlayDialog::SaveSettings);
  connect(m_host_input_authority_action, &QAction::toggled, this, &NetPlayDialog::SaveSettings);
  connect(m_golf_mode_action, &QAction::toggled, this, &NetPlayDialog::SaveSettings);
  connect(m_golf_mode_overlay_action, &QAction::toggled, this, &NetPlayDialog::SaveSettings);
  connect(m_fixed_delay_action, &QAction::toggled, this, &NetPlayDialog::SaveSettings);
  connect(m_hide_remote_gbas_action, &QAction::toggled, this, &NetPlayDialog::SaveSettings);
}

void NetPlayDialog::SendMessage(const std::string& msg)
{
  Settings::Instance().GetNetPlayClient()->SendChatMessage(msg);

  DisplayMessage(
      QStringLiteral("%1: %2").arg(QString::fromStdString(m_nickname), QString::fromStdString(msg)),
      "");
}

void NetPlayDialog::OnChat()
{
  QueueOnObject(this, [this] {
    const auto msg = m_chat_type_edit->text().toStdString();

    if (msg.empty())
      return;

    m_chat_type_edit->clear();

    SendMessage(msg);
  });
}

void NetPlayDialog::OnIndexAdded(bool success, const std::string error)
{
  DisplayMessage(success ? tr("Successfully added to the NetPlay index") :
                           tr("Failed to add this session to the NetPlay index: %1")
                               .arg(QString::fromStdString(error)),
                 success ? "green" : "red");
}

void NetPlayDialog::OnIndexRefreshFailed(const std::string error)
{
  DisplayMessage(QString::fromStdString(error), "red");
}

void NetPlayDialog::OnStart()
{
  if (!Settings::Instance().GetNetPlayClient()->DoAllPlayersHaveGame())
  {
    if (ModalMessageBox::question(
            this, tr("Warning"),
            tr("Not all players have the game. Do you really want to start?")) == QMessageBox::No)
      return;
  }

  if (m_strict_settings_sync_action->isChecked() && Config::Get(Config::GFX_EFB_SCALE) == 0)
  {
    ModalMessageBox::critical(
        this, tr("Error"),
        tr("Auto internal resolution is not allowed in strict sync mode, as it depends on window "
           "size.\n\nPlease select a specific internal resolution."));
    return;
  }

  const auto game = FindGameFile(m_current_game_identifier);
  if (!game)
  {
    PanicAlertFmtT("Selected game doesn't exist in game list!");
    return;
  }

  // When hosting Pokemon XD, force the fixed two-player GBA layout (host pad +
  // host GBA + guest GBA) and the session settings the link needs before the
  // start request. Non-XD sessions are completely untouched.
  if (const auto server = Settings::Instance().GetNetPlayServer();
      server && XDNetplay::IsXdGameId(game->GetGameID()))
  {
    XDNetplay::ApplyStartForcing(server.get());
  }

  if (Settings::Instance().GetNetPlayServer()->RequestStartGame())
    SetOptionsEnabled(false);
}

void NetPlayDialog::reject()
{
  if (ModalMessageBox::question(this, tr("Confirmation"),
                                tr("Are you sure you want to quit NetPlay?")) == QMessageBox::Yes)
  {
    QDialog::reject();
  }
}

void NetPlayDialog::show(std::string nickname, bool use_traversal)
{
  // The room is open from here (host and joiner alike; the one call site is
  // MainWindow's room creation): mark the netplay session active so boundary
  // heals (launcher showEvent, team editor open) know the guest-team stashes
  // and the disposable-save swap now belong to a LIVE room. OnRoomClosed's
  // EndSession is the matching clear.
  XDNetplay::BattleCustomizer::BeginSession();

  m_nickname = std::move(nickname);
  m_use_traversal = use_traversal;
  m_buffer_size = 0;
  m_old_player_count = 0;

  m_room_box->clear();
  m_chat_edit->clear();
  m_chat_type_edit->clear();

  const bool is_hosting = Settings::Instance().GetNetPlayServer() != nullptr;

  if (is_hosting)
  {
    if (use_traversal)
      m_room_box->addItem(tr("Room ID"));
    m_room_box->addItem(tr("External"));

    for (const auto& iface : Settings::Instance().GetNetPlayServer()->GetInterfaceSet())
    {
      const auto interface = QString::fromStdString(iface);
      m_room_box->addItem(iface == "!local!" ? tr("Local") : interface, interface);
    }
  }

  m_data_menu->menuAction()->setVisible(is_hosting);
  m_network_menu->menuAction()->setVisible(is_hosting);
  m_game_digest_menu->menuAction()->setVisible(is_hosting);
#ifdef HAS_LIBMGBA
  m_hide_remote_gbas_action->setVisible(is_hosting);
#else
  m_hide_remote_gbas_action->setVisible(false);
#endif
  // Only a joiner submits a team; the host edits its own saves directly.
  m_submit_team_button->setHidden(is_hosting);
  m_start_button->setHidden(!is_hosting);
  // Only the host owns the session buffer, so only the host gets the Auto box.
  // OnHostInputAuthorityChanged refines this once the mode is known.
  m_auto_buffer_box->setHidden(!is_hosting);
  m_kick_button->setHidden(!is_hosting);
  m_assign_ports_button->setHidden(!is_hosting);
  m_room_box->setHidden(!is_hosting);
  m_hostcode_label->setHidden(!is_hosting);
  m_hostcode_action_button->setHidden(!is_hosting);
  m_game_button->setEnabled(is_hosting);
  m_kick_button->setEnabled(false);

  SetOptionsEnabled(true);

  QDialog::show();
  UpdateGUI();
}

void NetPlayDialog::ResetExternalIP()
{
  m_external_ip_address =
      Common::Lazy<std::string>([]() -> std::string { return NetPlay::GetExternalIPAddress(); });
}

void NetPlayDialog::UpdateDiscordPresence()
{
#ifdef USE_DISCORD_PRESENCE
  // both m_current_game and m_player_count need to be set for the status to be displayed correctly
  if (m_player_count == 0 || m_current_game_name.empty())
    return;

  const auto use_default = [this] {
    Discord::UpdateDiscordPresence(m_player_count, Discord::SecretType::Empty, "",
                                   m_current_game_name);
  };

  if (Core::IsRunning(Core::System::GetInstance()))
    return use_default();

  if (IsHosting())
  {
    if (Common::g_TraversalClient)
    {
      const auto host_id = Common::g_TraversalClient->GetHostID();
      if (host_id[0] == '\0')
        return use_default();

      Discord::UpdateDiscordPresence(m_player_count, Discord::SecretType::RoomID,
                                     std::string(host_id.begin(), host_id.end()),
                                     m_current_game_name);
    }
    else
    {
      if (m_external_ip_address->empty())
        return use_default();
      const int port = Settings::Instance().GetNetPlayServer()->GetPort();

      Discord::UpdateDiscordPresence(
          m_player_count, Discord::SecretType::IPAddress,
          Discord::CreateSecretFromIPAddress(*m_external_ip_address, port), m_current_game_name);
    }
  }
  else
  {
    use_default();
  }
#endif
}

void NetPlayDialog::UpdateGUI()
{
  const auto client = Settings::Instance().GetNetPlayClient();
  const auto server = Settings::Instance().GetNetPlayServer();
  if (!client)
    return;

  // Update Player List
  const auto players = client->GetPlayers();

  if (static_cast<int>(players.size()) != m_player_count && m_player_count != 0)
    QApplication::alert(this);

  m_player_count = static_cast<int>(players.size());

  const int selection_pid = m_players_list->currentItem() ?
                                m_players_list->currentItem()->data(Qt::UserRole).toInt() :
                                -1;

  m_players_list->clear();
  m_players_list->setHorizontalHeaderLabels(
      {tr("Player"), tr("Game Status"), tr("Ping"), tr("Mapping"), tr("Revision")});
  m_players_list->setRowCount(m_player_count);

  static const std::map<NetPlay::SyncIdentifierComparison, std::pair<QString, QString>>
      player_status{
          {NetPlay::SyncIdentifierComparison::SameGame, {tr("OK"), tr("OK")}},
          {NetPlay::SyncIdentifierComparison::DifferentHash,
           {tr("Wrong hash"),
            tr("Game file has a different hash; right-click it, select Properties, switch to the "
               "Verify tab, and select Verify Integrity to check the hash")}},
          {NetPlay::SyncIdentifierComparison::DifferentDiscNumber,
           {tr("Wrong disc number"), tr("Game has a different disc number")}},
          {NetPlay::SyncIdentifierComparison::DifferentRevision,
           {tr("Wrong revision"), tr("Game has a different revision")}},
          {NetPlay::SyncIdentifierComparison::DifferentRegion,
           {tr("Wrong region"), tr("Game region does not match")}},
          {NetPlay::SyncIdentifierComparison::DifferentGame,
           {tr("Not found"), tr("No matching game was found")}},
      };

  for (int i = 0; i < m_player_count; i++)
  {
    const auto* p = players[i];

    auto* name_item = new QTableWidgetItem(QString::fromStdString(p->name));
    name_item->setToolTip(name_item->text());
    const auto it = player_status.find(p->game_status);
    const auto& status_info = it != player_status.end() ?
                                  it->second :
                                  std::make_pair(QStringLiteral("?"), QStringLiteral("?"));
    auto* status_item = new QTableWidgetItem(status_info.first);
    status_item->setToolTip(status_info.second);
    auto* ping_item = new QTableWidgetItem(QStringLiteral("%1 ms").arg(p->ping));
    ping_item->setToolTip(ping_item->text());
    auto* mapping_item =
        new QTableWidgetItem(QString::fromStdString(NetPlay::GetPlayerMappingString(
            p->pid, client->GetPadMapping(), client->GetGBAConfig(), client->GetWiimoteMapping())));
    mapping_item->setToolTip(mapping_item->text());
    auto* revision_item = new QTableWidgetItem(QString::fromStdString(p->revision));
    revision_item->setToolTip(revision_item->text());

    for (auto* item : {name_item, status_item, ping_item, mapping_item, revision_item})
    {
      item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
      item->setData(Qt::UserRole, static_cast<int>(p->pid));
    }

    m_players_list->setItem(i, 0, name_item);
    m_players_list->setItem(i, 1, status_item);
    m_players_list->setItem(i, 2, ping_item);
    m_players_list->setItem(i, 3, mapping_item);
    m_players_list->setItem(i, 4, revision_item);

    if (p->pid == selection_pid)
      m_players_list->selectRow(i);
  }

  if (m_old_player_count != m_player_count)
  {
    UpdateDiscordPresence();
    m_old_player_count = m_player_count;
  }

  if (!server)
    return;

  const bool is_local_ip_selected = m_room_box->currentIndex() > (m_use_traversal ? 1 : 0);
  if (is_local_ip_selected)
  {
    m_hostcode_label->setText(QString::fromStdString(
        server->GetInterfaceHost(m_room_box->currentData().toString().toStdString())));
    m_hostcode_action_button->setEnabled(true);
    m_hostcode_action_button->setText(tr("Copy"));
    m_is_copy_button_retry = false;
  }
  else if (m_use_traversal)
  {
    switch (Common::g_TraversalClient->GetState())
    {
    case Common::TraversalClient::State::Connecting:
      m_hostcode_label->setText(tr("Connecting"));
      m_hostcode_action_button->setEnabled(false);
      m_hostcode_action_button->setText(tr("..."));
      break;
    case Common::TraversalClient::State::Connected:
    {
      if (m_room_box->currentIndex() == 0)
      {
        // Display Room ID.
        const auto host_id = Common::g_TraversalClient->GetHostID();
        m_hostcode_label->setText(
            QString::fromStdString(std::string(host_id.begin(), host_id.end())));
      }
      else
      {
        // Externally mapped IP and port are known when using the traversal server.
        m_hostcode_label->setText(
            InetAddressToString(Common::g_TraversalClient->GetExternalAddress()));
      }

      m_hostcode_action_button->setEnabled(true);
      m_hostcode_action_button->setText(tr("Copy"));
      m_is_copy_button_retry = false;
      break;
    }
    case Common::TraversalClient::State::Failure:
      m_hostcode_label->setText(tr("Error"));
      m_hostcode_action_button->setText(tr("Retry"));
      m_hostcode_action_button->setEnabled(true);
      m_is_copy_button_retry = true;
      break;
    }
  }
  else
  {
    // Display External IP.
    if (!m_external_ip_address->empty())
    {
      const int port = Settings::Instance().GetNetPlayServer()->GetPort();
      m_hostcode_label->setText(QStringLiteral("%1:%2").arg(
          QString::fromStdString(*m_external_ip_address), QString::number(port)));
      m_hostcode_action_button->setEnabled(true);
    }
    else
    {
      m_hostcode_label->setText(tr("Unknown"));
      m_hostcode_action_button->setEnabled(false);
    }

    m_hostcode_action_button->setText(tr("Copy"));
    m_is_copy_button_retry = false;
  }
}

// NetPlayUI methods

void NetPlayDialog::BootGame(const std::string& filename,
                             std::unique_ptr<BootSessionData> boot_session_data)
{
  m_got_stop_request = false;
  m_start_game_callback(filename, std::move(boot_session_data));
}

void NetPlayDialog::StopGame()
{
  if (m_got_stop_request)
    return;

  m_got_stop_request = true;
  emit Stop();
}

bool NetPlayDialog::IsHosting() const
{
  return Settings::Instance().GetNetPlayServer() != nullptr;
}

void NetPlayDialog::Update()
{
  QueueOnObject(this, &NetPlayDialog::UpdateGUI);
}

void NetPlayDialog::DisplayMessage(const QString& msg, const std::string& color, int duration)
{
  QueueOnObject(m_chat_edit, [this, color, msg] {
    m_chat_edit->append(QStringLiteral("<font color='%1'>%2</font>")
                            .arg(QString::fromStdString(color), msg.toHtmlEscaped()));
  });

  const QColor c(color.empty() ? QStringLiteral("white") : QString::fromStdString(color));

  if (Config::Get(Config::GFX_SHOW_NETPLAY_MESSAGES) &&
      Core::IsRunning(Core::System::GetInstance()))
  {
    g_netplay_chat_ui->AppendChat(msg.toStdString(),
                                  {static_cast<float>(c.redF()), static_cast<float>(c.greenF()),
                                   static_cast<float>(c.blueF())});
  }
}

void NetPlayDialog::AppendChat(const std::string& msg)
{
  DisplayMessage(QString::fromStdString(msg), "");
  QApplication::alert(this);
}

std::string NetPlayDialog::OnTeamSubmission(const std::string& player, const std::string& text)
{
  // Host side, NETPLAY thread. The write must finish before we return: the
  // caller acks only afterwards, so the file is on disk before any Start can
  // read it. Touches no widgets -- the caller relays the text into chat.
  //
  // The payload may carry an in-game trainer name ahead of the team; a client
  // that predates that just sends the bare team (TeamInjector.h documents the
  // grammar and both compatibility directions).
  const XDNetplay::TeamSubmission submission = XDNetplay::ParseTeamSubmissionPayload(text);

  // The submission may also carry the guest's cosmetic "Model:" pick. Stash it
  // (every submission overwrites the stash; absent or invalid means "no
  // preference" and the host's Guest-model fallback dropdown wins) and rebuild
  // the "$OrreLink Battle Style" block right away, so a model submitted any
  // time before Start is already in the file SyncCodes reads. The server
  // rejects TeamData while a battle runs, so this cannot race a synced set.
  XDNetplay::BattleCustomizer::SetGuestModel(submission.model);
  XDNetplay::BattleCustomizer::RegenerateFromConfig(nullptr);

  // A payload is EITHER a party bundle (the guest's own save's team and
  // trainer identity, validated strictly before anything is written) OR
  // Showdown text; the Name header is ignored for bundles -- the bundle's
  // real trainer name wins (TeamInjector.h).
  std::string status;
  const bool applied =
      submission.save_bundle ?
          XDNetplay::InjectGuestBundle(*submission.save_bundle, 2, &status) :
          XDNetplay::InjectGuestTeam(submission.showdown_text, submission.trainer_name, 2, &status);
  if (!applied)
    return status.empty() ? std::string{"team not applied"} : "team not applied - " + status;
  return status;
}

void NetPlayDialog::OnRoomClosed()
{
  // Restores the host's own team AND erases every remaining copy of the
  // opponent's party (.bak/.tmp beside the save, netplay's NetPlayTemp GBA
  // saves). May defer itself until emulation has fully stopped -- the mGBA
  // core rewrites the save at teardown, so anything done sooner is undone.
  XDNetplay::RestoreHostTeam(2);
  // And retire the session's cosmetic battle-style state: drop the guest's
  // stashed model, strip the generated "$OrreLink Battle Style" block from the
  // local GXXE01.ini, and give the cheats flag back if the pre-start hook
  // forced it on for a cosmetics-only session.
  XDNetplay::BattleCustomizer::EndSession();
  // Finally, give a hosting machine its imported saves back: the session ran
  // on disposables (party + trainer identity only) that MainWindow::
  // NetPlayHost swapped in so the full imports never entered the synced save
  // path. MUST stay after RestoreHostTeam -- the ordering is what guarantees
  // the outer import restore runs after the inner .hostteam restore on the
  // same deferred-until-Uninitialized event (DisposableSave.h). Idempotent
  // and harmless on joiners and non-imported hosts.
  XDNetplay::DisposableSave::EndNetplaySession();
}

void NetPlayDialog::OnSubmitTeam()
{
  // Joiner side. A pokepast.es link is resolved HERE, on the submitting
  // client, so the host only ever parses plain text it was handed -- it never
  // fetches a URL a stranger chose.
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Submit Team"));

  auto* dialog_layout = new QVBoxLayout(&dialog);
  dialog_layout->addWidget(new QLabel(
      tr("Paste a Showdown team export, or a pokepast.es link.\n"
         "The host writes it into the save you'll play with."),
      &dialog));

  // In-game name, pre-filled with the netplay nickname so the common case is
  // zero typing. Sanitized to what a Gen 3 save can actually hold (7
  // characters from its own charset) BEFORE it is shown, so what the field
  // says is what the opponent will see -- no silent surprise later.
  auto* name_layout = new QHBoxLayout;
  auto* name_edit = new QLineEdit(
      QString::fromStdString(XDNetplay::EmeraldSave::SanitizeTrainerName(
          Config::Get(Config::NETPLAY_NICKNAME))),
      &dialog);
  name_edit->setMaxLength(
      static_cast<int>(XDNetplay::EmeraldSave::TRAINER_NAME_LEN));
  name_edit->setPlaceholderText(tr("In-game name"));
  name_layout->addWidget(new QLabel(tr("In-game name (max 7):"), &dialog));
  name_layout->addWidget(name_edit, 1);
  dialog_layout->addLayout(name_layout);

  // Cosmetic trainer-model pick (battle style feature). It rides the same
  // payload as the team, as a "Model:" header line, and the HOST validates it
  // against its own copy of this table before assembling the synced AR block.
  // "No preference" sends no header at all -- the host's "Guest model"
  // fallback dropdown then decides. Every model is field-proven in battle, so
  // no tier presentation here; the one caveat a pick can carry is having no
  // pre-rendered bust, said in the label as "(no portrait)" and explained by
  // the caption under the combo.
  auto* model_layout = new QHBoxLayout;
  auto* model_combo = new QComboBox(&dialog);
  model_combo->addItem(tr("No preference"), 0);
  for (const XDNetplay::BattleCustomizer::StyleOption& option :
       XDNetplay::BattleCustomizer::ModelTable())
  {
    const QString name = QString::fromUtf8(option.name);
    model_combo->addItem(XDNetplay::BattleCustomizer::ModelHasPortrait(option.id) ?
                             name :
                             tr("%1 (no portrait)").arg(name),
                         option.id);
  }
  model_layout->addWidget(new QLabel(tr("Trainer model:"), &dialog));
  model_layout->addWidget(model_combo, 1);
  dialog_layout->addLayout(model_layout);
  auto* portrait_note = new QLabel(
      tr("Models marked \"(no portrait)\" show no close-up on the connection and team "
         "screens — the battle model still changes."),
      &dialog);
  portrait_note->setWordWrap(true);
  dialog_layout->addWidget(portrait_note);

  // "Use my save": submit the party from the player's OWN local save (the
  // port-2 slot the Team Editor's "Host" role edits -- their slot regardless
  // of which side of a room they join) instead of a Showdown paste. Community
  // design by the community, adapted. The real mon bytes travel as-is and the save's
  // trainer identity travels with them; that identity match is what keeps the
  // party obedient (see PartyBundle.h), so the in-game name field above is
  // IGNORED in this mode and greyed out below to say so. The model pick stays
  // meaningful either way: it rides the payload as its own header.
  auto* use_save_check =
      new QCheckBox(tr("Use my save (send the party from my own save file — the Team Editor's "
                       "\"Host — GBA port 2\" team)"),
                    &dialog);
  dialog_layout->addWidget(use_save_check);

  // Shown only while the box is ticked. The disclosure list is deliberately
  // exhaustive (community review): the bundle carries the real party bytes and
  // the save's trainer identity, so everything in them lands on the host's
  // machine -- including the secret trainer ID, which nothing in a battle ever
  // shows. Saying "nicknames, OT, IDs" and stopping there would undersell it.
  auto* privacy_note = new QLabel(
      tr("Your save's party is sent exactly as it is, under the save's own trainer "
         "identity — the in-game name field above is ignored. The host's machine "
         "receives everything in those party bytes: exact moves, held items, stats, "
         "EVs, IVs, natures, nicknames, met information, ribbons, and your OT name "
         "and BOTH trainer IDs, including the secret one. Your opponent would see "
         "most of this by battling you — but not the secret ID, so only send your "
         "save to hosts you trust."),
      &dialog);
  privacy_note->setWordWrap(true);
  dialog_layout->addWidget(privacy_note);

  auto* team_edit = new QPlainTextEdit(&dialog);
  team_edit->setPlaceholderText(tr("Showdown export or pokepast.es link"));
  dialog_layout->addWidget(team_edit, 1);

  // Paste-time FORMAT feedback (FormatRules.h): with the LOCAL Format pick on
  // Orre Colosseum, a live note under the paste box says when the team about
  // to be sent breaks that ruleset -- the same wording the Team Editor uses --
  // so a violation is learned here, not from the host's refusal arriving in
  // room chat. NEVER blocking: Send stays enabled whatever the note says,
  // because only the HOST's format governs a room and this host may well be
  // running Free. Nothing here can check what the host runs; the note is
  // keyed off the local pick as agreed feedback, not enforcement. A
  // pokepast.es link parses to no sets, so it draws no note (checking it
  // would mean fetching it -- the send path resolves links later anyway).
  // With the local pick on Free none of this exists: no label in the layout,
  // no parse, no validation call, ever.
  if (const int local_format = Config::Get(Config::MAIN_XD_FORMAT);
      XDNetplay::FormatRules::HasTeamRules(local_format))
  {
    // Game data loads once per dialog open; on failure (broken install) the
    // note simply never appears -- feedback is a courtesy, never a judge.
    std::shared_ptr<const XDNetplay::Gen3Data> format_data;
    if (auto loaded = XDNetplay::Gen3Data::LoadBundled())
      format_data = std::make_shared<const XDNetplay::Gen3Data>(std::move(*loaded));
    if (format_data)
    {
      auto* format_note = new QLabel(&dialog);
      format_note->setWordWrap(true);
      format_note->hide();
      dialog_layout->addWidget(format_note);

      // Everything is captured by value (widget pointers parented to the
      // stack dialog, plus the shared_ptr); the connections die with the
      // dialog, so nothing here outlives what it touches.
      const auto update_format_note = [format_note, team_edit, use_save_check, format_data,
                                       local_format] {
        QString reason;
        if (use_save_check->isChecked())
        {
#ifdef HAS_LIBMGBA
          // Bundle mode sends the port-2 save's party: validate those actual
          // bytes, resolved exactly as the send path below resolves them.
          // Any read problem stays silent here -- the send path owns those
          // errors and words them better.
          std::string rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[1]);
          if (rom.empty() || !File::Exists(rom))
            rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[2]);
          if (!rom.empty() && File::Exists(rom))
          {
            const std::string save_path = HW::GBA::Core::GetSavePath(rom, 1);
            if (File::Exists(save_path))
            {
              if (const auto save = XDNetplay::LoadSaveFile(save_path))
              {
                if (const auto party = save->ReadParty())
                {
                  const XDNetplay::FormatRules::Verdict verdict =
                      XDNetplay::FormatRules::ValidateParty(local_format, *party, *format_data);
                  if (!verdict.ok)
                    reason = QString::fromStdString(verdict.reason);
                }
              }
            }
          }
#endif
        }
        else
        {
          const XDNetplay::FormatRules::Verdict verdict = XDNetplay::FormatRules::ValidateSets(
              local_format,
              XDNetplay::ShowdownParser::ParseTeam(team_edit->toPlainText().toStdString()),
              *format_data);
          if (!verdict.ok)
            reason = QString::fromStdString(verdict.reason);
        }
        if (!reason.isEmpty())
        {
          format_note->setText(
              tr("note: this team is not %1 legal - %2")
                  .arg(QString::fromUtf8(
                      XDNetplay::FormatRules::FormatDisplayName(local_format)))
                  .arg(reason));
        }
        format_note->setVisible(!reason.isEmpty());
      };
      // No initial call needed: right now the box is empty and unchecked (the
      // no-note state), and the prefill below fires these same signals for
      // anything it restores -- textChanged on the stored paste, toggled on a
      // stored bundle preference.
      connect(team_edit, &QPlainTextEdit::textChanged, &dialog, update_format_note);
      connect(use_save_check, &QCheckBox::toggled, &dialog, update_format_note);
    }
  }

  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Send"));
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  dialog_layout->addWidget(buttons);

  // Bundle mode repurposes the dialog: the save supplies both party and
  // identity, so the paste box and the name field would be dead text. Grey
  // them out rather than let the user type something that will not be sent.
  const auto apply_bundle_mode = [name_edit, team_edit, privacy_note](bool bundle_mode) {
    name_edit->setEnabled(!bundle_mode);
    team_edit->setEnabled(!bundle_mode);
    privacy_note->setVisible(bundle_mode);
  };
  connect(use_save_check, &QCheckBox::toggled, &dialog, apply_bundle_mode);

  // Prefill every field from the previous successful submission (stored on
  // send, below) so a returning player clicks Send instead of retyping. The
  // team text is stored base64-encoded -- Dolphin's INI config layer is
  // line-based and a Showdown export is multi-line -- and the strict decoder
  // rejects the never-stored "" cleanly, leaving the box empty. A stored name
  // is re-sanitized on the way in, preserving this dialog's invariant that
  // the field shows exactly what the opponent will see; when nothing is
  // stored the netplay-nickname default seeded above stays. A stored model id
  // the table no longer carries falls back to "No preference", the same idiom
  // as the launcher's Battle Style combos.
  if (const auto stored_team = XDNetplay::PartyBundle::Base64Decode(
          Config::Get(Config::MAIN_XD_SUBMIT_TEAM_B64)))
  {
    team_edit->setPlainText(QString::fromUtf8(
        reinterpret_cast<const char*>(stored_team->data()), static_cast<int>(stored_team->size())));
  }
  const std::string stored_name = Config::Get(Config::MAIN_XD_SUBMIT_NAME);
  if (!stored_name.empty())
  {
    name_edit->setText(
        QString::fromStdString(XDNetplay::EmeraldSave::SanitizeTrainerName(stored_name)));
  }
  const int stored_model_index = model_combo->findData(Config::Get(Config::MAIN_XD_SUBMIT_MODEL));
  model_combo->setCurrentIndex(stored_model_index >= 0 ? stored_model_index : 0);
  use_save_check->setChecked(Config::Get(Config::MAIN_XD_SUBMIT_USE_SAVE));
#ifndef HAS_LIBMGBA
  // Extracting a bundle means reading the local GBA save, and everything that
  // knows where that lives is compiled out with libmgba. Overrides the stored
  // preference on purpose: a checked-but-unusable box could not be honored.
  use_save_check->setChecked(false);
  use_save_check->setEnabled(false);
  use_save_check->setToolTip(tr("GBA support (libmgba) is not compiled into this build."));
#endif
  // setChecked only signals on a CHANGE, so a false-to-false prefill would
  // leave the privacy note visible; normalize explicitly once.
  apply_bundle_mode(use_save_check->isChecked());

  // Sized for the (longer, deliberately exhaustive) privacy note when bundle
  // mode is on; the note is hidden otherwise and the paste box absorbs the
  // extra height.
  dialog.resize(500, 520);
  if (dialog.exec() != QDialog::Accepted)
    return;

  const bool use_save = use_save_check->isChecked();
  const QString text = team_edit->toPlainText().trimmed();
  if (text.isEmpty() && !use_save)
    return;

  // Sanitize again on the way out: the field is a QLineEdit, so the user can
  // still have typed something the Gen 3 charset has no byte for. An empty
  // result simply means "no rename" -- the host keeps its own save's name.
  const std::string trainer_name = XDNetplay::EmeraldSave::SanitizeTrainerName(
      name_edit->text().toStdString());
  // The warning is meaningless in bundle mode -- the field is ignored there
  // (and was greyed out saying so), not "sent without".
  if (!use_save && trainer_name.empty() && !name_edit->text().trimmed().isEmpty())
  {
    DisplayMessage(
        tr("That in-game name has no Gen 3 equivalent — sending the team without it."), "");
  }

  // 0 is the "No preference" entry: BuildTeamSubmissionPayload emits no
  // "Model:" header for nullopt, which is byte-identical to the pre-feature
  // payload. Snapshot the value now -- the pokepaste path's deferred lambdas
  // below run after this function (and the stack dialog) are gone, so they
  // must capture the plain int, never the combo.
  const int model_id = model_combo->currentData().toInt();
  const std::optional<int> model =
      model_id > 0 ? std::optional<int>(model_id) : std::nullopt;

  // Persist what was submitted so the next open prefills (read back above).
  // Called only once a submission actually goes out -- an empty paste or a
  // save that would not extract leaves the previous session's values alone.
  // Everything is captured by value: the pokepaste path calls this after the
  // dialog's locals would otherwise be gone. Same SetBaseOrCurrent + Save()
  // idiom as the launcher's Battle Style keys.
  const std::string team_text = text.toStdString();
  const auto persist_submission = [team_text, trainer_name, model_id, use_save] {
    Config::SetBaseOrCurrent(
        Config::MAIN_XD_SUBMIT_TEAM_B64,
        XDNetplay::PartyBundle::Base64Encode(std::vector<u8>(team_text.begin(), team_text.end())));
    Config::SetBaseOrCurrent(Config::MAIN_XD_SUBMIT_NAME, trainer_name);
    Config::SetBaseOrCurrent(Config::MAIN_XD_SUBMIT_MODEL, model_id);
    Config::SetBaseOrCurrent(Config::MAIN_XD_SUBMIT_USE_SAVE, use_save);
    Config::Save();
  };

  if (use_save)
  {
#ifdef HAS_LIBMGBA
    // Read the same save the Team Editor's "Host — GBA port 2" role edits:
    // the player's own local slot (imported or editor-built), resolved with
    // the editor's exact ROM-then-fallback logic. No template fallback here,
    // unlike the editor: a fresh template holds an empty party, and Extract
    // would only refuse it with a less helpful message than ours.
    std::string rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[1]);
    if (rom.empty() || !File::Exists(rom))
      rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[2]);
    if (rom.empty() || !File::Exists(rom))
    {
      DisplayMessage(tr("No Emerald ROM configured — run the launcher checklist first."), "");
      return;
    }
    const std::string save_path = HW::GBA::Core::GetSavePath(rom, 1);
    if (!File::Exists(save_path))
    {
      DisplayMessage(tr("No save to send — import one or build a team in the Team Editor first."),
                     "");
      return;
    }
    std::string error;
    const std::optional<XDNetplay::EmeraldSave> save = XDNetplay::LoadSaveFile(save_path, &error);
    if (!save)
    {
      DisplayMessage(tr("Could not open your save: %1").arg(QString::fromStdString(error)), "");
      return;
    }
    // Extract refuses FRLG saves and empty parties itself, with reasons meant
    // for exactly this status line.
    const std::optional<std::vector<u8>> bundle = XDNetplay::PartyBundle::Extract(*save, &error);
    if (!bundle)
    {
      DisplayMessage(tr("Could not use your save: %1").arg(QString::fromStdString(error)), "");
      return;
    }
    // No Name header rides with a bundle -- the save's own identity wins
    // (BuildBundleSubmissionPayload never emits one). Model still does.
    Settings::Instance().GetNetPlayClient()->SendTeamSubmission(
        XDNetplay::BuildBundleSubmissionPayload(*bundle, model));
    persist_submission();
    DisplayMessage(tr("Your save's team was sent to the host."), "");
#endif
    return;
  }

  const QRegularExpression pokepaste_re(
      QStringLiteral("^https?://pokepast\\.es/[A-Za-z0-9]+"));
  const QRegularExpressionMatch match = pokepaste_re.match(text);
  if (!match.hasMatch())
  {
    Settings::Instance().GetNetPlayClient()->SendTeamSubmission(
        XDNetplay::BuildTeamSubmissionPayload(text.toStdString(), trainer_name, model));
    persist_submission();
    DisplayMessage(tr("Team sent to the host."), "");
    return;
  }

  const std::string url = match.captured(0).toStdString() + "/raw";
  DisplayMessage(tr("Fetching %1…").arg(match.captured(0)), "");
  // The fetch is asynchronous and the submission is committed from the user's
  // point of view, so persist NOW rather than in the deferred lambda: if a
  // flaky network fails the fetch, the link prefilling on the next open is
  // exactly what the retry wants.
  persist_submission();
  QPointer<NetPlayDialog> self(this);
  std::thread([self, url, trainer_name, model] {
    Common::HttpRequest request;
    request.FollowRedirects();
    Common::HttpRequest::Response response = request.Get(url);
    if (!self)
      return;
    QueueOnObject(self.data(), [self, trainer_name, model, response = std::move(response)] {
      if (!self)
        return;
      if (!response)
      {
        self->DisplayMessage(tr("Could not fetch that paste (network error)."), "");
        return;
      }
      if (auto client = Settings::Instance().GetNetPlayClient())
      {
        client->SendTeamSubmission(XDNetplay::BuildTeamSubmissionPayload(
            std::string(response->begin(), response->end()), trainer_name, model));
        self->DisplayMessage(tr("Team sent to the host."), "");
      }
    });
  }).detach();
}

void NetPlayDialog::OnMsgChangeGame(const NetPlay::SyncIdentifier& sync_identifier,
                                    const std::string& netplay_name)
{
  QString qname = QString::fromStdString(netplay_name);
  QueueOnObject(this, [this, qname, netplay_name, &sync_identifier] {
    m_game_button->setText(qname);
    m_current_game_identifier = sync_identifier;
    m_current_game_name = netplay_name;
    UpdateDiscordPresence();
  });
  DisplayMessage(tr("Game changed to \"%1\"").arg(qname), "magenta");
}

void NetPlayDialog::OnMsgChangeGBARom(int pad, const NetPlay::GBAConfig& config)
{
  if (config.has_rom)
  {
    DisplayMessage(
        tr("GBA%1 ROM changed to \"%2\"").arg(pad + 1).arg(QString::fromStdString(config.title)),
        "magenta");
  }
  else
  {
    DisplayMessage(tr("GBA%1 ROM disabled").arg(pad + 1), "magenta");
  }
}

void NetPlayDialog::GameStatusChanged(bool running)
{
  QueueOnObject(this, [this, running] { SetOptionsEnabled(!running); });
}

void NetPlayDialog::SetOptionsEnabled(bool enabled)
{
  if (Settings::Instance().GetNetPlayServer())
  {
    m_start_button->setEnabled(enabled);
    m_game_button->setEnabled(enabled);
    m_savedata_none_action->setEnabled(enabled);
    m_savedata_load_only_action->setEnabled(enabled);
    m_savedata_load_and_write_action->setEnabled(enabled);
    m_savedata_all_wii_saves_action->setEnabled(enabled);
    m_sync_codes_action->setEnabled(enabled);
    m_assign_ports_button->setEnabled(enabled);
    m_strict_settings_sync_action->setEnabled(enabled);
    m_host_input_authority_action->setEnabled(enabled);
    m_golf_mode_action->setEnabled(enabled);
    m_fixed_delay_action->setEnabled(enabled);
  }

  m_record_input_action->setEnabled(enabled);
}

void NetPlayDialog::OnMsgStartGame()
{
  DisplayMessage(tr("Started game"), "green");

  g_netplay_chat_ui =
      std::make_unique<NetPlayChatUI>([this](const std::string& message) { SendMessage(message); });

  if (m_host_input_authority && Settings::Instance().GetNetPlayClient()->GetNetSettings().golf_mode)
  {
    g_netplay_golf_ui = std::make_unique<NetPlayGolfUI>(Settings::Instance().GetNetPlayClient());
  }

  QueueOnObject(this, [this] {
    const auto client = Settings::Instance().GetNetPlayClient();

    if (client)
    {
      if (const auto game = FindGameFile(m_current_game_identifier))
        client->StartGame(game->GetFilePath());
      else
        PanicAlertFmtT("Selected game doesn't exist in game list!");
    }
    UpdateDiscordPresence();
  });
}

void NetPlayDialog::OnMsgStopGame()
{
  g_netplay_chat_ui.reset();
  g_netplay_golf_ui.reset();
  QueueOnObject(this, [this] { UpdateDiscordPresence(); });
}

void NetPlayDialog::OnMsgPowerButton()
{
  if (!Core::IsRunning(Core::System::GetInstance()))
    return;
  QueueOnObject(this, [] { UICommon::TriggerSTMPowerEvent(); });
}

void NetPlayDialog::OnPlayerConnect(const std::string& player)
{
  DisplayMessage(tr("%1 has joined").arg(QString::fromStdString(player)), "darkcyan");
}

void NetPlayDialog::OnPlayerDisconnect(const std::string& player)
{
  DisplayMessage(tr("%1 has left").arg(QString::fromStdString(player)), "darkcyan");
}

void NetPlayDialog::OnPadBufferChanged(u32 buffer)
{
  QueueOnObject(this, [this, buffer] {
    const QSignalBlocker blocker(m_buffer_size_box);
    m_buffer_size_box->setValue(buffer);

    // Say WHY the number moved. A host with Auto on did not touch the spinbox,
    // so an unexplained jump would read as a glitch. (Ask the server rather
    // than the checkbox: only the host runs the sizer, and only in fixed
    // delay, which is exactly what IsAutoPadBufferEnabled reports.)
    const auto server = Settings::Instance().GetNetPlayServer();
    if (server && !m_host_input_authority && server->IsAutoPadBufferEnabled())
    {
      DisplayMessage(tr("Buffer size changed to %1 (automatic, from ping)").arg(buffer),
                     "darkcyan");
      return;
    }

    DisplayMessage(m_host_input_authority ? tr("Max buffer size changed to %1").arg(buffer) :
                                            tr("Buffer size changed to %1").arg(buffer),
                   "darkcyan");
  });

  m_buffer_size = static_cast<int>(buffer);
}

void NetPlayDialog::OnHostInputAuthorityChanged(bool enabled)
{
  m_host_input_authority = enabled;
  DisplayMessage(enabled ? tr("Host input authority enabled") : tr("Host input authority disabled"),
                 "");

  QueueOnObject(this, [this, enabled] {
    const bool is_hosting = IsHosting();
    const bool enable_buffer = is_hosting != enabled;

    if (is_hosting)
    {
      m_buffer_size_box->setEnabled(enable_buffer);
      m_buffer_label->setEnabled(enable_buffer);
      m_buffer_size_box->setHidden(false);
      m_buffer_label->setHidden(false);
    }
    else
    {
      m_buffer_size_box->setEnabled(true);
      m_buffer_label->setEnabled(true);
      m_buffer_size_box->setHidden(!enable_buffer);
      m_buffer_label->setHidden(!enable_buffer);
    }

    // The automatic sizer is host-side and fixed-delay only: under host input
    // authority each client owns its own buffer, so there is no single
    // host-owned value to size and the server deliberately leaves it alone.
    const bool show_auto_buffer = is_hosting && !enabled;
    m_auto_buffer_box->setHidden(!show_auto_buffer);
    m_auto_buffer_box->setEnabled(show_auto_buffer);

    m_buffer_label->setText(enabled ? tr("Max Buffer:") : tr("Buffer:"));
    if (enabled)
    {
      const QSignalBlocker blocker(m_buffer_size_box);
      m_buffer_size_box->setValue(Config::Get(Config::NETPLAY_CLIENT_BUFFER_SIZE));
    }
  });
}

void NetPlayDialog::OnDesync(u32 frame, const std::string& player)
{
  DisplayMessage(tr("Possible desync detected: %1 might have desynced at frame %2")
                     .arg(QString::fromStdString(player), QString::number(frame)),
                 "red", OSD::Duration::VERY_LONG);
}

void NetPlayDialog::OnConnectionLost()
{
  DisplayMessage(tr("Lost connection to NetPlay server..."), "red");
}

void NetPlayDialog::OnConnectionError(const std::string& message)
{
  QueueOnObject(this, [this, message] {
    ModalMessageBox::critical(this, tr("Error"),
                              tr("Failed to connect to server: %1").arg(tr(message.c_str())));
  });
}

void NetPlayDialog::OnTraversalError(Common::TraversalClient::FailureReason error)
{
  QueueOnObject(this, [this, error] {
    switch (error)
    {
    case Common::TraversalClient::FailureReason::BadHost:
      ModalMessageBox::critical(this, tr("Traversal Error"), tr("Couldn't look up central server"));
      QDialog::reject();
      break;
    case Common::TraversalClient::FailureReason::VersionTooOld:
      ModalMessageBox::critical(this, tr("Traversal Error"),
                                tr("Dolphin is too old for traversal server"));
      QDialog::reject();
      break;
    case Common::TraversalClient::FailureReason::ServerForgotAboutUs:
    case Common::TraversalClient::FailureReason::SocketSendError:
    case Common::TraversalClient::FailureReason::ResendTimeout:
      UpdateGUI();
      break;
    }
  });
}

void NetPlayDialog::OnTraversalStateChanged(Common::TraversalClient::State state)
{
  switch (state)
  {
  case Common::TraversalClient::State::Connected:
  case Common::TraversalClient::State::Failure:
    UpdateDiscordPresence();
    break;
  default:
    break;
  }
}

void NetPlayDialog::OnGameStartAborted()
{
  QueueOnObject(this, [this] { SetOptionsEnabled(true); });
}

void NetPlayDialog::OnGolferChanged(const bool is_golfer, const std::string& golfer_name)
{
  if (m_host_input_authority)
  {
    QueueOnObject(this, [this, is_golfer] {
      m_buffer_size_box->setEnabled(!is_golfer);
      m_buffer_label->setEnabled(!is_golfer);
    });
  }

  if (!golfer_name.empty())
    DisplayMessage(tr("%1 is now golfing").arg(QString::fromStdString(golfer_name)), "");
}

void NetPlayDialog::OnTtlDetermined(u8 ttl)
{
  DisplayMessage(tr("Using TTL %1 for probe packet").arg(QString::number(ttl)), "");
}

bool NetPlayDialog::IsRecording()
{
  const std::optional<bool> is_recording = RunOnObject(m_record_input_action, &QAction::isChecked);
  if (is_recording)
    return *is_recording;
  return false;
}

std::shared_ptr<const UICommon::GameFile>
NetPlayDialog::FindGameFile(const NetPlay::SyncIdentifier& sync_identifier,
                            NetPlay::SyncIdentifierComparison* found)
{
  NetPlay::SyncIdentifierComparison temp;
  if (!found)
    found = &temp;

  *found = NetPlay::SyncIdentifierComparison::DifferentGame;

  const std::optional<std::shared_ptr<const UICommon::GameFile>> game_file =
      RunOnObject(this, [this, &sync_identifier, found] {
        for (int i = 0; i < m_game_list_model.rowCount(QModelIndex()); i++)
        {
          auto file = m_game_list_model.GetGameFile(i);
          *found = std::min(*found, file->CompareSyncIdentifier(sync_identifier));
          if (*found == NetPlay::SyncIdentifierComparison::SameGame)
            return file;
        }
        return static_cast<std::shared_ptr<const UICommon::GameFile>>(nullptr);
      });
  if (game_file)
    return *game_file;
  return nullptr;
}

std::string NetPlayDialog::FindGBARomPath(const std::array<u8, 20>& hash, std::string_view title,
                                          int device_number)
{
#ifdef HAS_LIBMGBA
  const auto result = RunOnObject(this, [&, this] {
    std::string rom_path;
    std::array<u8, 20> rom_hash;
    std::string rom_title;
    for (size_t i = device_number; i < static_cast<size_t>(device_number) + 4; ++i)
    {
      rom_path = Config::Get(Config::MAIN_GBA_ROM_PATHS[i % 4]);
      if (!rom_path.empty() && HW::GBA::Core::GetRomInfo(rom_path.c_str(), rom_hash, rom_title) &&
          rom_hash == hash && rom_title == title)
      {
        return rom_path;
      }
    }
    while (!(rom_path = GameCubePane::GetOpenGBARom(title)).empty())
    {
      if (HW::GBA::Core::GetRomInfo(rom_path.c_str(), rom_hash, rom_title))
      {
        if (rom_hash == hash && rom_title == title)
          return rom_path;
        ModalMessageBox::critical(
            this, tr("Error"),
            QString::fromStdString(Common::FmtFormatT(
                "Mismatched ROMs\n"
                "Selected: {0}\n- Title: {1}\n- Hash: {2:02X}\n"
                "Expected:\n- Title: {3}\n- Hash: {4:02X}",
                rom_path, rom_title, fmt::join(rom_hash, ""), title, fmt::join(hash, ""))));
      }
      else
      {
        ModalMessageBox::critical(
            this, tr("Error"), tr("%1 is not a valid ROM").arg(QString::fromStdString(rom_path)));
      }
    }
    return std::string();
  });
  if (result)
    return *result;
#endif
  return {};
}

void NetPlayDialog::LoadSettings()
{
  const int buffer_size = Config::Get(Config::NETPLAY_BUFFER_SIZE);
  const bool auto_buffer = Config::Get(Config::NETPLAY_AUTO_BUFFER);
  const bool savedata_load = Config::Get(Config::NETPLAY_SAVEDATA_LOAD);
  const bool savedata_write = Config::Get(Config::NETPLAY_SAVEDATA_WRITE);
  const bool sync_all_wii_saves = Config::Get(Config::NETPLAY_SAVEDATA_SYNC_ALL_WII);
  const bool sync_codes = Config::Get(Config::NETPLAY_SYNC_CODES);
  const bool record_inputs = Config::Get(Config::NETPLAY_RECORD_INPUTS);
  const bool strict_settings_sync = Config::Get(Config::NETPLAY_STRICT_SETTINGS_SYNC);
  const bool golf_mode_overlay = Config::Get(Config::NETPLAY_GOLF_MODE_OVERLAY);
  const bool hide_remote_gbas = Config::Get(Config::NETPLAY_HIDE_REMOTE_GBAS);

  m_buffer_size_box->setValue(buffer_size);
  m_auto_buffer_box->setChecked(auto_buffer);

  if (!savedata_load)
    m_savedata_none_action->setChecked(true);
  else if (!savedata_write)
    m_savedata_load_only_action->setChecked(true);
  else
    m_savedata_load_and_write_action->setChecked(true);
  m_savedata_all_wii_saves_action->setChecked(sync_all_wii_saves);

  m_sync_codes_action->setChecked(sync_codes);
  m_record_input_action->setChecked(record_inputs);
  m_strict_settings_sync_action->setChecked(strict_settings_sync);
  m_golf_mode_overlay_action->setChecked(golf_mode_overlay);
  m_hide_remote_gbas_action->setChecked(hide_remote_gbas);

  const std::string network_mode = Config::Get(Config::NETPLAY_NETWORK_MODE);

  if (network_mode == "fixeddelay")
  {
    m_fixed_delay_action->setChecked(true);
  }
  else if (network_mode == "hostinputauthority")
  {
    m_host_input_authority_action->setChecked(true);
  }
  else if (network_mode == "golf")
  {
    m_golf_mode_action->setChecked(true);
  }
  else
  {
    WARN_LOG_FMT(NETPLAY, "Unknown network mode '{}', using 'fixeddelay'", network_mode);
    m_fixed_delay_action->setChecked(true);
  }
}

void NetPlayDialog::SaveSettings()
{
  Config::ConfigChangeCallbackGuard config_guard;

  if (m_host_input_authority)
    Config::SetBase(Config::NETPLAY_CLIENT_BUFFER_SIZE, m_buffer_size_box->value());
  else
    Config::SetBase(Config::NETPLAY_BUFFER_SIZE, m_buffer_size_box->value());

  Config::SetBase(Config::NETPLAY_AUTO_BUFFER, m_auto_buffer_box->isChecked());

  const bool write_savedata = m_savedata_load_and_write_action->isChecked();
  const bool load_savedata = write_savedata || m_savedata_load_only_action->isChecked();
  Config::SetBase(Config::NETPLAY_SAVEDATA_LOAD, load_savedata);
  Config::SetBase(Config::NETPLAY_SAVEDATA_WRITE, write_savedata);

  Config::SetBase(Config::NETPLAY_SAVEDATA_SYNC_ALL_WII,
                  m_savedata_all_wii_saves_action->isChecked());
  Config::SetBase(Config::NETPLAY_SYNC_CODES, m_sync_codes_action->isChecked());
  Config::SetBase(Config::NETPLAY_RECORD_INPUTS, m_record_input_action->isChecked());
  Config::SetBase(Config::NETPLAY_STRICT_SETTINGS_SYNC, m_strict_settings_sync_action->isChecked());
  Config::SetBase(Config::NETPLAY_GOLF_MODE_OVERLAY, m_golf_mode_overlay_action->isChecked());
  Config::SetBase(Config::NETPLAY_HIDE_REMOTE_GBAS, m_hide_remote_gbas_action->isChecked());

  std::string network_mode;
  if (m_fixed_delay_action->isChecked())
  {
    network_mode = "fixeddelay";
  }
  else if (m_host_input_authority_action->isChecked())
  {
    network_mode = "hostinputauthority";
  }
  else if (m_golf_mode_action->isChecked())
  {
    network_mode = "golf";
  }

  Config::SetBase(Config::NETPLAY_NETWORK_MODE, network_mode);
}

void NetPlayDialog::ShowGameDigestDialog(const std::string& title)
{
  QueueOnObject(this, [this, title] {
    m_game_digest_menu->setEnabled(false);

    if (m_game_digest_dialog->isVisible())
      m_game_digest_dialog->close();

    m_game_digest_dialog->show(QString::fromStdString(title));
  });
}

void NetPlayDialog::SetGameDigestProgress(int pid, int progress)
{
  QueueOnObject(this, [this, pid, progress] {
    if (m_game_digest_dialog->isVisible())
      m_game_digest_dialog->SetProgress(pid, progress);
  });
}

void NetPlayDialog::SetGameDigestResult(int pid, const std::string& result)
{
  QueueOnObject(this, [this, pid, result] {
    m_game_digest_dialog->SetResult(pid, result);
    m_game_digest_menu->setEnabled(true);
  });
}

void NetPlayDialog::AbortGameDigest()
{
  QueueOnObject(this, [this] {
    m_game_digest_dialog->close();
    m_game_digest_menu->setEnabled(true);
  });
}

void NetPlayDialog::ShowChunkedProgressDialog(const std::string& title, const u64 data_size,
                                              std::span<const int> players)
{
  QueueOnObject(this, [this, title, data_size, players] {
    if (m_chunked_progress_dialog->isVisible())
      m_chunked_progress_dialog->done(QDialog::Accepted);

    m_chunked_progress_dialog->show(QString::fromStdString(title), data_size, players);
  });
}

void NetPlayDialog::HideChunkedProgressDialog()
{
  QueueOnObject(this, [this] { m_chunked_progress_dialog->done(QDialog::Accepted); });
}

void NetPlayDialog::SetChunkedProgress(const int pid, const u64 progress)
{
  QueueOnObject(this, [this, pid, progress] {
    if (m_chunked_progress_dialog->isVisible())
      m_chunked_progress_dialog->SetProgress(pid, progress);
  });
}

void NetPlayDialog::SetHostWiiSyncData(std::vector<u64> titles, std::string redirect_folder)
{
  const auto client = Settings::Instance().GetNetPlayClient();
  if (client)
    client->SetWiiSyncData(nullptr, std::move(titles), std::move(redirect_folder));
}
