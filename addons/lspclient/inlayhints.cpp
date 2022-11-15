#include "inlayhints.h"

#include <KSyntaxHighlighting/Theme>
#include <KTextEditor/InlineNote>
#include <KTextEditor/InlineNoteInterface>
#include <KTextEditor/InlineNoteProvider>

#include <QApplication>
#include <QPainter>

#include "lspclientservermanager.h"
#include <QSet>
#include <ktexteditor_utils.h>

static std::size_t hash_combine(std::size_t seed, std::size_t v)
{
    seed ^= v + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    return seed;
}

size_t qHash(const LSPInlayHint &s, size_t seed = 0)
{
    std::size_t h1 = hash_combine(seed, qHash(s.position));
    return hash_combine(h1, qHash(s.label));
}

[[maybe_unused]] static bool operator==(const LSPInlayHint &l, const LSPInlayHint &r)
{
    return l.position == r.position && l.label == r.label;
}

template<typename T>
static auto binaryFind(T &&hints, int line)
{
    auto it = std::lower_bound(hints.begin(), hints.end(), line, [](const LSPInlayHint &h, int l) {
        return h.position.line() < l;
    });
    if (it != hints.end() && it->position.line() == line) {
        return it;
    }
    return hints.end();
}

static auto binaryFind(const QVector<LSPInlayHint> &hints, KTextEditor::Cursor pos)
{
    auto it = std::lower_bound(hints.begin(), hints.end(), pos, [](const LSPInlayHint &h, KTextEditor::Cursor p) {
        return h.position < p;
    });
    if (it != hints.end() && it->position == pos) {
        return it;
    }
    return hints.end();
}

InlayHintNoteProvider::InlayHintNoteProvider()
{
}

void InlayHintNoteProvider::setView(KTextEditor::View *v)
{
    m_view = v;
    if (v) {
        m_noteColor = QColor::fromRgba(v->theme().textColor(KSyntaxHighlighting::Theme::Normal));
        m_noteColor.setAlphaF(0.5);
    }
    m_hints = {};
}

void InlayHintNoteProvider::setHints(const QVector<LSPInlayHint> &hints)
{
    m_hints = hints;
}

QVector<int> InlayHintNoteProvider::inlineNotes(int line) const
{
    QVector<int> ret;
    auto it = binaryFind(std::as_const(m_hints), line);
    while (it != m_hints.cend() && it->position.line() == line) {
        ret.push_back(it->position.column());
        ++it;
    }
    return ret;
}

QSize InlayHintNoteProvider::inlineNoteSize(const KTextEditor::InlineNote &note) const
{
    const auto font = qApp->font();
    const auto fm = QFontMetrics(font);
    const auto pos = note.position();
    for (const auto &hint : std::as_const(m_hints)) {
        if (hint.position == pos) {
            return {fm.horizontalAdvance(hint.label), note.lineHeight()};
        }
    }
    return {};
}

void InlayHintNoteProvider::paintInlineNote(const KTextEditor::InlineNote &note, QPainter &painter) const
{
    painter.setPen(m_noteColor);
    const auto font = qApp->font();
    painter.setFont(font);
    auto it = binaryFind(m_hints, note.position());
    if (it != m_hints.end()) {
        QRectF r{0., 0., note.width(), (qreal)note.lineHeight()};
        painter.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, it->label);
    }
}

InlayHintsManager::InlayHintsManager(const QSharedPointer<LSPClientServerManager> &manager, QObject *parent)
    : QObject(parent)
    , m_serverManager(manager)
{
    m_requestTimer.setInterval(300);
    m_requestTimer.setSingleShot(true);
    m_requestTimer.callOnTimeout(this, &InlayHintsManager::sendRequest);
}

