/*
    SPDX-FileCopyrightText: 2006-2008 Robert Knight <robertknight@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Own
#include "Application.h"

// Qt
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

// KDE
#include <KActionCollection>
#if HAVE_DBUS
#include <KGlobalAccel>
#endif
#include <KLocalizedString>

// Konsole
#include "KonsoleSettings.h"
#include "MainWindow.h"
#include "ShellCommand.h"
#include "ViewManager.h"
#include "WindowSystemInfo.h"
#include "profile/ProfileCommandParser.h"
#include "profile/ProfileManager.h"
#include "session/Session.h"
#include "session/SessionManager.h"
#include "widgets/ViewContainer.h"

#include "pluginsystem/IKonsolePlugin.h"
#include "tmux/TmuxController.h"
#include "tmux/TmuxControllerRegistry.h"
#include "tmux/TmuxProcessBridge.h"

using namespace Konsole;

Application::Application(QSharedPointer<QCommandLineParser> parser, const QStringList &customCommand)
    : _backgroundInstance(nullptr)
    , m_parser(parser)
    , m_customCommand(customCommand)
{
    m_pluginManager.loadAllPlugins();
}

void Application::populateCommandLineParser(QCommandLineParser *parser)
{
    const auto options = QVector<QCommandLineOption>{
        {{QStringLiteral("profile")}, i18nc("@info:shell", "Name of profile to use for new Konsole instance"), QStringLiteral("name")},
        {{QStringLiteral("layout")}, i18nc("@info:shell", "json layoutfile to be loaded to use for new Konsole instance"), QStringLiteral("file")},
        {{QStringLiteral("builtin-profile")}, i18nc("@info:shell", "Use the built-in profile instead of the default profile")},
        {{QStringLiteral("workdir")}, i18nc("@info:shell", "Set the initial working directory of the new tab or window to 'dir'"), QStringLiteral("dir")},
        {{QStringLiteral("hold"), QStringLiteral("noclose")}, i18nc("@info:shell", "Do not close the initial session automatically when it ends.")},
        // BR: 373440
        {{QStringLiteral("new-tab")},
         i18nc("@info:shell",
               "Create a new tab in an existing window rather than creating a new window ('Run all Konsole windows in a single process' must be enabled)")},
        {{QStringLiteral("tabs-from-file")}, i18nc("@info:shell", "Create tabs as specified in given tabs configuration file"), QStringLiteral("file")},
        {{QStringLiteral("background-mode")},
         i18nc("@info:shell", "Start Konsole in the background and bring to the front when Ctrl+Shift+F12 (by default) is pressed")},
        {{QStringLiteral("separate"), QStringLiteral("nofork")}, i18nc("@info:shell", "Run in a separate process")},
        {{QStringLiteral("show-menubar")}, i18nc("@info:shell", "Show the menubar, overriding the default setting")},
        {{QStringLiteral("hide-menubar")}, i18nc("@info:shell", "Hide the menubar, overriding the default setting")},
        {{QStringLiteral("show-toolbars")}, i18nc("@info:shell", "Show all the toolbars, overriding the default setting")},
        {{QStringLiteral("hide-toolbars")}, i18nc("@info:shell", "Hide all the toolbars, overriding the default setting")},
        {{QStringLiteral("show-tabbar")}, i18nc("@info:shell", "Show the tabbar, overriding the default setting")},
        {{QStringLiteral("hide-tabbar")}, i18nc("@info:shell", "Hide the tabbar, overriding the default setting")},
        {{QStringLiteral("fullscreen")}, i18nc("@info:shell", "Start Konsole in fullscreen mode")},
        {{QStringLiteral("notransparency")}, i18nc("@info:shell", "Disable transparent backgrounds, even if the system supports them.")},
        {{QStringLiteral("list-profiles")}, i18nc("@info:shell", "List the available profiles")},
        {{QStringLiteral("list-profile-properties")}, i18nc("@info:shell", "List all the profile properties names and their type (for use with -p)")},
        {{QStringLiteral("p")}, i18nc("@info:shell", "Change the value of a profile property."), QStringLiteral("property=value")},
        {{QStringLiteral("e")},
         i18nc("@info:shell", "Command to execute. This option will catch all following arguments, so use it as the last option."),
         QStringLiteral("cmd")},
        {{QStringLiteral("force-reuse")},
         i18nc("@info:shell", "Force re-using the existing instance even if it breaks functionality, e. g. --new-tab. Mostly for debugging.")},
        {{QStringLiteral("s"), QStringLiteral("session")}, i18nc("@info:shell", "Name of the tmux session to attach to or create"), QStringLiteral("name")},
        {{QStringLiteral("S"), QStringLiteral("socket")}, i18nc("@info:shell", "Path to the tmux server socket"), QStringLiteral("path")},
        {{QStringLiteral("rsh")},
         i18nc("@info:shell", "Remote shell command used to run tmux (e.g. \"ssh user@host\"). Defaults to the KMUX_RSH environment variable."),
         QStringLiteral("cmd")},
        {{QStringLiteral("tmux-path")},
         i18nc("@info:shell", "Path or name of the tmux program to run (locally, or on the remote host when --rsh is given). Defaults to \"tmux\" in PATH."),
         QStringLiteral("prog")},
    };

    for (const auto &option : options) {
        parser->addOption(option);
    }

    parser->addPositionalArgument(QStringLiteral("[args]"), i18nc("@info:shell", "Arguments passed to command"));

    // Add a no-op compatibility option to make Konsole compatible with
    // Debian's policy on X terminal emulators.
    // -T is technically meant to set a title, that is not really meaningful
    // for Konsole as we have multiple user-facing options controlling
    // the title and overriding whatever is set elsewhere.
    // https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=532029
    // https://www.debian.org/doc/debian-policy/ch-customized-programs.html#s11.8.3
    // --title is used by the VirtualBox Guest Additions installer
    auto titleOption =
        QCommandLineOption({QStringLiteral("T"), QStringLiteral("title")}, QStringLiteral("Debian policy compatibility, not used"), QStringLiteral("value"));
    titleOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser->addOption(titleOption);
}

QStringList Application::getCustomCommand(QStringList &args)
{
    int i = args.indexOf(QStringLiteral("-e"));
    QStringList customCommand;
    if ((0 < i) && (i < (args.size() - 1))) {
        // -e was specified with at least one extra argument
        // if -e was specified without arguments, QCommandLineParser will deal
        // with that
        args.removeAt(i);
        while (args.size() > i) {
            customCommand << args.takeAt(i);
        }
    }
    return customCommand;
}

Application::~Application()
{
    SessionManager::instance()->closeAllSessions();
}

MainWindow *Application::newMainWindow()
{
    WindowSystemInfo::HAVE_TRANSPARENCY = !m_parser->isSet(QStringLiteral("notransparency"));

    auto window = new MainWindow();

    connect(window, &Konsole::MainWindow::newWindowRequest, this, &Konsole::Application::createWindow);

    connect(window, &Konsole::MainWindow::newTmuxWindowRequest, this, [this, window](const QString &directory) {
        createTmuxWindow(window, directory);
    });

    connect(window, &Konsole::MainWindow::detachTmuxWindowRequest, this, [this, window](int windowId) {
        detachTmuxWindow(window, windowId);
    });

    connect(window, &Konsole::MainWindow::mergeTmuxWindowRequest, this, [this, window](int windowId) {
        mergeTmuxWindow(window, windowId);
    });

    connect(window,
            &Konsole::MainWindow::terminalsDetached,
            this,
            [this, window](ViewSplitter *splitter, const QHash<TerminalDisplay *, Session *> &sessionsMap) {
                detachTerminals(window, splitter, sessionsMap);
            });

    m_pluginManager.registerMainWindow(window);

    return window;
}

void Application::createWindow(const Profile::Ptr &profile, const QString &directory, const ContainerInfo &container)
{
    MainWindow *window = newMainWindow();
    Session *session = window->createSession(profile, directory);

    // Apply inherited container context for the new window.
    // Inheritance takes priority over the profile's ContainerName setting.
    // ViewManager::createSession already handles in-process inheritance,
    // but for new windows _pluggedController is null, so we apply it here.
    if (container.isValid()) {
        session->setContainerContext(container);
    }

    window->show();
}

void Application::createTmuxWindow(MainWindow *source, const QString &directory)
{
    auto *bridge = source->findChild<TmuxProcessBridge *>();
    if (!bridge || !bridge->controller()) {
        return;
    }

    QPointer<MainWindow> sourceGuard(source);
    const QString tmuxPath = bridge->tmuxPath();
    const QStringList tmuxArgs = bridge->tmuxArgs();
    const QStringList command = bridge->command();
    const QStringList rshCommand = bridge->rshCommand();

    // Route the new tmux window to its own kmux MainWindow: hide it on the
    // source side (so the source keeps only its existing tabs) and restrict
    // the new bridge to that one window. Without this, both the source and
    // the new MainWindow would each show all tmux windows as tabs.
    bridge->controller()->requestNewWindow(directory, [this, sourceGuard, tmuxPath, tmuxArgs, command, rshCommand](int newWindowId) {
        if (newWindowId < 0 || !sourceGuard) {
            return;
        }

        if (auto *srcBridge = sourceGuard->findChild<TmuxProcessBridge *>()) {
            if (auto *srcCtrl = srcBridge->controller()) {
                srcCtrl->hideWindow(newWindowId);
            }
        }

        // newMainWindow() wires signals and registers plugins — the new tmux-
        // attached window gets everything a normally-spawned window would.
        MainWindow *window = newMainWindow();
        auto *newBridge = new TmuxProcessBridge(window->viewManager(), window);
        if (!newBridge->start(tmuxPath, tmuxArgs, command, rshCommand)) {
            delete window;
            return;
        }
        if (auto *newController = newBridge->controller()) {
            newController->showOnlyWindow(newWindowId);
        }
        window->show();
    });
}

void Application::detachTmuxWindow(MainWindow *source, int windowId)
{
    auto *bridge = source->findChild<TmuxProcessBridge *>();
    if (!bridge || !bridge->controller()) {
        return;
    }

    // Hide the window on the source side — removes its tab and stops tracking
    // it. The tmux window itself keeps running; it's just no longer shown here.
    bridge->controller()->hideWindow(windowId);

    // Spawn a new MainWindow via newMainWindow() so it gets plugins and
    // signal wiring. Attach its own bridge to the same tmux session, but
    // restricted to the detached window so it shows only that one tab.
    MainWindow *window = newMainWindow();
    auto *newBridge = new TmuxProcessBridge(window->viewManager(), window);
    if (!newBridge->start(bridge->tmuxPath(), bridge->tmuxArgs(), bridge->command(), bridge->rshCommand())) {
        delete window;
        return;
    }
    if (auto *newController = newBridge->controller()) {
        newController->showOnlyWindow(windowId);
    }
    window->show();
}

void Application::mergeTmuxWindow(MainWindow *source, int windowId)
{
    auto *sourceBridge = source->findChild<TmuxProcessBridge *>();
    if (!sourceBridge || !sourceBridge->controller()) {
        return;
    }
    auto *sourceCtrl = sourceBridge->controller();

    // Pick the target MainWindow: among same-session controllers excluding
    // ourselves, the one with the most visible tabs wins; ties go to the
    // first-registered (QList::append order).
    TmuxController *target = nullptr;
    int bestTabs = -1;
    const auto controllers = TmuxControllerRegistry::instance()->controllers();
    for (TmuxController *c : controllers) {
        if (c == sourceCtrl) {
            continue;
        }
        if (c->sessionId() != sourceCtrl->sessionId()) {
            continue;
        }
        const int tabs = c->windowToTabIndex().size();
        if (tabs > bestTabs) {
            target = c;
            bestTabs = tabs;
        }
    }

    if (!target) {
        return;
    }

    target->unhideWindow(windowId);
    source->deleteLater();
}

void Application::detachTerminals(MainWindow *currentWindow, ViewSplitter *splitter, const QHash<TerminalDisplay *, Session *> &sessionsMap)
{
    MainWindow *window = newMainWindow();
    ViewManager *manager = window->viewManager();

    const QList<TerminalDisplay *> displays = splitter->findChildren<TerminalDisplay *>();
    for (TerminalDisplay *terminal : displays) {
        manager->attachView(terminal, sessionsMap[terminal]);
    }
    manager->activeContainer()->addSplitter(splitter);

    window->show();
    window->resize(currentWindow->width(), currentWindow->height());
    window->move(QCursor::pos());
}

int Application::newInstance()
{
    // handle session management

    // returns from processWindowArgs(args, createdNewMainWindow)
    // if a new window was created
    bool createdNewMainWindow = false;

    // check for arguments to print help or other information to the
    // terminal, quit if such an argument was found
    if (processHelpArgs()) {
        return 0;
    }

    // create a new window or use an existing one
    MainWindow *window = processWindowArgs(createdNewMainWindow);

    // Spawn tmux in plain control mode (-C) as a hidden subprocess.
    // No PTY, no Session, no terminal emulation — TmuxProcessBridge
    // pipes stdout lines to TmuxGateway and stdin commands back.
    auto *bridge = new TmuxProcessBridge(window->viewManager(), window);

    // Build tmux args: [-S <socket>]
    QStringList tmuxArgs;
    if (m_parser->isSet(QStringLiteral("socket"))) {
        tmuxArgs << QStringLiteral("-S") << m_parser->value(QStringLiteral("socket"));
    }

    // Build tmux command: "new-session -A [-s <session>]"
    QStringList tmuxCommand = {QStringLiteral("new-session"), QStringLiteral("-A")};
    if (m_parser->isSet(QStringLiteral("session"))) {
        tmuxCommand << QStringLiteral("-s") << m_parser->value(QStringLiteral("session"));
    }

    // Optional remote-shell wrapper (rsync-style): --rsh overrides
    // KMUX_RSH. Split with shell-like quoting so "ssh -p 2222 host" works.
    QString rshString;
    if (m_parser->isSet(QStringLiteral("rsh"))) {
        rshString = m_parser->value(QStringLiteral("rsh"));
    } else {
        rshString = qEnvironmentVariable("KMUX_RSH");
    }
    const QStringList rshCommand = rshString.isEmpty() ? QStringList() : QProcess::splitCommand(rshString);

    const QString tmuxPath = m_parser->isSet(QStringLiteral("tmux-path")) ? m_parser->value(QStringLiteral("tmux-path")) : QString();

    if (!bridge->start(tmuxPath, tmuxArgs, tmuxCommand, rshCommand)) {
        qWarning() << "Failed to start tmux";
        delete bridge;
        return 0;
    }

    // if the background-mode argument is supplied, start the background
    // session ( or bring to the front if it already exists )
    if (m_parser->isSet(QStringLiteral("background-mode"))) {
        startBackgroundMode(window);
    } else {
        // Defer showing the window until tmux sends its first reply. If
        // --rsh is prompting for input (e.g. ssh password) on the launching
        // terminal, popping the GUI now would steal focus mid-typing.
        //
        // Qt constrains top-level windows which have not been manually
        // resized (via QWidget::resize()) to a maximum of 2/3rds of the
        //  screen size.
        //
        // This means that the terminal display might not get the width/
        // height it asks for.  To work around this, the widget must be
        // manually resized to its sizeHint().
        //
        // This problem only affects the first time the application is run.
        // run. After that KMainWindow will have manually resized the
        // window to its saved size at this point (so the Qt::WA_Resized
        // attribute will be set)

        // If not restoring size from last time or only adding new tab,
        // resize window to chosen profile size (see Bug:345403)
        QPointer<MainWindow> windowGuard(window);
        connect(bridge, &TmuxProcessBridge::ready, window, [windowGuard, createdNewMainWindow]() {
            if (!windowGuard) {
                return;
            }
            if (createdNewMainWindow) {
                windowGuard->show();
            } else {
                windowGuard->setWindowState(windowGuard->windowState() & (~Qt::WindowMinimized | Qt::WindowActive));
                windowGuard->show();
                windowGuard->activateWindow();
            }
        });
    }

    return 1;
}

/* Documentation for tab file:
 *
 * ;; is the token separator
 * # at the beginning of line results in line being ignored.
 * supported tokens: title, command, profile and workdir
 *
 * Note that the title is static and the tab will close when the
 * command is complete (do not use --noclose).  You can start new tabs.
 *
 * Example below will create 6 tabs as listed and a 7th default tab
title: This is the title;; command: ssh localhost
title: This is the title;; command: ssh localhost;; profile: Shell
title: Top this!;; command: top
title: mc this!;; command: mc;; workdir: /tmp
#this line is comment
command: ssh localhost
profile: Shell
*/
bool Application::processTabsFromFileArgs(MainWindow *window)
{
    // Open tab configuration file
    const QString tabsFileName(m_parser->value(QStringLiteral("tabs-from-file")));
    QFile tabsFile(tabsFileName);
    if (!tabsFile.open(QFile::ReadOnly)) {
        qWarning() << "ERROR: Cannot open tabs file " << tabsFileName.toLocal8Bit().data();
        return false;
    }

    unsigned int sessions = 0;
    while (!tabsFile.atEnd()) {
        QString lineString(QString::fromUtf8(tabsFile.readLine()).trimmed());
        if ((lineString.isEmpty()) || (lineString[0] == QLatin1Char('#'))) {
            continue;
        }

        QHash<QString, QString> lineTokens;
        QStringList lineParts = lineString.split(QStringLiteral(";;"), Qt::SkipEmptyParts);

        for (int i = 0; i < lineParts.size(); ++i) {
            QString key = lineParts.at(i).section(QLatin1Char(':'), 0, 0).trimmed().toLower();
            QString value = lineParts.at(i).section(QLatin1Char(':'), 1, -1).trimmed();
            lineTokens[key] = value;
        }
        // should contain at least one of 'command' and 'profile'
        if (lineTokens.contains(QStringLiteral("command")) || lineTokens.contains(QStringLiteral("profile"))) {
            createTabFromArgs(window, lineTokens);
            sessions++;
        } else {
            qWarning() << "Each line should contain at least one of 'command' and 'profile'.";
        }
    }
    tabsFile.close();

    if (sessions < 1) {
        qWarning() << "No valid lines found in " << tabsFileName.toLocal8Bit().data();
        return false;
    }

    return true;
}

