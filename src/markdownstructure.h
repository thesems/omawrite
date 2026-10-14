#pragma once

#include <QFont>
#include <QFontInfo>
#include <QRegularExpression>

// Source positions stay shared by layout, highlighting and checklist controls.
namespace MarkdownStructure {
inline QRegularExpressionMatch listPrefix(const QString &text) {
    static const QRegularExpression rule(QStringLiteral("^[ \\t]{0,3}([-*_])(?:[ \\t]*\\1){2,}[ \\t]*$"));
    static const QRegularExpression list(QStringLiteral("^([ \\t]*)([-+*]|\\d{1,9}(?:\\.\\d{1,9})*[.)])([ \\t]+)"));
    return rule.match(text).hasMatch() ? QRegularExpressionMatch() : list.match(text);
}

inline QRegularExpressionMatch checklist(const QString &text) {
    static const QRegularExpression task(
        QStringLiteral("^([ \\t]*(?:[-+*]|\\d{1,9}(?:\\.\\d{1,9})*[.)])[ \\t]+)(\\[([ xX])\\])(?=[ \\t]|$)([ \\t]*)"));
    return task.match(text);
}

inline qreal checkboxWidth(const QFont &font) {
    const int pixels = font.pixelSize() > 0 ? font.pixelSize() : QFontInfo(font).pixelSize();
    return qMax(qreal(12), pixels * 0.72) + 1;
}

struct FenceLine { int nextState; bool code; };
inline FenceLine fenceLine(const QString &text, int previousState) {
    const int state = qMax(0, previousState);
    static const QRegularExpression marker(QStringLiteral("^[ \\t]*(`{3,}|~{3,})(.*)$"));
    const auto match = marker.match(text);
    if (!match.hasMatch()) return {state, state != 0};
    const QString run = match.captured(1);
    const bool tilde = run.front() == QLatin1Char('~');
    if (state == 0) {
        if (!tilde && match.captured(2).contains(QLatin1Char('`'))) return {0, false};
        return {(int(run.size()) << 1) | int(tilde), true};
    }
    if (tilde == bool(state & 1) && run.size() >= (state >> 1)
            && match.captured(2).trimmed().isEmpty())
        return {0, true};
    return {state, true};
}
}