void InlayHintsManager::registerView(KTextEditor::View *v)
{
    using namespace KTextEditor;
    if (v) {
        m_currentView = v;
        qobject_cast<InlineNoteInterface *>(m_currentView)->registerInlineNoteProvider(&m_noteProvider);
        m_noteProvider.setView(v);
        auto d = v->document();

        connect(d, &Document::reloaded, this, [this](Document *d) {
            if (m_currentView && m_currentView->document() == d) {
                m_hintDataByDoc.erase(std::remove_if(m_hintDataByDoc.begin(),
                                                     m_hintDataByDoc.end(),
                                                     [d](const HintData &hd) {
                                                         return hd.doc == d;
                                                     }),
                                      m_hintDataByDoc.end());
                sendRequestDelayed(d->documentRange(), 50);
            }
        });

        connect(d, &Document::textInserted, this, [this](Document *doc, const Cursor &pos, const QString &text) {
            if (pos.line() > doc->lines()) {
                return;
            }
            // normal typing is size==1, with undo / paste it can be bigger
            // for which we should rerequest the server
            m_insertCount += text.size();

            auto it = std::find_if(m_hintDataByDoc.begin(), m_hintDataByDoc.end(), [doc](const HintData &hd) {
                return hd.doc == doc;
            });
            if (it != m_hintDataByDoc.end()) {
                auto &list = it->m_hints;
                bool changed = false;
                auto it = binaryFind(list, pos.line());
                for (; it != list.end(); ++it) {
                    if (it->position.line() > pos.line()) {
                        break;
                    }
                    if (it->position >= pos) {
                        it->position.setColumn(it->position.column() + text.size());
                        changed = true;
                    }
                }
                if (changed) {
                    m_noteProvider.setHints(list);
                }
            }

            if (m_insertCount > 5 || text.size() > 1) {
                Range r(pos.line(), 0, pos.line(), doc->lineLength(pos.line()));
                sendRequestDelayed(r);
                m_insertCount = 0;
            }
        });

        connect(d, &Document::textRemoved, this, [this](Document *doc, const Range &range, const QString &t) {
            if (range.onSingleLine()) {
                auto it = std::find_if(m_hintDataByDoc.begin(), m_hintDataByDoc.end(), [doc](const HintData &hd) {
                    return hd.doc == doc;
                });
                if (it != m_hintDataByDoc.end()) {
                    auto &list = it->m_hints;
                    auto bit = binaryFind(list, range.start().line());
                    auto bitCopy = bit;
                    auto end = list.end();
                    bool changed = false;
                    for (; bit != list.end(); ++bit) {
                        if (bit->position.line() > range.start().line()) {
                            end = bit;
                            break;
                        }
                        if (range.contains(bit->position)) {
                            // was inside range? remove this note
                            bit->position = Cursor::invalid();
                            changed = true;
                        } else if (bit->position > range.end()) {
                            // in front? => adjust position
                            bit->position.setColumn(bit->position.column() - t.size());
                            changed = true;
                        }
                    }
                    if (changed) {
                        list.erase(std::remove_if(bitCopy,
                                                  end,
                                                  [](const LSPInlayHint &h) {
                                                      return !h.position.isValid();
                                                  }),
                                   end);
                        m_noteProvider.setHints(list);
                    }

                    if (t.size() > 1) {
                        // non trivial text removal? => request new
                        // could be for e.g., uncommenting
                        Range r(range.start().line(), 0, range.end().line(), doc->lineLength(range.end().line()));
                        sendRequestDelayed(r);
                    }
                }
            } else {
                Range r(range.start().line(), 0, range.end().line(), doc->lineLength(range.end().line()));
                sendRequestDelayed(r);
            }
        });

        connect(d, &Document::lineWrapped, this, [this](KTextEditor::Document *doc, const KTextEditor::Cursor &position) {
            auto it = std::find_if(m_hintDataByDoc.begin(), m_hintDataByDoc.end(), [doc](const HintData &hd) {
                return hd.doc == doc;
            });
            if (it != m_hintDataByDoc.end()) {
                auto bit = std::lower_bound(it->m_hints.begin(), it->m_hints.end(), position.line(), [](const LSPInlayHint &h, int l) {
                    return h.position.line() < l;
                });
                while (bit != it->m_hints.end()) {
                    if (bit->position >= position) {
                        break;
                    }
                    ++bit;
                }
                bool changed = bit != it->m_hints.end();
                while (bit != it->m_hints.end()) {
                    bit->position.setLine(bit->position.line() + 1);
                    ++bit;
                }

                if (changed) {
                    m_noteProvider.setHints(it->m_hints);
                }
            }
        });

        connect(d, &Document::lineUnwrapped, this, [this](KTextEditor::Document *doc, int line) {
            auto it = std::find_if(m_hintDataByDoc.begin(), m_hintDataByDoc.end(), [doc](const HintData &hd) {
                return hd.doc == doc;
            });

            if (it != m_hintDataByDoc.end()) {
                auto bit = std::lower_bound(it->m_hints.begin(), it->m_hints.end(), line, [](const LSPInlayHint &h, int l) {
                    return h.position.line() < l;
                });
                bool changed = bit != it->m_hints.end();
                while (bit != it->m_hints.end()) {
                    bit->position.setLine(bit->position.line() - 1);
                    ++bit;
                }
                if (changed) {
                    m_noteProvider.setHints(it->m_hints);
                }
            }
        });

        auto it = std::find_if(m_hintDataByDoc.begin(), m_hintDataByDoc.end(), [doc = v->document()](const HintData &hd) {
            return hd.doc == doc;
        });

        // If the document was found and checksum hasn't changed
        if (it != m_hintDataByDoc.end() && it->checksum == d->checksum()) {
            m_noteProvider.setHints(it->m_hints);
        } else {
            // Send delayed request for inlay hints
            sendRequestDelayed(v->document()->documentRange(), 100);
        }
    }

    clearHintsForInvalidDocs();
}

