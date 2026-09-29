/*
    SPDX-FileCopyrightText: 2026 kmux contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxRshProcess.h"

#include "Emulation.h"
#include "profile/Profile.h"
#include "profile/ProfileManager.h"
#include "session/SessionController.h"
#include "session/SessionManager.h"
#include "session/VirtualSession.h"
#include "terminalDisplay/TerminalDisplay.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

#include <KLocalizedString>
#include <KPtyDevice>

#include <csignal>
#include <termios.h>
#include <utility>

namespace Konsole
{
TmuxRshProcess::TmuxRshProcess(const QString &command, QObject *parent)
    : KPtyProcess(parent)
    , _dialog(new QDialog())
    , _display(new TerminalDisplay(_dialog))
    , _session(SessionManager::instance()->createVirtualSession(ProfileManager::instance()->defaultProfile()))
    , _promptTimer(new QTimer(this))
{
    _session->setParent(this);
    setPtyChannels(StdinChannel | StderrChannel);
    _dialog->setAttribute(Qt::WA_QuitOnClose, false);
    _dialog->setWindowTitle(i18nc("@title:window", "Remote Shell — %1", command));
    _dialog->setObjectName(QStringLiteral("tmuxRshPrompt"));
    auto *layout = new QVBoxLayout(_dialog);
    auto *label = new QLabel(i18n("Complete the remote connection below. This window closes when tmux connects."), _dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    layout->addWidget(_display);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, _dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, _dialog, &QDialog::reject);
    connect(_dialog, &QDialog::rejected, this, &TmuxRshProcess::killTransport);

    const auto profile = ProfileManager::instance()->defaultProfile();
    new SessionController(_session, _display, _display);
    _display->applyProfile(profile);
    _session->addView(_display);
    _display->setSize(80, 16);
    _session->setKeyBindings(profile->keyBindings());
    _session->setHistorySize(0);
    _dialog->resize(_dialog->sizeHint());
    pty()->setWinSize(16, 80);
    connect(_session->emulation(), &Emulation::imageSizeChanged, this, [this](int lines, int columns) {
        pty()->setWinSize(lines, columns);
    });
    connect(_session->emulation(), &Emulation::sendData, this, [this](const QByteArray &data) {
        if (_authenticating && state() == QProcess::Running) {
            pty()->write(data);
        }
    });
    connect(pty(), &KPtyDevice::readyRead, this, &TmuxRshProcess::readTerminal);
    _promptTimer->setSingleShot(true);
    connect(_promptTimer, &QTimer::timeout, this, &TmuxRshProcess::showPrompt);
    connect(this, &QProcess::started, this, [this]() {
        // Even a wrapper reading silently needs an input window. Explicit
        // askpass users already have their own UI for that case.
        if (processEnvironment().value(QStringLiteral("SSH_ASKPASS_REQUIRE")) != QLatin1String("force")) {
            _promptTimer->start(1000);
        }
    });
}

TmuxRshProcess::~TmuxRshProcess()
{
    if (state() != QProcess::NotRunning) {
        killTransport();
        waitForFinished(2000);
    }
    delete _dialog;
}

void TmuxRshProcess::receiveOutput(const QByteArray &data)
{
    if (!_authenticating || data.isEmpty()) {
        return;
    }
    _session->injectData(data.constData(), data.size());
    showPrompt();
}

void TmuxRshProcess::showPrompt()
{
    if (!_authenticating || state() != QProcess::Running) {
        return;
    }
    if (!_dialog->isVisible()) {
        _dialog->show();
        _dialog->activateWindow();
        _display->setFocus(Qt::OtherFocusReason);
        Q_EMIT promptShown();
    }
}

void TmuxRshProcess::readTerminal()
{
    const QByteArray data = pty()->readAll();
    // Like QProcess stderr, retain diagnostics for a failed connection.
    // Passwords are never echoed by a properly configured terminal reader.
    _diagnosticOutput.append(data);
    if (_diagnosticOutput.size() > 65536) {
        _diagnosticOutput = _diagnosticOutput.right(65536);
    }
    receiveOutput(data);
}

QByteArray TmuxRshProcess::diagnosticOutput()
{
    readTerminal();
    return _diagnosticOutput;
}

void TmuxRshProcess::finishAuthentication()
{
    closePrompt();
    _diagnosticOutput.clear();
    // stdin now carries tmux commands. Disable echo, canonical line limits,
    // and signal/flow-control interpretation before sending any protocol data.
    struct termios attributes;
    if (pty()->tcGetAttr(&attributes)) {
        cfmakeraw(&attributes);
        pty()->tcSetAttr(&attributes);
    }
    pty()->write(std::exchange(_pendingControlData, {}));
}

void TmuxRshProcess::closePrompt()
{
    _authenticating = false;
    _promptTimer->stop();
    _dialog->hide();
}

void TmuxRshProcess::killTransport()
{
    closePrompt();
    if (state() != QProcess::NotRunning && processId() > 0) {
        // KPtyProcess makes the child a session/process-group leader.
        // Cancel wrappers and their authentication children together.
        ::kill(-processId(), SIGKILL);
        kill();
    }
}

void TmuxRshProcess::writeControlData(const QByteArray &data)
{
    // Existing views can enqueue resizes or other commands during reconnect.
    // Never let those bytes reach a wrapper still reading authentication input.
    if (_authenticating) {
        _pendingControlData.append(data);
    } else {
        pty()->write(data);
    }
}
}

#include "moc_TmuxRshProcess.cpp"
