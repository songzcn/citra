// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <clocale>
#include <memory>
#include <thread>
#include <glad/glad.h>
#define QT_NO_OPENGL
#include <cinttypes>
#include <QDesktopWidget>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QtConcurrent/QtConcurrentRun>
#include <QtGui>
#include <QtWidgets>
#include "citra_qt/aboutdialog.h"
#include "citra_qt/applets/swkbd.h"
#include "citra_qt/bootmanager.h"
#include "citra_qt/camera/qt_multimedia_camera.h"
#include "citra_qt/camera/still_image_camera.h"
#include "citra_qt/compatdb.h"
#include "citra_qt/configuration/config.h"
#include "citra_qt/configuration/configure_dialog.h"
#include "citra_qt/debugger/console.h"
#include "citra_qt/debugger/graphics/graphics.h"
#include "citra_qt/debugger/graphics/graphics_breakpoints.h"
#include "citra_qt/debugger/graphics/graphics_cmdlists.h"
#include "citra_qt/debugger/graphics/graphics_surface.h"
#include "citra_qt/debugger/graphics/graphics_tracing.h"
#include "citra_qt/debugger/graphics/graphics_vertex_shader.h"
#include "citra_qt/debugger/lle_service_modules.h"
#include "citra_qt/debugger/profiler.h"
#include "citra_qt/debugger/registers.h"
#include "citra_qt/debugger/wait_tree.h"
#include "citra_qt/game_list.h"
#include "citra_qt/hotkeys.h"
#include "citra_qt/main.h"
#include "citra_qt/multiplayer/state.h"
#include "citra_qt/ui_settings.h"
#include "citra_qt/updater/updater.h"
#include "citra_qt/util/clickable_label.h"
#include "common/common_paths.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/logging/log.h"
#include "common/logging/text_formatter.h"
#include "common/microprofile.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/archive_source_sd_savedata.h"
#include "core/frontend/applets/default_applets.h"
#include "core/gdbstub/gdbstub.h"
#include "core/hle/service/fs/archive.h"
#include "core/loader/loader.h"
#include "core/settings.h"

#ifdef QT_STATICPLUGIN
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#endif

#ifdef _WIN32
extern "C" {
// tells Nvidia drivers to use the dedicated GPU by default on laptops with switchable graphics
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
}
#endif

/**
 * "Callouts" are one-time instructional messages shown to the user. In the config settings, there
 * is a bitfield "callout_flags" options, used to track if a message has already been shown to the
 * user. This is 32-bits - if we have more than 32 callouts, we should retire and recyle old ones.
 */
enum class CalloutFlag : uint32_t {
    Telemetry = 0x1,
};

void GMainWindow::ShowTelemetryCallout() {
    if (UISettings::values.callout_flags & static_cast<uint32_t>(CalloutFlag::Telemetry)) {
        return;
    }

    UISettings::values.callout_flags |= static_cast<uint32_t>(CalloutFlag::Telemetry);
    static const QString telemetry_message =
        tr("<a href='https://citra-emu.org/entry/telemetry-and-why-thats-a-good-thing/'>Anonymous "
           "data is collected</a> to help improve Citra. "
           "<br/><br/>Would you like to share your usage data with us?");
    if (QMessageBox::question(this, tr("Telemetry"), telemetry_message) != QMessageBox::Yes) {
        Settings::values.enable_telemetry = false;
        Settings::Apply();
    }
}

const int GMainWindow::max_recent_files_item;

GMainWindow::GMainWindow() : config(new Config()), emu_thread(nullptr) {
    Log::Filter log_filter;
    log_filter.ParseFilterString(Settings::values.log_filter);
    Log::SetGlobalFilter(log_filter);
    FileUtil::CreateFullPath(FileUtil::GetUserPath(D_LOGS_IDX));
    Log::AddBackend(
        std::make_unique<Log::FileBackend>(FileUtil::GetUserPath(D_LOGS_IDX) + LOG_FILE));
    Debugger::ToggleConsole();
    Settings::LogSettings();

    // register types to use in slots and signals
    qRegisterMetaType<size_t>("size_t");
    qRegisterMetaType<Service::AM::InstallStatus>("Service::AM::InstallStatus");

    LoadTranslation();

    Pica::g_debug_context = Pica::DebugContext::Construct();
    setAcceptDrops(true);
    ui.setupUi(this);
    statusBar()->hide();

    default_theme_paths = QIcon::themeSearchPaths();
    UpdateUITheme();

    Network::Init();

    InitializeWidgets();
    InitializeDebugWidgets();
    InitializeRecentFileMenuActions();
    InitializeHotkeys();
    ShowUpdaterWidgets();

    SetDefaultUIGeometry();
    RestoreUIState();

    ConnectMenuEvents();
    ConnectWidgetEvents();

    SetupUIStrings();
    LOG_INFO(Frontend, "Citra Version: {} | {}-{}", Common::g_build_fullname, Common::g_scm_branch,
             Common::g_scm_desc);

    show();

    game_list->LoadCompatibilityList();
    game_list->PopulateAsync(UISettings::values.game_dirs);

    // Show one-time "callout" messages to the user
    ShowTelemetryCallout();

    if (UISettings::values.check_for_update_on_start) {
        CheckForUpdates();
    }

    QStringList args = QApplication::arguments();
    if (args.length() >= 2) {
        BootGame(args[1]);
    }
}

GMainWindow::~GMainWindow() {
    // will get automatically deleted otherwise
    if (render_window->parent() == nullptr)
        delete render_window;

    Pica::g_debug_context.reset();
    Network::Shutdown();
}

void GMainWindow::InitializeWidgets() {
#ifdef CITRA_ENABLE_COMPATIBILITY_REPORTING
    ui.action_Report_Compatibility->setVisible(true);
#endif
    render_window = new GRenderWindow(this, emu_thread.get());
    render_window->hide();

    game_list = new GameList(this);
    ui.horizontalLayout->addWidget(game_list);

    game_list_placeholder = new GameListPlaceholder(this);
    ui.horizontalLayout->addWidget(game_list_placeholder);
    game_list_placeholder->setVisible(false);

    multiplayer_state = new MultiplayerState(this, game_list->GetModel(), ui.action_Leave_Room,
                                             ui.action_Show_Room);
    multiplayer_state->setVisible(false);

    // Setup updater
    updater = new Updater(this);
    UISettings::values.updater_found = updater->HasUpdater();

    // Create status bar
    message_label = new QLabel();
    // Configured separately for left alignment
    message_label->setVisible(false);
    message_label->setFrameStyle(QFrame::NoFrame);
    message_label->setContentsMargins(4, 0, 4, 0);
    message_label->setAlignment(Qt::AlignLeft);
    statusBar()->addPermanentWidget(message_label, 1);

    progress_bar = new QProgressBar();
    progress_bar->hide();
    statusBar()->addPermanentWidget(progress_bar);

    emu_speed_label = new QLabel();
    emu_speed_label->setToolTip(tr("Current emulation speed. Values higher or lower than 100% "
                                   "indicate emulation is running faster or slower than a 3DS."));
    game_fps_label = new QLabel();
    game_fps_label->setToolTip(tr("How many frames per second the game is currently displaying. "
                                  "This will vary from game to game and scene to scene."));
    emu_frametime_label = new QLabel();
    emu_frametime_label->setToolTip(
        tr("Time taken to emulate a 3DS frame, not counting framelimiting or v-sync. For "
           "full-speed emulation this should be at most 16.67 ms."));

    for (auto& label : {emu_speed_label, game_fps_label, emu_frametime_label}) {
        label->setVisible(false);
        label->setFrameStyle(QFrame::NoFrame);
        label->setContentsMargins(4, 0, 4, 0);
        statusBar()->addPermanentWidget(label, 0);
    }
    statusBar()->addPermanentWidget(multiplayer_state->GetStatusText(), 0);
    statusBar()->addPermanentWidget(multiplayer_state->GetStatusIcon(), 0);
    statusBar()->setVisible(true);

    // Removes an ugly inner border from the status bar widgets under Linux
    setStyleSheet("QStatusBar::item{border: none;}");

    QActionGroup* actionGroup_ScreenLayouts = new QActionGroup(this);
    actionGroup_ScreenLayouts->addAction(ui.action_Screen_Layout_Default);
    actionGroup_ScreenLayouts->addAction(ui.action_Screen_Layout_Single_Screen);
    actionGroup_ScreenLayouts->addAction(ui.action_Screen_Layout_Large_Screen);
    actionGroup_ScreenLayouts->addAction(ui.action_Screen_Layout_Side_by_Side);
}