void Application::createTabFromArgs(MainWindow *window, const QHash<QString, QString> &tokens)
{
    const QString &title = tokens[QStringLiteral("title")];
    const QString &command = tokens[QStringLiteral("command")];
    const QString &profile = tokens[QStringLiteral("profile")];
    const QString &color = tokens[QStringLiteral("tabcolor")];
    const QString &activityColor = tokens[QStringLiteral("tabactivitycolor")];

    Profile::Ptr baseProfile;
    if (!profile.isEmpty()) {
        baseProfile = ProfileManager::instance()->loadProfile(profile);
    }
    if (!baseProfile) {
        // fallback to default profile
        baseProfile = ProfileManager::instance()->defaultProfile();
    }

    Profile::Ptr newProfile = Profile::Ptr(new Profile(baseProfile));
    newProfile->setHidden(true);
    newProfile->setProperty(Profile::Name, ProfileManager::instance()->generateUniqueName());

    // FIXME: the method of determining whether to use newProfile does not
    // scale well when we support more fields in the future
    bool shouldUseNewProfile = false;

    if (!command.isEmpty()) {
        newProfile->setProperty(Profile::Command, command);
        newProfile->setProperty(Profile::Arguments, command.split(QLatin1Char(' ')));
        shouldUseNewProfile = true;
    }

    if (!title.isEmpty()) {
        newProfile->setProperty(Profile::LocalTabTitleFormat, title);
        newProfile->setProperty(Profile::RemoteTabTitleFormat, title);
        shouldUseNewProfile = true;
    }

    // For tab color support
    if (!color.isEmpty() && QColor::isValidColorName(color)) {
        newProfile->setProperty(Profile::TabColor, QColor::fromString(color));
        shouldUseNewProfile = true;
    }

    // For tab color support
    if (!activityColor.isEmpty() && QColor::isValidColorName(activityColor)) {
        newProfile->setProperty(Profile::TabActivityColor, QColor::fromString(activityColor));
        shouldUseNewProfile = true;
    }

    // Create the new session
    Profile::Ptr theProfile = shouldUseNewProfile ? newProfile : baseProfile;
    Session *session = window->createSession(theProfile, QString());

    const QString wdirOptionName(QStringLiteral("workdir"));
    auto it = tokens.constFind(wdirOptionName);
    const QString workingDirectory = it != tokens.cend() ? it.value() : m_parser->value(wdirOptionName);

    if (!workingDirectory.isEmpty()) {
        session->setInitialWorkingDirectory(workingDirectory);
    }

    if (m_parser->isSet(QStringLiteral("noclose"))) {
        session->setAutoClose(false);
    }

    if (!window->testAttribute(Qt::WA_Resized)) {
        window->resize(window->sizeHint());
    }

    // FIXME: this ugly hack here is to make the session start running, so that
    // its tab title is displayed as expected.
    //
    // This is another side effect of the commit fixing BKO 176902.
    window->show();
    window->hide();
}

