/*
    SPDX-FileCopyrightText: 2026 kmux contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXRSHPROCESS_H
#define TMUXRSHPROCESS_H

#include <KPtyProcess>

class QDialog;
class QTimer;

namespace Konsole
{
class TerminalDisplay;
class VirtualSession;

// Supplies a local terminal for rsh authentication without putting tmux's
// stdout protocol through terminal emulation or terminal output processing.
class TmuxRshProcess : public KPtyProcess
{
    Q_OBJECT
public:
    explicit TmuxRshProcess(const QString &command, QObject *parent = nullptr);
    ~TmuxRshProcess() override;

    void receiveOutput(const QByteArray &data);
    void finishAuthentication();
    void closePrompt();
    void killTransport();
    void writeControlData(const QByteArray &data);
    QByteArray diagnosticOutput();

Q_SIGNALS:
    void promptShown();

private:
    void readTerminal();
    void showPrompt();

    QDialog *_dialog;
    TerminalDisplay *_display;
    VirtualSession *_session;
    QTimer *_promptTimer;
    QByteArray _diagnosticOutput;
    QByteArray _pendingControlData;
    bool _authenticating = true;
};
}

#endif