void GMainWindow::InitializeDebugWidgets() {
    connect(ui.action_Create_Pica_Surface_Viewer, &QAction::triggered, this,
            &GMainWindow::OnCreateGraphicsSurfaceViewer);

    QMenu* debug_menu = ui.menu_View_Debugging;

#if MICROPROFILE_ENABLED
    microProfileDialog = new MicroProfileDialog(this);
    microProfileDialog->hide();
    debug_menu->addAction(microProfileDialog->toggleViewAction());
#endif

    registersWidget = new RegistersWidget(this);
    addDockWidget(Qt::RightDockWidgetArea, registersWidget);
    registersWidget->hide();
    debug_menu->addAction(registersWidget->toggleViewAction());
    connect(this, &GMainWindow::EmulationStarting, registersWidget,
            &RegistersWidget::OnEmulationStarting);
    connect(this, &GMainWindow::EmulationStopping, registersWidget,
            &RegistersWidget::OnEmulationStopping);

    graphicsWidget = new GPUCommandStreamWidget(this);
    addDockWidget(Qt::RightDockWidgetArea, graphicsWidget);
    graphicsWidget->hide();
    debug_menu->addAction(graphicsWidget->toggleViewAction());

    graphicsCommandsWidget = new GPUCommandListWidget(this);
    addDockWidget(Qt::RightDockWidgetArea, graphicsCommandsWidget);
    graphicsCommandsWidget->hide();
    debug_menu->addAction(graphicsCommandsWidget->toggleViewAction());

    graphicsBreakpointsWidget = new GraphicsBreakPointsWidget(Pica::g_debug_context, this);
    addDockWidget(Qt::RightDockWidgetArea, graphicsBreakpointsWidget);
    graphicsBreakpointsWidget->hide();
    debug_menu->addAction(graphicsBreakpointsWidget->toggleViewAction());

    graphicsVertexShaderWidget = new GraphicsVertexShaderWidget(Pica::g_debug_context, this);
    addDockWidget(Qt::RightDockWidgetArea, graphicsVertexShaderWidget);
    graphicsVertexShaderWidget->hide();
    debug_menu->addAction(graphicsVertexShaderWidget->toggleViewAction());

    graphicsTracingWidget = new GraphicsTracingWidget(Pica::g_debug_context, this);
    addDockWidget(Qt::RightDockWidgetArea, graphicsTracingWidget);
    graphicsTracingWidget->hide();
    debug_menu->addAction(graphicsTracingWidget->toggleViewAction());
    connect(this, &GMainWindow::EmulationStarting, graphicsTracingWidget,
            &GraphicsTracingWidget::OnEmulationStarting);
    connect(this, &GMainWindow::EmulationStopping, graphicsTracingWidget,
            &GraphicsTracingWidget::OnEmulationStopping);

    waitTreeWidget = new WaitTreeWidget(this);
    addDockWidget(Qt::LeftDockWidgetArea, waitTreeWidget);
    waitTreeWidget->hide();
    debug_menu->addAction(waitTreeWidget->toggleViewAction());
    connect(this, &GMainWindow::EmulationStarting, waitTreeWidget,
            &WaitTreeWidget::OnEmulationStarting);
    connect(this, &GMainWindow::EmulationStopping, waitTreeWidget,
            &WaitTreeWidget::OnEmulationStopping);

    lleServiceModulesWidget = new LLEServiceModulesWidget(this);
    addDockWidget(Qt::RightDockWidgetArea, lleServiceModulesWidget);
    lleServiceModulesWidget->hide();
    debug_menu->addAction(lleServiceModulesWidget->toggleViewAction());
    connect(this, &GMainWindow::EmulationStarting,
            [this] { lleServiceModulesWidget->setDisabled(true); });
    connect(this, &GMainWindow::EmulationStopping, waitTreeWidget,
            [this] { lleServiceModulesWidget->setDisabled(false); });
}

void GMainWindow::InitializeRecentFileMenuActions() {
    for (int i = 0; i < max_recent_files_item; ++i) {
        actions_recent_files[i] = new QAction(this);
        actions_recent_files[i]->setVisible(false);
        connect(actions_recent_files[i], &QAction::triggered, this, &GMainWindow::OnMenuRecentFile);

        ui.menu_recent_files->addAction(actions_recent_files[i]);
    }
    ui.menu_recent_files->addSeparator();
    QAction* action_clear_recent_files = new QAction(this);
    action_clear_recent_files->setText(tr("Clear Recent Files"));
    connect(action_clear_recent_files, &QAction::triggered, this, [this] {
        UISettings::values.recent_files.clear();
        UpdateRecentFiles();
    });
    ui.menu_recent_files->addAction(action_clear_recent_files);

    UpdateRecentFiles();
}