// Creates a new Konsole window.
// If --new-tab is given, use existing window.
MainWindow *Application::processWindowArgs(bool &createdNewMainWindow)
{
    MainWindow *window = nullptr;

    if (Konsole::KonsoleSettings::forceNewTabs() || m_parser->isSet(QStringLiteral("new-tab"))) {
        const QList<QWidget *> list = QApplication::topLevelWidgets();
        for (auto it = list.crbegin(), endIt = list.crend(); it != endIt; ++it) {
            window = qobject_cast<MainWindow *>(*it);
            if (window) {
                break;
            }
        }
    }

    if (window == nullptr) {
        createdNewMainWindow = true;
        window = newMainWindow();

        // override default menubar visibility
        if (m_parser->isSet(QStringLiteral("show-menubar"))) {
            window->setMenuBarInitialVisibility(true);
        }
        if (m_parser->isSet(QStringLiteral("hide-menubar"))) {
            window->setMenuBarInitialVisibility(false);
        }

        // override default toolbars visibility
        if (m_parser->isSet(QStringLiteral("show-toolbars"))) {
            window->setToolBarsInitialVisibility(true);
        }
        if (m_parser->isSet(QStringLiteral("hide-toolbars"))) {
            window->setToolBarsInitialVisibility(false);
        }

        // override default tabbar visibility
        if (m_parser->isSet(QStringLiteral("show-tabbar"))) {
            window->viewManager()->setNavigationVisibility(ViewManager::AlwaysShowNavigation);
        } else if (m_parser->isSet(QStringLiteral("hide-tabbar"))) {
            window->viewManager()->setNavigationVisibility(ViewManager::AlwaysHideNavigation);
        }

        if (m_parser->isSet(QStringLiteral("fullscreen"))) {
            window->viewFullScreen(true);
        }
    }
    return window;
}

