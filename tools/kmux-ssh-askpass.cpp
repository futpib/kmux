/*
    SPDX-FileCopyrightText: 2026 kmux contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QTextStream>

#include <KLocalizedString>

namespace
{
bool isConfirmationPrompt(const QString &prompt)
{
    return prompt.contains(QLatin1String("yes/no"), Qt::CaseInsensitive) || prompt.contains(QLatin1String("continue connecting"), Qt::CaseInsensitive);
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("kmux-ssh-askpass"));
    KLocalizedString::setApplicationDomain("kmux");

    const QString prompt = argc > 1 ? QString::fromLocal8Bit(argv[1]) : i18n("SSH authentication requested");
    QTextStream output(stdout);

    if (isConfirmationPrompt(prompt)) {
        const auto answer =
            QMessageBox::question(nullptr, i18nc("@title:window", "SSH Host Verification"), prompt, QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return 1;
        }
        output << "yes\n";
        output.flush();
        return 0;
    }

    bool accepted = false;
    const QString response = QInputDialog::getText(nullptr, i18nc("@title:window", "SSH Authentication"), prompt, QLineEdit::Password, QString(), &accepted);
    if (!accepted) {
        return 1;
    }

    output << response << Qt::endl;
    return 0;
}