void GMainWindow::InitializeHotkeys() {
    RegisterHotkey("Main Window", "Load File", QKeySequence::Open);
    RegisterHotkey("Main Window", "Start Emulation");
    RegisterHotkey("Main Window", "Continue/Pause", QKeySequence(Qt::Key_F4));
    RegisterHotkey("Main Window", "Restart", QKeySequence(Qt::Key_F5));
    RegisterHotkey("Main Window", "Swap Screens", QKeySequence(tr("F9")));
    RegisterHotkey("Main Window", "Toggle Screen Layout", QKeySequence(tr("F10")));
    RegisterHotkey("Main Window", "Fullscreen", QKeySequence::FullScreen);
    RegisterHotkey("Main Window", "Exit Fullscreen", QKeySequence(Qt::Key_Escape),
                   Qt::ApplicationShortcut);
    RegisterHotkey("Main Window", "Toggle Speed Limit", QKeySequence("CTRL+Z"),
                   Qt::ApplicationShortcut);
    RegisterHotkey("Main Window", "Increase Speed Limit", QKeySequence("+"),
                   Qt::ApplicationShortcut);
    RegisterHotkey("Main Window", "Decrease Speed Limit", QKeySequence("-"),
                   Qt::ApplicationShortcut);
    LoadHotkeys();

    connect(GetHotkey("Main Window", "Load File", this), &QShortcut::activated, this,
            &GMainWindow::OnMenuLoadFile);
    connect(GetHotkey("Main Window", "Start Emulation", this), &QShortcut::activated, this,
            &GMainWindow::OnStartGame);
    connect(GetHotkey("Main Window", "Continue/Pause", this), &QShortcut::activated, this, [&] {
        if (emulation_running) {
            if (emu_thread->IsRunning()) {
                OnPauseGame();
            } else {
                OnStartGame();
            }
        }
    });
    connect(GetHotkey("Main Window", "Restart", this), &QShortcut::activated, this, [this] {
        if (!Core::System::GetInstance().IsPoweredOn())
            return;
        BootGame(QString(game_path));
    });
    connect(GetHotkey("Main Window", "Swap Screens", render_window), &QShortcut::activated,
            ui.action_Screen_Layout_Swap_Screens, &QAction::trigger);
    connect(GetHotkey("Main Window", "Toggle Screen Layout", render_window), &QShortcut::activated,
            this, &GMainWindow::ToggleScreenLayout);
    connect(GetHotkey("Main Window", "Fullscreen", render_window), &QShortcut::activated,
            ui.action_Fullscreen, &QAction::trigger);
    connect(GetHotkey("Main Window", "Fullscreen", render_window), &QShortcut::activatedAmbiguously,
            ui.action_Fullscreen, &QAction::trigger);
    connect(GetHotkey("Main Window", "Exit Fullscreen", this), &QShortcut::activated, this, [&] {
        if (emulation_running) {
            ui.action_Fullscreen->setChecked(false);
            ToggleFullscreen();
        }
    });
    connect(GetHotkey("Main Window", "Toggle Speed Limit", this), &QShortcut::activated, this, [&] {
        Settings::values.use_frame_limit = !Settings::values.use_frame_limit;
        UpdateStatusBar();
    });
    constexpr u16 SPEED_LIMIT_STEP = 5;
    connect(GetHotkey("Main Window", "Increase Speed Limit", this), &QShortcut::activated, this,
            [&] {
                if (Settings::values.frame_limit < 9999 - SPEED_LIMIT_STEP) {
                    Settings::values.frame_limit += SPEED_LIMIT_STEP;
                    UpdateStatusBar();
                }
            });
    connect(GetHotkey("Main Window", "Decrease Speed Limit", this), &QShortcut::activated, this,
            [&] {
                if (Settings::values.frame_limit > SPEED_LIMIT_STEP) {
                    Settings::values.frame_limit -= SPEED_LIMIT_STEP;
                    UpdateStatusBar();
                }
            });
}

void GMainWindow::ShowUpdaterWidgets() {
    ui.action_Check_For_Updates->setVisible(UISettings::values.updater_found);
    ui.action_Open_Maintenance_Tool->setVisible(UISettings::values.updater_found);

    connect(updater, &Updater::CheckUpdatesDone, this, &GMainWindow::OnUpdateFound);
}

void GMainWindow::SetDefaultUIGeometry() {
    // geometry: 55% of the window contents are in the upper screen half, 45% in the lower half
    const QRect screenRect = QApplication::desktop()->screenGeometry(this);

    const int w = screenRect.width() * 2 / 3;
    const int h = screenRect.height() / 2;
    const int x = (screenRect.x() + screenRect.width()) / 2 - w / 2;
    const int y = (screenRect.y() + screenRect.height()) / 2 - h * 55 / 100;

    setGeometry(x, y, w, h);
}

void GMainWindow::RestoreUIState() {
    restoreGeometry(UISettings::values.geometry);
    restoreState(UISettings::values.state);
    render_window->restoreGeometry(UISettings::values.renderwindow_geometry);
#if MICROPROFILE_ENABLED
    microProfileDialog->restoreGeometry(UISettings::values.microprofile_geometry);
    microProfileDialog->setVisible(UISettings::values.microprofile_visible);
#endif

    game_list->LoadInterfaceLayout();

    ui.action_Single_Window_Mode->setChecked(UISettings::values.single_window_mode);
    ToggleWindowMode();

    ui.action_Fullscreen->setChecked(UISettings::values.fullscreen);
    SyncMenuUISettings();

    ui.action_Display_Dock_Widget_Headers->setChecked(UISettings::values.display_titlebar);
    OnDisplayTitleBars(ui.action_Display_Dock_Widget_Headers->isChecked());

    ui.action_Show_Filter_Bar->setChecked(UISettings::values.show_filter_bar);
    game_list->setFilterVisible(ui.action_Show_Filter_Bar->isChecked());

    ui.action_Show_Status_Bar->setChecked(UISettings::values.show_status_bar);
    statusBar()->setVisible(ui.action_Show_Status_Bar->isChecked());
}

void GMainWindow::ConnectWidgetEvents() {
    connect(game_list, &GameList::GameChosen, this, &GMainWindow::OnGameListLoadFile);
    connect(game_list, &GameList::OpenDirectory, this, &GMainWindow::OnGameListOpenDirectory);
    connect(game_list, &GameList::OpenFolderRequested, this, &GMainWindow::OnGameListOpenFolder);
    connect(game_list, &GameList::NavigateToGamedbEntryRequested, this,
            &GMainWindow::OnGameListNavigateToGamedbEntry);
    connect(game_list, &GameList::AddDirectory, this, &GMainWindow::OnGameListAddDirectory);
    connect(game_list_placeholder, &GameListPlaceholder::AddDirectory, this,
            &GMainWindow::OnGameListAddDirectory);
    connect(game_list, &GameList::ShowList, this, &GMainWindow::OnGameListShowList);

    connect(this, &GMainWindow::EmulationStarting, render_window,
            &GRenderWindow::OnEmulationStarting);
    connect(this, &GMainWindow::EmulationStopping, render_window,
            &GRenderWindow::OnEmulationStopping);

    connect(&status_bar_update_timer, &QTimer::timeout, this, &GMainWindow::UpdateStatusBar);

    connect(this, &GMainWindow::UpdateProgress, this, &GMainWindow::OnUpdateProgress);
    connect(this, &GMainWindow::CIAInstallReport, this, &GMainWindow::OnCIAInstallReport);
    connect(this, &GMainWindow::CIAInstallFinished, this, &GMainWindow::OnCIAInstallFinished);
    connect(this, &GMainWindow::UpdateThemedIcons, multiplayer_state,
            &MultiplayerState::UpdateThemedIcons);
}