// Loads a profile.
// If --profile <name> is given, loads profile <name>.
// If --builtin-profile is given, loads built-in profile.
// Else loads the default profile.
Profile::Ptr Application::processProfileSelectArgs()
{
    if (m_parser->isSet(QStringLiteral("profile"))) {
        Profile::Ptr profile = ProfileManager::instance()->loadProfile(m_parser->value(QStringLiteral("profile")));
        if (profile) {
            return profile;
        }
    } else if (m_parser->isSet(QStringLiteral("builtin-profile"))) {
        // no need to check twice: built-in and default profiles are always available
        return ProfileManager::instance()->builtinProfile();
    }
    return ProfileManager::instance()->defaultProfile();
}

bool Application::processHelpArgs()
{
    if (m_parser->isSet(QStringLiteral("list-profiles"))) {
        listAvailableProfiles();
        return true;
    } else if (m_parser->isSet(QStringLiteral("list-profile-properties"))) {
        listProfilePropertyInfo();
        return true;
    }
    return false;
}

void Application::listAvailableProfiles()
{
    const QStringList paths = ProfileManager::instance()->availableProfilePaths();

    for (const QString &path : paths) {
        QFileInfo info(path);
        printf("%s\n", info.completeBaseName().toLocal8Bit().constData());
    }
}