void InlayHintsManager::unregisterView(KTextEditor::View *v)
{
    if (v) {
        v->disconnect(this);
        v->document()->disconnect(this);
        qobject_cast<KTextEditor::InlineNoteInterface *>(m_currentView)->unregisterInlineNoteProvider(&m_noteProvider);
        auto it = std::find_if(m_hintDataByDoc.begin(), m_hintDataByDoc.end(), [doc = v->document()](const HintData &hd) {
            return hd.doc == doc;
        });
        // update checksum
        // we use it to check if doc was changed when restoring hints
        if (it != m_hintDataByDoc.end()) {
            it->checksum = v->document()->checksum();
        }
    }
    m_insertCount = 0;
    m_noteProvider.setView(nullptr);
}

void InlayHintsManager::onViewChanged(KTextEditor::View *v)
{
    if (v == m_currentView) {
        return;
    }

    unregisterView(m_currentView);
    registerView(v);
}

void InlayHintsManager::sendRequestDelayed(KTextEditor::Range r, int delay)
{
    m_requestData.r = r;
    m_requestTimer.start(delay);
}

void InlayHintsManager::sendRequest()
{
    if (!m_currentView || !m_currentView->document()) {
        return;
    }
    if (!m_requestData.r.isValid()) {
        qWarning() << Q_FUNC_INFO << "Unexpected invalid range";
        return;
    }

    const auto url = m_currentView->document()->url();

    auto v = m_currentView;
    auto server = m_serverManager->findServer(v, false);
    if (server) {
        server->documentInlayHint(url, m_requestData.r, this, [v = QPointer(m_currentView), this](const QVector<LSPInlayHint> &hints) {
            if (v) {
                const auto result = insertHintsForDoc(v->document(), hints);
                m_noteProvider.setHints(result.addedHints);
                if (result.newDoc) {
                    m_noteProvider.inlineNotesReset();
                } else {
                    // qDebug() << "hints: " << hints.size() << "changed lines: " << result.changedLines.size();
                    for (const auto &line : result.changedLines) {
                        m_noteProvider.inlineNotesChanged(line);
                    }
                }
            }
        });
    }
}

void InlayHintsManager::clearHintsForInvalidDocs()
{
    m_hintDataByDoc.erase(std::remove_if(m_hintDataByDoc.begin(),
                                         m_hintDataByDoc.end(),
                                         [](const HintData &hd) {
                                             return !hd.doc;
                                         }),
                          m_hintDataByDoc.end());
}

InlayHintsManager::InsertResult InlayHintsManager::insertHintsForDoc(KTextEditor::Document *doc, const QVector<LSPInlayHint> &newHints)
{
    auto it = std::find_if(m_hintDataByDoc.begin(), m_hintDataByDoc.end(), [doc](const HintData &hd) {
        return hd.doc == doc;
    });
    // New document
    if (it == m_hintDataByDoc.end()) {
        auto &r = m_hintDataByDoc.emplace_back();
        r = HintData{doc, doc->checksum(), newHints};
        std::sort(r.m_hints.begin(), r.m_hints.end(), [](const LSPInlayHint &l, const LSPInlayHint &r) {
            return l.position < r.position;
        });
        return {true, {}, r.m_hints};
    }
    // Old
    auto &existing = it->m_hints;
    if (newHints.isEmpty()) {
        const auto r = m_requestData.r;
        auto bit = std::lower_bound(existing.begin(), existing.end(), r.start().line(), [](const LSPInlayHint &h, int l) {
            return h.position.line() < l;
        });
        if (bit != existing.end()) {
            QSet<int> affectedLines;
            existing.erase(std::remove_if(bit,
                                          existing.end(),
                                          [r, &affectedLines](const LSPInlayHint &h) {
                                              bool contains = r.contains(h.position);
                                              if (contains) {
                                                  affectedLines.insert(h.position.line());
                                                  return true;
                                              }
                                              return false;
                                          }),
                           existing.end());
            return {false, {affectedLines.begin(), affectedLines.end()}, existing};
        }
        return {};
    }

    QSet<int> affectedLines;
    for (const auto &newHint : newHints) {
        affectedLines.insert(newHint.position.line());
    }

    QSet<LSPInlayHint> rangesToInsert(newHints.begin(), newHints.end());

    // remove existing hints that contain affectedLines
    existing.erase(std::remove_if(existing.begin(),
                                  existing.end(),
                                  [&affectedLines, &rangesToInsert](const LSPInlayHint &h) {
                                      if (affectedLines.contains(h.position.line())) {
                                          auto i = rangesToInsert.find(h);
                                          if (i != rangesToInsert.end()) {
                                              // if this range already exists then it doesn't need to be reinserted
                                              rangesToInsert.erase(i);
                                              return false;
                                          }
                                          return true;
                                      }
                                      return false;
                                  }),
                   existing.end());

    // now add new ones
    affectedLines.clear();
    for (const auto &h : rangesToInsert) {
        existing.append(h);
        // update affectedLines with lines that are actually changed
        affectedLines.insert(h.position.line());
    }

    //  and sort them
    std::sort(existing.begin(), existing.end(), [](const LSPInlayHint &l, const LSPInlayHint &r) {
        return l.position < r.position;
    });

    return {false, {affectedLines.begin(), affectedLines.end()}, existing};
}