void GMainWindow::ConnectMenuEvents() {
    // File
    connect(ui.action_Load_File, &QAction::triggered, this, &GMainWindow::OnMenuLoadFile);
    connect(ui.action_Install_CIA, &QAction::triggered, this, &GMainWindow::OnMenuInstallCIA);
    connect(ui.action_Exit, &QAction::triggered, this, &QMainWindow::close);

    // Emulation
    connect(ui.action_Start, &QAction::triggered, this, &GMainWindow::OnStartGame);
    connect(ui.action_Pause, &QAction::triggered, this, &GMainWindow::OnPauseGame);
    connect(ui.action_Stop, &QAction::triggered, this, &GMainWindow::OnStopGame);
    connect(ui.action_Restart, &QAction::triggered, this, [this] { BootGame(QString(game_path)); });
    connect(ui.action_Report_Compatibility, &QAction::triggered, this,
            &GMainWindow::OnMenuReportCompatibility);
    connect(ui.action_Configure, &QAction::triggered, this, &GMainWindow::OnConfigure);

    // View
    connect(ui.action_Single_Window_Mode, &QAction::triggered, this,
            &GMainWindow::ToggleWindowMode);
    connect(ui.action_Display_Dock_Widget_Headers, &QAction::triggered, this,
            &GMainWindow::OnDisplayTitleBars);
    ui.action_Show_Filter_Bar->setShortcut(tr("CTRL+F"));
    connect(ui.action_Show_Filter_Bar, &QAction::triggered, this, &GMainWindow::OnToggleFilterBar);
    connect(ui.action_Show_Status_Bar, &QAction::triggered, statusBar(), &QStatusBar::setVisible);

    // Multiplayer
    connect(ui.action_View_Lobby, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnViewLobby);
    connect(ui.action_Start_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnCreateRoom);
    connect(ui.action_Leave_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnCloseRoom);
    connect(ui.action_Connect_To_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnDirectConnectToRoom);
    connect(ui.action_Show_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnOpenNetworkRoom);

    ui.action_Fullscreen->setShortcut(GetHotkey("Main Window", "Fullscreen", this)->key());
    ui.action_Screen_Layout_Swap_Screens->setShortcut(
        GetHotkey("Main Window", "Swap Screens", this)->key());
    ui.action_Screen_Layout_Swap_Screens->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(ui.action_Fullscreen, &QAction::triggered, this, &GMainWindow::ToggleFullscreen);
    connect(ui.action_Screen_Layout_Default, &QAction::triggered, this,
            &GMainWindow::ChangeScreenLayout);
    connect(ui.action_Screen_Layout_Single_Screen, &QAction::triggered, this,
            &GMainWindow::ChangeScreenLayout);
    connect(ui.action_Screen_Layout_Large_Screen, &QAction::triggered, this,
            &GMainWindow::ChangeScreenLayout);
    connect(ui.action_Screen_Layout_Side_by_Side, &QAction::triggered, this,
            &GMainWindow::ChangeScreenLayout);
    connect(ui.action_Screen_Layout_Swap_Screens, &QAction::triggered, this,
            &GMainWindow::OnSwapScreens);

    // Help
    connect(ui.action_FAQ, &QAction::triggered,
            []() { QDesktopServices::openUrl(QUrl("https://citra-emu.org/wiki/faq/")); });
    connect(ui.action_About, &QAction::triggered, this, &GMainWindow::OnMenuAboutCitra);
    connect(ui.action_Check_For_Updates, &QAction::triggered, this,
            &GMainWindow::OnCheckForUpdates);
    connect(ui.action_Open_Maintenance_Tool, &QAction::triggered, this,
            &GMainWindow::OnOpenUpdater);
}

void GMainWindow::OnDisplayTitleBars(bool show) {
    QList<QDockWidget*> widgets = findChildren<QDockWidget*>();

    if (show) {
        for (QDockWidget* widget : widgets) {
            QWidget* old = widget->titleBarWidget();
            widget->setTitleBarWidget(nullptr);
            if (old != nullptr)
                delete old;
        }
    } else {
        for (QDockWidget* widget : widgets) {
            QWidget* old = widget->titleBarWidget();
            widget->setTitleBarWidget(new QWidget());
            if (old != nullptr)
                delete old;
        }
    }
}

void GMainWindow::OnCheckForUpdates() {
    explicit_update_check = true;
    CheckForUpdates();
}

void GMainWindow::CheckForUpdates() {
    if (updater->CheckForUpdates()) {
        LOG_INFO(Frontend, "Update check started");
    } else {
        LOG_WARNING(Frontend, "Unable to start check for updates");
    }
}

void GMainWindow::OnUpdateFound(bool found, bool error) {
    if (error) {
        LOG_WARNING(Frontend, "Update check failed");
        return;
    }

    if (!found) {
        LOG_INFO(Frontend, "No updates found");

        // If the user explicitly clicked the "Check for Updates" button, we are
        //  going to want to show them a prompt anyway.
        if (explicit_update_check) {
            explicit_update_check = false;
            ShowNoUpdatePrompt();
        }
        return;
    }

    if (emulation_running && !explicit_update_check) {
        LOG_INFO(Frontend, "Update found, deferring as game is running");
        defer_update_prompt = true;
        return;
    }

    LOG_INFO(Frontend, "Update found!");
    explicit_update_check = false;

    ShowUpdatePrompt();
}