void Application::listProfilePropertyInfo()
{
    const std::vector<std::string> &properties = Profile::propertiesInfoList();

    for (const auto &prop : properties) {
        printf("%s\n", prop.c_str());
    }
}

Profile::Ptr Application::processProfileChangeArgs(Profile::Ptr baseProfile)
{
    bool shouldUseNewProfile = false;

    Profile::Ptr newProfile = Profile::Ptr(new Profile(baseProfile));
    newProfile->setHidden(true);

    // temporary changes to profile options specified on the command line
    const QStringList profileProperties = m_parser->values(QStringLiteral("p"));
    for (const QString &value : profileProperties) {
        ProfileCommandParser parser;
        newProfile->assignProperties(parser.parse(value));
        shouldUseNewProfile = true;
    }

    // run a custom command
    if (!m_customCommand.isEmpty()) {
        // Example: konsole -e man ls
        QString commandExec = m_customCommand[0];
        QStringList commandArguments(m_customCommand);
        if ((m_customCommand.size() == 1) && (QStandardPaths::findExecutable(commandExec).isEmpty())) {
            // Example: konsole -e "man ls"
            ShellCommand shellCommand(commandExec);
            commandExec = shellCommand.command();
            commandArguments = shellCommand.arguments();
        }

        if (commandExec.startsWith(QLatin1String("./"))) {
            commandExec = QDir::currentPath() + commandExec.mid(1);
        }

        newProfile->setProperty(Profile::Command, commandExec);
        newProfile->setProperty(Profile::Arguments, commandArguments);

        shouldUseNewProfile = true;
    }

    if (shouldUseNewProfile) {
        return newProfile;
    }
    return baseProfile;
}

