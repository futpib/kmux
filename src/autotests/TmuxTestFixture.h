/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXTESTFIXTURE_H
#define TMUXTESTFIXTURE_H

#include <QList>
#include <QPair>
#include <QPointer>
#include <QString>

#include <initializer_list>

namespace Konsole
{

class MainWindow;
class TabbedViewContainer;
class TmuxProcessBridge;
class ViewManager;

namespace TmuxTestFixture
{

struct PaneSpec {
    QString command;
    int columns = 14;
    int lines = 3;
    bool focused = false;
};

struct LayoutSpec {
    enum Type {
        Leaf,
        HSplit,
        VSplit
    };
    Type type = Leaf;
    PaneSpec pane;
    QList<LayoutSpec> children;
};

struct SessionContext {
    QString sessionName;
    QString socketPath;
};

struct AttachResult {
    QPointer<MainWindow> mw;
    TmuxProcessBridge *bridge = nullptr;
    QPointer<TabbedViewContainer> container;
};

LayoutSpec pane(const QString &command = {}, int columns = 14, int lines = 3, bool focused = false);
LayoutSpec horizontal(std::initializer_list<LayoutSpec> children);
LayoutSpec vertical(std::initializer_list<LayoutSpec> children);

void setupSinglePane(const QString &command, const QString &tmuxPath, const QString &socketDir, SessionContext &ctx, int columns = 14, int lines = 3);

// Create a detached tmux session matching the typed layout, then verify its
// pane count and dimensions. Each call uses an isolated tmux server socket.
void setupTmuxSession(const LayoutSpec &layout, const QString &tmuxPath, const QString &socketDir, SessionContext &ctx);

void attachKonsole(const QString &tmuxPath, const SessionContext &ctx, AttachResult &result);
void applyKonsoleLayout(const LayoutSpec &layout, ViewManager *vm);
void assertKonsoleLayout(const LayoutSpec &layout, ViewManager *vm);
void killTmuxSession(const QString &tmuxPath, const SessionContext &ctx);
QString findTmuxOrSkip();

int countPanes(const LayoutSpec &layout);
QPair<int, int> computeWindowSize(const LayoutSpec &layout);

} // namespace TmuxTestFixture

} // namespace Konsole

#endif // TMUXTESTFIXTURE_H