void GMainWindow::ShowUpdatePrompt() {
    defer_update_prompt = false;

    auto result =
        QMessageBox::question(this, tr("Update Available"),
                              tr("An update is available. Would you like to install it now?"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (result == QMessageBox::Yes) {
        updater->LaunchUIOnExit();
        close();
    }
}

void GMainWindow::ShowNoUpdatePrompt() {
    QMessageBox::information(this, tr("No Update Found"), tr("No update is found."),
                             QMessageBox::Ok, QMessageBox::Ok);
}

void GMainWindow::OnOpenUpdater() {
    updater->LaunchUI();
}

bool GMainWindow::LoadROM(const QString& filename) {
    // Shutdown previous session if the emu thread is still active...
    if (emu_thread != nullptr)
        ShutdownGame();

    render_window->InitRenderTarget();
    render_window->MakeCurrent();

    if (!gladLoadGL()) {
        QMessageBox::critical(this, tr("OpenGL 3.3 Unsupported"),
                              tr("Your GPU may not support OpenGL 3.3, or you do not "
                                 "have the latest graphics driver."));
        return false;
    }

    Core::System& system{Core::System::GetInstance()};

    const Core::System::ResultStatus result{system.Load(render_window, filename.toStdString())};

    if (result != Core::System::ResultStatus::Success) {
        switch (result) {
        case Core::System::ResultStatus::ErrorGetLoader:
            LOG_CRITICAL(Frontend, "Failed to obtain loader for {}!", filename.toStdString());
            QMessageBox::critical(
                this, tr("Invalid ROM Format"),
                tr("Your ROM format is not supported.<br/>Please follow the guides to redump your "
                   "<a href='https://citra-emu.org/wiki/dumping-game-cartridges/'>game "
                   "cartridges</a> or "
                   "<a href='https://citra-emu.org/wiki/dumping-installed-titles/'>installed "
                   "titles</a>."));
            break;

        case Core::System::ResultStatus::ErrorSystemMode:
            LOG_CRITICAL(Frontend, "Failed to load ROM!");
            QMessageBox::critical(
                this, tr("ROM Corrupted"),
                tr("Your ROM is corrupted. <br/>Please follow the guides to redump your "
                   "<a href='https://citra-emu.org/wiki/dumping-game-cartridges/'>game "
                   "cartridges</a> or "
                   "<a href='https://citra-emu.org/wiki/dumping-installed-titles/'>installed "
                   "titles</a>."));
            break;

        case Core::System::ResultStatus::ErrorLoader_ErrorEncrypted: {
            QMessageBox::critical(
                this, tr("ROM Encrypted"),
                tr("Your ROM is encrypted. <br/>Please follow the guides to redump your "
                   "<a href='https://citra-emu.org/wiki/dumping-game-cartridges/'>game "
                   "cartridges</a> or "
                   "<a href='https://citra-emu.org/wiki/dumping-installed-titles/'>installed "
                   "titles</a>."));
            break;
        }
        case Core::System::ResultStatus::ErrorLoader_ErrorInvalidFormat:
            QMessageBox::critical(
                this, tr("Invalid ROM Format"),
                tr("Your ROM format is not supported.<br/>Please follow the guides to redump your "
                   "<a href='https://citra-emu.org/wiki/dumping-game-cartridges/'>game "
                   "cartridges</a> or "
                   "<a href='https://citra-emu.org/wiki/dumping-installed-titles/'>installed "
                   "titles</a>."));
            break;

        case Core::System::ResultStatus::ErrorVideoCore:
            QMessageBox::critical(
                this, tr("Video Core Error"),
                tr("An error has occured. Please <a "
                   "href='https://community.citra-emu.org/t/how-to-upload-the-log-file/296'>see "
                   "the "
                   "log</a> for more details. "
                   "Ensure that you have the latest graphics drivers for your GPU."));

            break;

        default:
            QMessageBox::critical(
                this, tr("Error while loading ROM!"),
                tr("An unknown error occured. Please see the log for more details."));
            break;
        }
        return false;
    }

    std::string title;
    system.GetAppLoader().ReadTitle(title);
    game_title = QString::fromStdString(title);
    SetupUIStrings();

    game_path = filename;

    Core::Telemetry().AddField(Telemetry::FieldType::App, "Frontend", "Qt");
    return true;
}

void GMainWindow::BootGame(const QString& filename) {
    LOG_INFO(Frontend, "Citra starting...");
    StoreRecentFile(filename); // Put the filename on top of the list

    if (!LoadROM(filename))
        return;

    // Create and start the emulation thread
    emu_thread = std::make_unique<EmuThread>(render_window);
    emit EmulationStarting(emu_thread.get());
    render_window->moveContext();
    emu_thread->start();

    connect(render_window, &GRenderWindow::Closed, this, &GMainWindow::OnStopGame);
    // BlockingQueuedConnection is important here, it makes sure we've finished refreshing our views
    // before the CPU continues
    connect(emu_thread.get(), &EmuThread::DebugModeEntered, registersWidget,
            &RegistersWidget::OnDebugModeEntered, Qt::BlockingQueuedConnection);
    connect(emu_thread.get(), &EmuThread::DebugModeEntered, waitTreeWidget,
            &WaitTreeWidget::OnDebugModeEntered, Qt::BlockingQueuedConnection);
    connect(emu_thread.get(), &EmuThread::DebugModeLeft, registersWidget,
            &RegistersWidget::OnDebugModeLeft, Qt::BlockingQueuedConnection);
    connect(emu_thread.get(), &EmuThread::DebugModeLeft, waitTreeWidget,
            &WaitTreeWidget::OnDebugModeLeft, Qt::BlockingQueuedConnection);

    // Update the GUI
    registersWidget->OnDebugModeEntered();
    if (ui.action_Single_Window_Mode->isChecked()) {
        game_list->hide();
        game_list_placeholder->hide();
    }
    status_bar_update_timer.start(2000);

    render_window->show();
    render_window->setFocus();

    emulation_running = true;
    if (ui.action_Fullscreen->isChecked()) {
        ShowFullscreen();
    }
    OnStartGame();
}

void GMainWindow::ShutdownGame() {
    emu_thread->RequestStop();

    // Release emu threads from any breakpoints
    // This belongs after RequestStop() and before wait() because if emulation stops on a GPU
    // breakpoint after (or before) RequestStop() is called, the emulation would never be able
    // to continue out to the main loop and terminate. Thus wait() would hang forever.
    // TODO(bunnei): This function is not thread safe, but it's being used as if it were
    Pica::g_debug_context->ClearBreakpoints();

    emit EmulationStopping();

    // Wait for emulation thread to complete and delete it
    emu_thread->wait();
    emu_thread = nullptr;

    Camera::QtMultimediaCameraHandler::ReleaseHandlers();

    // The emulation is stopped, so closing the window or not does not matter anymore
    disconnect(render_window, &GRenderWindow::Closed, this, &GMainWindow::OnStopGame);

    // Update the GUI
    ui.action_Start->setEnabled(false);
    ui.action_Start->setText(tr("Start"));
    ui.action_Pause->setEnabled(false);
    ui.action_Stop->setEnabled(false);
    ui.action_Restart->setEnabled(false);
    ui.action_Report_Compatibility->setEnabled(false);
    render_window->hide();
    if (game_list->isEmpty())
        game_list_placeholder->show();
    else
        game_list->show();
    game_list->setFilterFocus();

    // Disable status bar updates
    status_bar_update_timer.stop();
    message_label->setVisible(false);
    emu_speed_label->setVisible(false);
    game_fps_label->setVisible(false);
    emu_frametime_label->setVisible(false);

    emulation_running = false;

    if (defer_update_prompt) {
        ShowUpdatePrompt();
    }

    game_title.clear();
    SetupUIStrings();

    game_path.clear();
}

void GMainWindow::StoreRecentFile(const QString& filename) {
    UISettings::values.recent_files.prepend(filename);
    UISettings::values.recent_files.removeDuplicates();
    while (UISettings::values.recent_files.size() > max_recent_files_item) {
        UISettings::values.recent_files.removeLast();
    }

    UpdateRecentFiles();
}

void GMainWindow::UpdateRecentFiles() {
    const int num_recent_files =
        std::min(UISettings::values.recent_files.size(), max_recent_files_item);

    for (int i = 0; i < num_recent_files; i++) {
        const QString text = QString("&%1. %2").arg(i + 1).arg(
            QFileInfo(UISettings::values.recent_files[i]).fileName());
        actions_recent_files[i]->setText(text);
        actions_recent_files[i]->setData(UISettings::values.recent_files[i]);
        actions_recent_files[i]->setToolTip(UISettings::values.recent_files[i]);
        actions_recent_files[i]->setVisible(true);
    }

    for (int j = num_recent_files; j < max_recent_files_item; ++j) {
        actions_recent_files[j]->setVisible(false);
    }

    // Enable the recent files menu if the list isn't empty
    ui.menu_recent_files->setEnabled(num_recent_files != 0);
}

void GMainWindow::OnGameListLoadFile(QString game_path) {
    BootGame(game_path);
}

void GMainWindow::OnGameListOpenFolder(u64 program_id, GameListOpenTarget target) {
    std::string path;
    std::string open_target;

    switch (target) {
    case GameListOpenTarget::SAVE_DATA: {
        open_target = "Save Data";
        std::string sdmc_dir = FileUtil::GetUserPath(D_SDMC_IDX);
        path = FileSys::ArchiveSource_SDSaveData::GetSaveDataPathFor(sdmc_dir, program_id);
        break;
    }
    case GameListOpenTarget::APPLICATION:
        open_target = "Application";
        path = Service::AM::GetTitlePath(Service::FS::MediaType::SDMC, program_id) + "content/";
        break;
    case GameListOpenTarget::UPDATE_DATA:
        open_target = "Update Data";
        path = Service::AM::GetTitlePath(Service::FS::MediaType::SDMC, program_id + 0xe00000000) +
               "content/";
        break;
    default:
        LOG_ERROR(Frontend, "Unexpected target {}", static_cast<int>(target));
        return;
    }

    QString qpath = QString::fromStdString(path);

    QDir dir(qpath);
    if (!dir.exists()) {
        QMessageBox::critical(
            this, tr("Error Opening %1 Folder").arg(QString::fromStdString(open_target)),
            tr("Folder does not exist!"));
        return;
    }

    LOG_INFO(Frontend, "Opening {} path for program_id={:016x}", open_target, program_id);

    QDesktopServices::openUrl(QUrl::fromLocalFile(qpath));
}

void GMainWindow::OnGameListNavigateToGamedbEntry(
    u64 program_id,
    std::unordered_map<std::string, std::pair<QString, QString>>& compatibility_list) {

    auto it = std::find_if(
        compatibility_list.begin(), compatibility_list.end(),
        [program_id](const std::pair<std::string, std::pair<QString, QString>>& element) {
            std::string pid = Common::StringFromFormat("%016" PRIX64, program_id);
            return element.first == pid;
        });

    QString directory = "";

    if (it != compatibility_list.end())
        directory = it->second.second;

    QDesktopServices::openUrl(QUrl("https://citra-emu.org/game/" + directory));
}

void GMainWindow::OnGameListOpenDirectory(QString directory) {
    QString path;
    if (directory == "INSTALLED") {
        path =
            QString::fromStdString(FileUtil::GetUserPath(D_SDMC_IDX).c_str() +
                                   std::string("Nintendo "
                                               "3DS/00000000000000000000000000000000/"
                                               "00000000000000000000000000000000/title/00040000"));
    } else if (directory == "SYSTEM") {
        path =
            QString::fromStdString(FileUtil::GetUserPath(D_NAND_IDX).c_str() +
                                   std::string("00000000000000000000000000000000/title/00040010"));
    } else {
        path = directory;
    }
    if (!QFileInfo::exists(path)) {
        QMessageBox::critical(this, tr("Error Opening %1").arg(path), tr("Folder does not exist!"));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void GMainWindow::OnGameListAddDirectory() {
    QString dir_path = QFileDialog::getExistingDirectory(this, tr("Select Directory"));
    if (dir_path.isEmpty())
        return;
    UISettings::GameDir game_dir{dir_path, false, true};
    if (!UISettings::values.game_dirs.contains(game_dir)) {
        UISettings::values.game_dirs.append(game_dir);
        game_list->PopulateAsync(UISettings::values.game_dirs);
    } else {
        LOG_WARNING(Frontend, "Selected directory is already in the game list");
    }
}

void GMainWindow::OnGameListShowList(bool show) {
    if (emulation_running && ui.action_Single_Window_Mode->isChecked())
        return;
    game_list->setVisible(show);
    game_list_placeholder->setVisible(!show);
};

void GMainWindow::OnMenuLoadFile() {
    QString extensions;
    for (const auto& piece : game_list->supported_file_extensions)
        extensions += "*." + piece + " ";

    QString file_filter = tr("3DS Executable") + " (" + extensions + ")";
    file_filter += ";;" + tr("All Files (*.*)");

    QString filename = QFileDialog::getOpenFileName(this, tr("Load File"),
                                                    UISettings::values.roms_path, file_filter);
    if (!filename.isEmpty()) {
        UISettings::values.roms_path = QFileInfo(filename).path();

        BootGame(filename);
    }
}

void GMainWindow::OnMenuInstallCIA() {
    QStringList filepaths = QFileDialog::getOpenFileNames(
        this, tr("Load Files"), UISettings::values.roms_path,
        tr("3DS Installation File (*.CIA*)") + ";;" + tr("All Files (*.*)"));
    if (filepaths.isEmpty())
        return;

    ui.action_Install_CIA->setEnabled(false);
    progress_bar->show();
    progress_bar->setMaximum(INT_MAX);

    QtConcurrent::run([&, filepaths] {
        QString current_path;
        Service::AM::InstallStatus status;
        const auto cia_progress = [&](size_t written, size_t total) {
            emit UpdateProgress(written, total);
        };
        for (const auto current_path : filepaths) {
            status = Service::AM::InstallCIA(current_path.toStdString(), cia_progress);
            emit CIAInstallReport(status, current_path);
        }
        emit CIAInstallFinished();
    });
}

void GMainWindow::OnUpdateProgress(size_t written, size_t total) {
    progress_bar->setValue(
        static_cast<int>(INT_MAX * (static_cast<double>(written) / static_cast<double>(total))));
}

void GMainWindow::OnCIAInstallReport(Service::AM::InstallStatus status, QString filepath) {
    QString filename = QFileInfo(filepath).fileName();
    switch (status) {
    case Service::AM::InstallStatus::Success:
        this->statusBar()->showMessage(tr("%1 has been installed successfully.").arg(filename));
        break;
    case Service::AM::InstallStatus::ErrorFailedToOpenFile:
        QMessageBox::critical(this, tr("Unable to open File"),
                              tr("Could not open %1").arg(filename));
        break;
    case Service::AM::InstallStatus::ErrorAborted:
        QMessageBox::critical(
            this, tr("Installation aborted"),
            tr("The installation of %1 was aborted. Please see the log for more details")
                .arg(filename));
        break;
    case Service::AM::InstallStatus::ErrorInvalid:
        QMessageBox::critical(this, tr("Invalid File"), tr("%1 is not a valid CIA").arg(filename));
        break;
    case Service::AM::InstallStatus::ErrorEncrypted:
        QMessageBox::critical(this, tr("Encrypted File"),
                              tr("%1 must be decrypted "
                                 "before being used with Citra. A real 3DS is required.")
                                  .arg(filename));
        break;
    }
}

void GMainWindow::OnCIAInstallFinished() {
    progress_bar->hide();
    progress_bar->setValue(0);
    ui.action_Install_CIA->setEnabled(true);
}

void GMainWindow::OnMenuRecentFile() {
    QAction* action = qobject_cast<QAction*>(sender());
    assert(action);

    const QString filename = action->data().toString();
    if (QFileInfo::exists(filename)) {
        BootGame(filename);
    } else {
        // Display an error message and remove the file from the list.
        QMessageBox::information(this, tr("File not found"),
                                 tr("File \"%1\" not found").arg(filename));

        UISettings::values.recent_files.removeOne(filename);
        UpdateRecentFiles();
    }
}

void GMainWindow::OnStartGame() {
    Camera::QtMultimediaCameraHandler::ResumeCameras();
    emu_thread->SetRunning(true);
    qRegisterMetaType<Core::System::ResultStatus>("Core::System::ResultStatus");
    qRegisterMetaType<std::string>("std::string");
    connect(emu_thread.get(), &EmuThread::ErrorThrown, this, &GMainWindow::OnCoreError);

    ui.action_Start->setEnabled(false);
    ui.action_Start->setText(tr("Continue"));

    ui.action_Pause->setEnabled(true);
    ui.action_Stop->setEnabled(true);
    ui.action_Restart->setEnabled(true);
    ui.action_Report_Compatibility->setEnabled(true);
}

void GMainWindow::OnPauseGame() {
    emu_thread->SetRunning(false);
    Camera::QtMultimediaCameraHandler::StopCameras();
    ui.action_Start->setEnabled(true);
    ui.action_Pause->setEnabled(false);
    ui.action_Stop->setEnabled(true);
}

void GMainWindow::OnStopGame() {
    ShutdownGame();
}

void GMainWindow::OnMenuReportCompatibility() {
    if (!Settings::values.citra_token.empty() && !Settings::values.citra_username.empty()) {
        CompatDB compatdb{this};
        compatdb.exec();
    } else {
        QMessageBox::critical(this, tr("Missing Citra Account"),
                              tr("You must link your Citra account to submit test cases."
                                 "<br/>Go to Emulation &gt; Configure... &gt; Web to do so."));
    }
}

void GMainWindow::ToggleFullscreen() {
    if (!emulation_running) {
        return;
    }
    if (ui.action_Fullscreen->isChecked()) {
        ShowFullscreen();
    } else {
        HideFullscreen();
    }
}

void GMainWindow::ShowFullscreen() {
    if (ui.action_Single_Window_Mode->isChecked()) {
        UISettings::values.geometry = saveGeometry();
        ui.menubar->hide();
        statusBar()->hide();
        showFullScreen();
    } else {
        UISettings::values.renderwindow_geometry = render_window->saveGeometry();
        render_window->showFullScreen();
    }
}

void GMainWindow::HideFullscreen() {
    if (ui.action_Single_Window_Mode->isChecked()) {
        statusBar()->setVisible(ui.action_Show_Status_Bar->isChecked());
        ui.menubar->show();
        showNormal();
        restoreGeometry(UISettings::values.geometry);
    } else {
        render_window->showNormal();
        render_window->restoreGeometry(UISettings::values.renderwindow_geometry);
    }
}

void GMainWindow::ToggleWindowMode() {
    if (ui.action_Single_Window_Mode->isChecked()) {
        // Render in the main window...
        render_window->BackupGeometry();
        ui.horizontalLayout->addWidget(render_window);
        render_window->setFocusPolicy(Qt::ClickFocus);
        if (emulation_running) {
            render_window->setVisible(true);
            render_window->setFocus();
            game_list->hide();
        }

    } else {
        // Render in a separate window...
        ui.horizontalLayout->removeWidget(render_window);
        render_window->setParent(nullptr);
        render_window->setFocusPolicy(Qt::NoFocus);
        if (emulation_running) {
            render_window->setVisible(true);
            render_window->RestoreGeometry();
            game_list->show();
        }
    }
}

void GMainWindow::ChangeScreenLayout() {
    Settings::LayoutOption new_layout = Settings::LayoutOption::Default;

    if (ui.action_Screen_Layout_Default->isChecked()) {
        new_layout = Settings::LayoutOption::Default;
    } else if (ui.action_Screen_Layout_Single_Screen->isChecked()) {
        new_layout = Settings::LayoutOption::SingleScreen;
    } else if (ui.action_Screen_Layout_Large_Screen->isChecked()) {
        new_layout = Settings::LayoutOption::LargeScreen;
    } else if (ui.action_Screen_Layout_Side_by_Side->isChecked()) {
        new_layout = Settings::LayoutOption::SideScreen;
    }

    Settings::values.layout_option = new_layout;
    Settings::Apply();
}

void GMainWindow::ToggleScreenLayout() {
    Settings::LayoutOption new_layout = Settings::LayoutOption::Default;

    switch (Settings::values.layout_option) {
    case Settings::LayoutOption::Default:
        new_layout = Settings::LayoutOption::SingleScreen;
        break;
    case Settings::LayoutOption::SingleScreen:
        new_layout = Settings::LayoutOption::LargeScreen;
        break;
    case Settings::LayoutOption::LargeScreen:
        new_layout = Settings::LayoutOption::SideScreen;
        break;
    case Settings::LayoutOption::SideScreen:
        new_layout = Settings::LayoutOption::Default;
        break;
    }

    Settings::values.layout_option = new_layout;
    SyncMenuUISettings();
    Settings::Apply();
}

void GMainWindow::OnSwapScreens() {
    Settings::values.swap_screen = ui.action_Screen_Layout_Swap_Screens->isChecked();
    Settings::Apply();
}

void GMainWindow::OnConfigure() {
    ConfigureDialog configureDialog(this);
    connect(&configureDialog, &ConfigureDialog::languageChanged, this,
            &GMainWindow::OnLanguageChanged);
    auto old_theme = UISettings::values.theme;
    auto result = configureDialog.exec();
    if (result == QDialog::Accepted) {
        configureDialog.applyConfiguration();
        if (UISettings::values.theme != old_theme)
            UpdateUITheme();
        emit UpdateThemedIcons();
        SyncMenuUISettings();
        config->Save();
    }
}

void GMainWindow::OnToggleFilterBar() {
    game_list->setFilterVisible(ui.action_Show_Filter_Bar->isChecked());
    if (ui.action_Show_Filter_Bar->isChecked()) {
        game_list->setFilterFocus();
    } else {
        game_list->clearFilter();
    }
}

void GMainWindow::OnCreateGraphicsSurfaceViewer() {
    auto graphicsSurfaceViewerWidget = new GraphicsSurfaceWidget(Pica::g_debug_context, this);
    addDockWidget(Qt::RightDockWidgetArea, graphicsSurfaceViewerWidget);
    // TODO: Maybe graphicsSurfaceViewerWidget->setFloating(true);
    graphicsSurfaceViewerWidget->show();
}

void GMainWindow::UpdateStatusBar() {
    if (emu_thread == nullptr) {
        status_bar_update_timer.stop();
        return;
    }

    auto results = Core::System::GetInstance().GetAndResetPerfStats();

    if (Settings::values.use_frame_limit) {
        emu_speed_label->setText(tr("Speed: %1% / %2%")
                                     .arg(results.emulation_speed * 100.0, 0, 'f', 0)
                                     .arg(Settings::values.frame_limit));
    } else {
        emu_speed_label->setText(tr("Speed: %1%").arg(results.emulation_speed * 100.0, 0, 'f', 0));
    }
    game_fps_label->setText(tr("Game: %1 FPS").arg(results.game_fps, 0, 'f', 0));
    emu_frametime_label->setText(tr("Frame: %1 ms").arg(results.frametime * 1000.0, 0, 'f', 2));

    emu_speed_label->setVisible(true);
    game_fps_label->setVisible(true);
    emu_frametime_label->setVisible(true);
}

void GMainWindow::OnCoreError(Core::System::ResultStatus result, std::string details) {
    QMessageBox::StandardButton answer;
    QString status_message;
    const QString common_message =
        tr("%1 is missing. Please <a "
           "href='https://citra-emu.org/wiki/"
           "dumping-system-archives-and-the-shared-fonts-from-a-3ds-console/'>dump your "
           "system archives</a>.<br/>Continuing emulation may result in crashes and bugs.");
    QString title, message;
    switch (result) {
    case Core::System::ResultStatus::ErrorSystemFiles: {
        if (!details.empty()) {
            message = common_message.arg(QString::fromStdString(details));
        } else {
            message = common_message.arg("A system archive");
        }

        title = tr("System Archive Not Found");
        status_message = "System Archive Missing";
        break;
    }

    case Core::System::ResultStatus::ErrorSharedFont: {
        message = tr("Shared fonts not found. ");
        message.append(common_message);
        title = tr("Shared Fonts Not Found");
        status_message = "Shared Font Missing";
        break;
    }

    default:
        title = tr("Fatal Error");
        message =
            tr("A fatal error occured. "
               "<a href='https://community.citra-emu.org/t/how-to-upload-the-log-file/296'>Check "
               "the log</a> for details."
               "<br/>Continuing emulation may result in crashes and bugs.");
        status_message = "Fatal Error encountered";
        break;
    }

    QMessageBox message_box;
    message_box.setWindowTitle(title);
    message_box.setText(message);
    message_box.setIcon(QMessageBox::Icon::Critical);
    QPushButton* continue_button = message_box.addButton(tr("Continue"), QMessageBox::RejectRole);
    QPushButton* abort_button = message_box.addButton(tr("Abort"), QMessageBox::AcceptRole);
    message_box.exec();

    if (message_box.clickedButton() == abort_button) {
        if (emu_thread) {
            ShutdownGame();
        }
    } else {
        // Only show the message if the game is still running.
        if (emu_thread) {
            emu_thread->SetRunning(true);
            message_label->setText(status_message);
            message_label->setVisible(true);
        }
    }
}

void GMainWindow::OnMenuAboutCitra() {
    AboutDialog about{this};
    about.exec();
}

bool GMainWindow::ConfirmClose() {
    if (emu_thread == nullptr || !UISettings::values.confirm_before_closing)
        return true;

    QMessageBox::StandardButton answer =
        QMessageBox::question(this, tr("Citra"), tr("Would you like to exit now?"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return answer != QMessageBox::No;
}

void GMainWindow::closeEvent(QCloseEvent* event) {
    if (!ConfirmClose()) {
        event->ignore();
        return;
    }

    if (!ui.action_Fullscreen->isChecked()) {
        UISettings::values.geometry = saveGeometry();
        UISettings::values.renderwindow_geometry = render_window->saveGeometry();
    }
    UISettings::values.state = saveState();
#if MICROPROFILE_ENABLED
    UISettings::values.microprofile_geometry = microProfileDialog->saveGeometry();
    UISettings::values.microprofile_visible = microProfileDialog->isVisible();
#endif
    UISettings::values.single_window_mode = ui.action_Single_Window_Mode->isChecked();
    UISettings::values.fullscreen = ui.action_Fullscreen->isChecked();
    UISettings::values.display_titlebar = ui.action_Display_Dock_Widget_Headers->isChecked();
    UISettings::values.show_filter_bar = ui.action_Show_Filter_Bar->isChecked();
    UISettings::values.show_status_bar = ui.action_Show_Status_Bar->isChecked();
    UISettings::values.first_start = false;

    game_list->SaveInterfaceLayout();
    SaveHotkeys();

    // Shutdown session if the emu thread is active...
    if (emu_thread != nullptr)
        ShutdownGame();

    render_window->close();
    multiplayer_state->Close();
    QWidget::closeEvent(event);
}

static bool IsSingleFileDropEvent(QDropEvent* event) {
    const QMimeData* mimeData = event->mimeData();
    return mimeData->hasUrls() && mimeData->urls().length() == 1;
}

void GMainWindow::dropEvent(QDropEvent* event) {
    if (IsSingleFileDropEvent(event) && ConfirmChangeGame()) {
        const QMimeData* mimeData = event->mimeData();
        QString filename = mimeData->urls().at(0).toLocalFile();
        BootGame(filename);
    }
}

void GMainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (IsSingleFileDropEvent(event)) {
        event->acceptProposedAction();
    }
}

void GMainWindow::dragMoveEvent(QDragMoveEvent* event) {
    event->acceptProposedAction();
}

bool GMainWindow::ConfirmChangeGame() {
    if (emu_thread == nullptr)
        return true;

    auto answer = QMessageBox::question(
        this, tr("Citra"), tr("The game is still running. Would you like to stop emulation?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return answer != QMessageBox::No;
}

void GMainWindow::filterBarSetChecked(bool state) {
    ui.action_Show_Filter_Bar->setChecked(state);
    emit(OnToggleFilterBar());
}

void GMainWindow::UpdateUITheme() {
    QStringList theme_paths(default_theme_paths);
    if (UISettings::values.theme != UISettings::themes[0].second &&
        !UISettings::values.theme.isEmpty()) {
        const QString theme_uri(":" + UISettings::values.theme + "/style.qss");
        QFile f(theme_uri);
        if (f.open(QFile::ReadOnly | QFile::Text)) {
            QTextStream ts(&f);
            qApp->setStyleSheet(ts.readAll());
            GMainWindow::setStyleSheet(ts.readAll());
        } else {
            LOG_ERROR(Frontend, "Unable to set style, stylesheet file not found");
        }
        theme_paths.append(QStringList{":/icons/default", ":/icons/" + UISettings::values.theme});
        QIcon::setThemeName(":/icons/" + UISettings::values.theme);
    } else {
        qApp->setStyleSheet("");
        GMainWindow::setStyleSheet("");
        theme_paths.append(QStringList{":/icons/default"});
        QIcon::setThemeName(":/icons/default");
    }
    QIcon::setThemeSearchPaths(theme_paths);
}

void GMainWindow::LoadTranslation() {
    // If the selected language is English, no need to install any translation
    if (UISettings::values.language == "en") {
        return;
    }

    bool loaded;

    if (UISettings::values.language.isEmpty()) {
        // If the selected language is empty, use system locale
        loaded = translator.load(QLocale(), "", "", ":/languages/");
    } else {
        // Otherwise load from the specified file
        loaded = translator.load(UISettings::values.language, ":/languages/");
    }

    if (loaded) {
        qApp->installTranslator(&translator);
    } else {
        UISettings::values.language = "en";
    }
}

void GMainWindow::OnLanguageChanged(const QString& locale) {
    if (UISettings::values.language != "en") {
        qApp->removeTranslator(&translator);
    }

    UISettings::values.language = locale;
    LoadTranslation();
    ui.retranslateUi(this);
    RetranslateStatusBar();
    SetupUIStrings();

    if (emulation_running)
        ui.action_Start->setText(tr("Continue"));
}

void GMainWindow::SetupUIStrings() {
    if (game_title.isEmpty()) {
        setWindowTitle(tr("Citra %1").arg(Common::g_build_fullname));
    } else {
        setWindowTitle(tr("Citra %1| %2").arg(Common::g_build_fullname, game_title));
    }
}

void GMainWindow::SyncMenuUISettings() {
    ui.action_Screen_Layout_Default->setChecked(Settings::values.layout_option ==
                                                Settings::LayoutOption::Default);
    ui.action_Screen_Layout_Single_Screen->setChecked(Settings::values.layout_option ==
                                                      Settings::LayoutOption::SingleScreen);
    ui.action_Screen_Layout_Large_Screen->setChecked(Settings::values.layout_option ==
                                                     Settings::LayoutOption::LargeScreen);
    ui.action_Screen_Layout_Side_by_Side->setChecked(Settings::values.layout_option ==
                                                     Settings::LayoutOption::SideScreen);
    ui.action_Screen_Layout_Swap_Screens->setChecked(Settings::values.swap_screen);
}

void GMainWindow::RetranslateStatusBar() {
    if (emu_thread)
        UpdateStatusBar();

    emu_speed_label->setToolTip(tr("Current emulation speed. Values higher or lower than 100% "
                                   "indicate emulation is running faster or slower than a 3DS."));
    game_fps_label->setToolTip(tr("How many frames per second the game is currently displaying. "
                                  "This will vary from game to game and scene to scene."));
    emu_frametime_label->setToolTip(
        tr("Time taken to emulate a 3DS frame, not counting framelimiting or v-sync. For "
           "full-speed emulation this should be at most 16.67 ms."));

    multiplayer_state->retranslateUi();
}

#ifdef main
#undef main
#endif

int main(int argc, char* argv[]) {
    MicroProfileOnThreadCreate("Frontend");
    SCOPE_EXIT({ MicroProfileShutdown(); });

    // Init settings params
    QCoreApplication::setOrganizationName("Citra team");
    QCoreApplication::setApplicationName("Citra");

    QApplication app(argc, argv);

    // Qt changes the locale and causes issues in float conversion using std::to_string() when
    // generating shaders
    setlocale(LC_ALL, "C");

    GMainWindow main_window;

    // Register CameraFactory
    Camera::RegisterFactory("image", std::make_unique<Camera::StillImageCameraFactory>());
    Camera::RegisterFactory("qt", std::make_unique<Camera::QtMultimediaCameraFactory>());
    Camera::QtMultimediaCameraHandler::Init();

    // Register frontend applets
    Frontend::RegisterDefaultApplets();
    Frontend::RegisterSoftwareKeyboard(std::make_shared<QtKeyboard>(main_window));

    main_window.show();
    return app.exec();
}