void Application::startBackgroundMode(MainWindow *window)
{
    if (_backgroundInstance != nullptr) {
        return;
    }

#if HAVE_DBUS
    KActionCollection *collection = window->actionCollection();
    QAction *action = collection->addAction(QStringLiteral("toggle-background-window"));
    action->setObjectName(QStringLiteral("Konsole Background Mode"));
    action->setText(i18nc("@item", "Toggle Background Window"));
    KGlobalAccel::self()->setGlobalShortcut(action, QKeySequence(Konsole::ACCEL | Qt::Key_F12));
    connect(action, &QAction::triggered, this, &Application::toggleBackgroundInstance);
#endif
    _backgroundInstance = window;
}

void Application::toggleBackgroundInstance()
{
    Q_ASSERT(_backgroundInstance);

    if (!_backgroundInstance->isVisible()) {
        _backgroundInstance->show();
        // ensure that the active terminal display has the focus. Without
        // this, an odd problem occurred where the focus widget would change
        // each time the background instance was shown
        _backgroundInstance->setFocus();
    } else {
        _backgroundInstance->hide();
    }
}

void Application::slotActivateRequested(QStringList args, const QString & /*workingDir*/)
{
    // QCommandLineParser expects the first argument to be the executable name
    // In the current version it just strips it away
    args.prepend(qApp->applicationFilePath());

    m_customCommand = getCustomCommand(args);

    // We can't reuse QCommandLineParser instances, it preserves earlier
    // parsed values
    auto parser = new QCommandLineParser;
    populateCommandLineParser(parser);
    parser->parse(args);
    m_parser.reset(parser);

    newInstance();
}

#include "moc_Application.cpp"
