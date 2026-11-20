#include "cropoverlay.h"

#include "components/actionmanager/actionmanager.h"
#include "gui/overlays/cropanswer.h"
#include "gui/viewers/viewerwidget.h"

// TODO: this is pretty old. Clean up

CropOverlay::CropOverlay(ViewerWidget *viewerArg)
    : FloatingWidget(viewerArg), m_viewer(viewerArg), m_startPos(QPoint(0, 0)), m_endPos(QPoint(0, 0)),
      m_selectionRect(QRect(0, 0, 0, 0)), m_lockAspectRatio(false), m_cursorAction(NO_DRAG) {
    setMouseTracking(true);
    prepareDrawElements();
    m_brushInactiveTint.setColor(QColor(0, 0, 0, 160));
    m_brushInactiveTint.setStyle(Qt::SolidPattern);
    m_brushDarkGray.setColor(QColor(120, 120, 120, 160));
    m_brushDarkGray.setStyle(Qt::SolidPattern);
    m_brushGray.setColor(QColor(150, 150, 150, 160));
    m_brushGray.setStyle(Qt::SolidPattern);
    m_brushLightGray.setColor(QColor(220, 220, 220, 160));
    m_brushLightGray.setStyle(Qt::SolidPattern);
    m_selectionOutlinePen.setColor(Qt::white);
    m_selectionOutlinePen.setStyle(Qt::SolidLine);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);

    //tmp for testing
    m_ar.setX(16);
    m_ar.setY(9);

    if(m_viewer)
        setContainerSize(m_viewer->size());
    // zoom while cropping re-derives the mapping instead of staling it —
    // the selection is image-anchored, so it rides the zoom unchanged
    connect(m_viewer, &ViewerWidget::canvasScaleChanged, this, &CropOverlay::refreshMapping);

    hide();
}

//------------------------------------------------------------------------------
void CropOverlay::prepareDrawElements() {
    for(int i = 0; i < 8; i++) {
        m_handles[i] = QRect(0, 0, handleSize() * 2, handleSize() * 2);
    }
}

//------------------------------------------------------------------------------
// live pulls — the geometry these return is the viewer's own, queried at
// use time; the expressions are identical to the old push-and-store math,
// only the data source moved
QRect CropOverlay::imageRectLive() const {
    return QRect(QPoint(0, 0), m_viewer->sourceSize());
}

QRect CropOverlay::imageDrawRectDevice() const {
    QRect r = m_viewer->imageRect();
    return QRect(r.topLeft() * dpr(), r.bottomRight() * dpr());
}

QRect CropOverlay::imageDrawRectLogical() const {
    return m_viewer->imageRect();
}

float CropOverlay::imageScale() const {
    return m_viewer->currentScale();
}

// a new image invalidates the selection in image coordinates
void CropOverlay::onImageChanged() {
    clearSelection();
    update();
}

//------------------------------------------------------------------------------
void CropOverlay::clearSelection() {
    if(hasSelection()) {
        m_startPos = QPoint(0, 0);
        m_endPos = QPoint(0, 0);
        m_selectionRect = QRect(0, 0, 0, 0);
        m_selectionDrawRect = QRect(0, 0, 0, 0);
        update();
        emit selectionChanged(m_selectionRect);
    }
}

//------------------------------------------------------------------------------
bool CropOverlay::hasSelection() {
    if(m_selectionRect.width() && m_selectionRect.height())
        return true;
    return false;
}

//------------------------------------------------------------------------------
void CropOverlay::selectAll() {
    m_selectionRect = imageRectLive();
    updateSelectionDrawRect();
    updateHandlePositions();
    update();
    emit selectionChanged(m_selectionRect);
}

//------------------------------------------------------------------------------
void CropOverlay::setLockAspectRatio(bool mode) {
    m_lockAspectRatio = mode;
}

//------------------------------------------------------------------------------
void CropOverlay::setAspectRatio(QPointF ratio) {
    if(ratio.x() == 0.0 || ratio.y() == 0.0)
        return;
    m_ar = ratio;
    setLockAspectRatio(true);
    // force resize selection area
    if(hasSelection()) {
        resizeSelection(QPoint(0, 0));
        update();
        emit selectionChanged(m_selectionRect);
    }
}

//------------------------------------------------------------------------------
void CropOverlay::hide() {
    m_startPos = QPoint(0, 0);
    m_endPos = QPoint(0, 0);
    clearSelection();
    FloatingWidget::hide();
}

//------------------------------------------------------------------------------
void CropOverlay::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QPainter p(this);
    if(!hasSelection()) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_brushInactiveTint);
        p.drawRect(imageDrawRectLogical());
    } else {
        // draw tint over non-selected area of the image
        QRegion tintRegion(imageDrawRectLogical());
        p.setClipRegion(tintRegion.subtracted(m_selectionDrawRectDpi));
        p.fillRect(imageDrawRectLogical(), m_brushInactiveTint);
        p.setClipRegion(rect());
        // selection outline & handles
        if(m_selectionDrawRect.width() > 0 && m_selectionDrawRect.height() > 0) {
            drawSelection(&p);
            // draw handles if there is no interaction going on
            // and selection is large enough
            if(m_cursorAction == NO_DRAG && m_selectionDrawRect.width() >= 90 && m_selectionDrawRect.height() >= 90)
                drawHandles(m_brushGray, &p);
        }
    }
}

//------------------------------------------------------------------------------
void CropOverlay::drawSelection(QPainter *painter) {
    painter->save();
    painter->setPen(m_selectionOutlinePen);
    painter->setBrush(QBrush(QColor(0, 0, 0, 0)));
    painter->drawRect(m_selectionDrawRectDpi);
    painter->restore();
}

//------------------------------------------------------------------------------
void CropOverlay::drawHandles(QBrush &brush, QPainter *painter) {
    painter->save();
    painter->setBrush(brush);
    painter->setPen(m_selectionOutlinePen);
    for(int i = 0; i < 8; i++)
        painter->drawRect(m_handlesDpi[i]);
    painter->restore();
}

//------------------------------------------------------------------------------
// moves first QRect inside second
// resizes inner rect to outer size if needed
QRect CropOverlay::placeInside(QRect inner, QRect outer) {
    if(inner.width() > outer.width()) {
        inner.setLeft(outer.left());
        inner.setRight(outer.right());
    } else {
        if(inner.left() < outer.left())
            inner.moveLeft(outer.left());
        if(inner.right() > outer.right())
            inner.moveRight(outer.right());
    }
    if(inner.height() > outer.height()) {
        inner.setTop(outer.top());
        inner.setBottom(outer.bottom());
    } else {
        if(inner.top() < outer.top())
            inner.moveTop(outer.top());
        if(inner.bottom() > outer.bottom())
            inner.moveBottom(outer.bottom());
    }
    return inner;
}

//------------------------------------------------------------------------------
CursorAction CropOverlay::hoverTarget(QPoint pos) {
    CursorAction target;
    if(m_handles[0].contains(pos)) {
        target = DRAG_TOPLEFT;
    } else if(m_handles[1].contains(pos)) {
        target = DRAG_TOPRIGHT;
    } else if(m_handles[2].contains(pos)) {
        target = DRAG_BOTTOMLEFT;
    } else if(m_handles[3].contains(pos)) {
        target = DRAG_BOTTOMRIGHT;
    } else if(m_handles[4].contains(pos)) {
        target = DRAG_LEFT;
    } else if(m_handles[5].contains(pos)) {
        target = DRAG_RIGHT;
    } else if(m_handles[6].contains(pos)) {
        target = DRAG_TOP;
    } else if(m_handles[7].contains(pos)) {
        target = DRAG_BOTTOM;
    } else if(m_selectionDrawRect.contains(pos)) {
        target = DRAG_MOVE;
    } else {
        target = NO_DRAG;
    }
    return target;
}

//------------------------------------------------------------------------------
void CropOverlay::setCursorAction(CursorAction action) {
    switch(action) {
    case DRAG_TOPLEFT:
    case DRAG_BOTTOMRIGHT: setCursor(QCursor(Qt::SizeFDiagCursor)); break;
    case DRAG_TOPRIGHT:
    case DRAG_BOTTOMLEFT: setCursor(QCursor(Qt::SizeBDiagCursor)); break;
    case DRAG_LEFT:
    case DRAG_RIGHT: setCursor(QCursor(Qt::SizeHorCursor)); break;
    case DRAG_TOP:
    case DRAG_BOTTOM: setCursor(QCursor(Qt::SizeVerCursor)); break;
    case DRAG_MOVE: setCursor(QCursor(Qt::OpenHandCursor)); break;
    default: setCursor(QCursor(Qt::ArrowCursor)); break;
    }
}

//------------------------------------------------------------------------------
void CropOverlay::setResizeAnchor(CursorAction action) {
    switch(action) {
    case DRAG_TOPLEFT: m_resizeAnchor = m_selectionRect.bottomRight(); break;
    case DRAG_TOPRIGHT: m_resizeAnchor = m_selectionRect.bottomLeft(); break;
    case DRAG_BOTTOMLEFT: m_resizeAnchor = m_selectionRect.topRight(); break;
    case DRAG_BOTTOMRIGHT: m_resizeAnchor = m_selectionRect.topLeft(); break;
    case DRAG_LEFT: m_resizeAnchor = m_selectionRect.topRight(); break;
    case DRAG_RIGHT:
    case DRAG_TOP: m_resizeAnchor = m_selectionRect.bottomLeft(); break;
    case DRAG_BOTTOM: m_resizeAnchor = m_selectionRect.topLeft(); break;
    default: break;
    }
}

//------------------------------------------------------------------------------
// TODO: flip selection rectangle
void CropOverlay::resizeSelection(QPoint delta) {
    if(m_lockAspectRatio)
        resizeSelectionAR(delta);
    else
        resizeSelectionFree(delta);
    updateSelectionDrawRect();
    updateHandlePositions();
}

//------------------------------------------------------------------------------
// TODO: split this up
void CropOverlay::resizeSelectionAR(QPoint delta) {
    QSizeF newSz(m_selectionRect.size());
    QSizeF maxSz;
    /* ~JustQtThings~
     * int QRect::bottom() const
     *   Note that for historical reasons this function returns top() + height() - 1;
     *   use y() + height() to retrieve the true y-coordinate.
     * same for QRect::right()
     */
    switch(m_cursorAction) {
    case DRAG_TOPLEFT:
        // get max selection size
        maxSz.setWidth(m_selectionRect.right() + 1);
        maxSz.setHeight(m_selectionRect.bottom() + 1);
        // apply ar to current size
        newSz.setHeight(newSz.width() / m_ar.x() * m_ar.y());
        // scale to fit
        newSz.scale(qMin(newSz.width() - delta.x(), maxSz.width()), maxSz.height(), Qt::KeepAspectRatio);
        // apply
        m_selectionRect.setSize(newSz.toSize());
        // move the corner so it stays in original place
        m_selectionRect.moveBottomRight(m_resizeAnchor);
        break;
    case DRAG_TOPRIGHT:
        maxSz.setWidth(imageRectLive().width() - m_selectionRect.left());
        maxSz.setHeight(m_selectionRect.bottom() + 1);
        newSz.setHeight(newSz.width() / m_ar.x() * m_ar.y());
        newSz.scale(qMin(newSz.width() + delta.x(), maxSz.width()), maxSz.height(), Qt::KeepAspectRatio);
        m_selectionRect.setSize(newSz.toSize());
        m_selectionRect.moveBottomLeft(m_resizeAnchor);
        break;
    case DRAG_LEFT:
    case DRAG_BOTTOMLEFT:
        maxSz.setWidth(m_selectionRect.right() + 1);
        maxSz.setHeight(imageRectLive().height() - m_selectionRect.top());
        newSz.setHeight(newSz.width() / m_ar.x() * m_ar.y());
        newSz.scale(qMin(newSz.width() - delta.x(), maxSz.width()), maxSz.height(), Qt::KeepAspectRatio);
        m_selectionRect.setSize(newSz.toSize());
        m_selectionRect.moveTopRight(m_resizeAnchor);
        break;
    case DRAG_TOP:
        maxSz.setWidth(imageRectLive().width() - m_selectionRect.left());
        maxSz.setHeight(m_selectionRect.bottom() + 1);
        newSz.setHeight(newSz.width() / m_ar.x() * m_ar.y());
        newSz.scale(maxSz.width(), qMin(newSz.height() - delta.y(), maxSz.height()), Qt::KeepAspectRatio);
        m_selectionRect.setSize(newSz.toSize());
        m_selectionRect.moveBottomLeft(m_resizeAnchor);
        break;
    case DRAG_BOTTOM:
        maxSz.setWidth(imageRectLive().width() - m_selectionRect.left());
        maxSz.setHeight(imageRectLive().height() - m_selectionRect.top());
        newSz.setHeight(newSz.width() / m_ar.x() * m_ar.y());
        newSz.scale(maxSz.width(), qMin(newSz.height() + delta.y(), maxSz.height()), Qt::KeepAspectRatio);
        m_selectionRect.setSize(newSz.toSize());
        break;
    case DRAG_RIGHT:
    case DRAG_BOTTOMRIGHT:
    default:
        maxSz.setWidth(imageRectLive().width() - m_selectionRect.left());
        maxSz.setHeight(imageRectLive().height() - m_selectionRect.top());
        newSz.setHeight(newSz.width() / m_ar.x() * m_ar.y());
        newSz.scale(qMin(newSz.width() + delta.x(), maxSz.width()), maxSz.height(), Qt::KeepAspectRatio);
        m_selectionRect.setSize(newSz.toSize());
        break;
    }
}

//------------------------------------------------------------------------------
void CropOverlay::resizeSelectionFree(QPoint delta) {
    // resize
    switch(m_cursorAction) {
    case DRAG_TOPLEFT: m_selectionRect.setTopLeft(m_selectionRect.topLeft() + delta); break;
    case DRAG_TOPRIGHT: m_selectionRect.setTopRight(m_selectionRect.topRight() + delta); break;
    case DRAG_BOTTOMLEFT: m_selectionRect.setBottomLeft(m_selectionRect.bottomLeft() + delta); break;
    case DRAG_BOTTOMRIGHT: m_selectionRect.setBottomRight(m_selectionRect.bottomRight() + delta); break;
    case DRAG_LEFT: m_selectionRect.setLeft(m_selectionRect.left() + delta.x()); break;
    case DRAG_RIGHT: m_selectionRect.setRight(m_selectionRect.right() + delta.x()); break;
    case DRAG_TOP: m_selectionRect.setTop(m_selectionRect.top() + delta.y()); break;
    case DRAG_BOTTOM: m_selectionRect.setBottom(m_selectionRect.bottom() + delta.y()); break;
    default: break;
    }
    // flip if needed
    if(m_selectionRect.width() < 0) {
        int left = m_selectionRect.left();
        m_selectionRect.setLeft(m_selectionRect.right());
        m_selectionRect.setRight(left);
        switch(m_cursorAction) {
        case DRAG_TOPLEFT: m_cursorAction = DRAG_TOPRIGHT; break;
        case DRAG_TOPRIGHT: m_cursorAction = DRAG_TOPLEFT; break;
        case DRAG_BOTTOMLEFT: m_cursorAction = DRAG_BOTTOMRIGHT; break;
        case DRAG_BOTTOMRIGHT: m_cursorAction = DRAG_BOTTOMLEFT; break;
        case DRAG_LEFT: m_cursorAction = DRAG_RIGHT; break;
        case DRAG_RIGHT: m_cursorAction = DRAG_LEFT; break;
        default: break;
        }
        setCursorAction(m_cursorAction);
    }
    if(m_selectionRect.height() < 0) {
        int top = m_selectionRect.top();
        m_selectionRect.setTop(m_selectionRect.bottom());
        m_selectionRect.setBottom(top);
        switch(m_cursorAction) {
        case DRAG_TOPLEFT: m_cursorAction = DRAG_BOTTOMLEFT; break;
        case DRAG_TOPRIGHT: m_cursorAction = DRAG_BOTTOMRIGHT; break;
        case DRAG_BOTTOMLEFT: m_cursorAction = DRAG_TOPLEFT; break;
        case DRAG_BOTTOMRIGHT: m_cursorAction = DRAG_TOPRIGHT; break;
        case DRAG_TOP: m_cursorAction = DRAG_BOTTOM; break;
        case DRAG_BOTTOM: m_cursorAction = DRAG_TOP; break;
        default: break;
        }
        setCursorAction(m_cursorAction);
    }
    // crop selection so it's bounded by imageRect
    m_selectionRect = m_selectionRect.intersected(imageRectLive());
}

//------------------------------------------------------------------------------
// generate a draw rectangle from selectionRect
void CropOverlay::updateSelectionDrawRect() {
    m_selectionDrawRect = m_selectionRect;
    m_selectionDrawRect.moveTopLeft(m_selectionRect.topLeft() * imageScale() + imageDrawRectDevice().topLeft());
    m_selectionDrawRect.setSize((m_selectionRect.size() - QSize(1, 1)) * imageScale());
    m_selectionDrawRectDpi = QRect(QPoint(m_selectionDrawRect.topLeft() / dpr()), m_selectionDrawRect.size() / dpr());
}

//------------------------------------------------------------------------------
// map a point to image coordinate
QPoint CropOverlay::mapPointToImage(QPoint p) {
    // shift relative to (0,0)
    QRect draw = imageDrawRectDevice();
    if(p.x() < draw.x())
        p.setX(draw.x());
    if(p.y() < draw.y())
        p.setY(draw.y());
    p.setX(p.x() - draw.x());
    p.setY(p.y() - draw.y());
    p = p / imageScale();
    // bound to image size
    if(p.x() > imageRectLive().width())
        p.setX(imageRectLive().width() - 1);
    if(p.y() > imageRectLive().height())
        p.setY(imageRectLive().height() - 1);
    return p;
}

//------------------------------------------------------------------------------
void CropOverlay::mousePressEvent(QMouseEvent *event) {
    if(event->buttons() & Qt::LeftButton) {
        m_cursorAction = hoverTarget(event->pos() * dpr());
        setCursorAction(m_cursorAction);
        setResizeAnchor(m_cursorAction);
        m_moveStartPos = event->pos();
        // start selection!
        if(!hasSelection()) {
            m_cursorAction = SELECTION_START; // move to hovertarget??
            QPoint point = mapPointToImage(event->pos() * dpr());
            m_selectionRect.setTopLeft(point);
            m_selectionRect.setBottomRight(point);
            m_resizeAnchor = point;
        }
    } else if(event->buttons() & Qt::RightButton) {
        clearSelection();
        m_cursorAction = NO_DRAG;
    }
}

//------------------------------------------------------------------------------
// TODO: uneven movement with fractional scaling
void CropOverlay::mouseMoveEvent(QMouseEvent *event) {
    if(event->buttons() & Qt::LeftButton /*&& hasSelection()*/) {
        if(m_cursorAction == SELECTION_START) {
            // skip if cursor hasn't been moved in some direction
            if(event->pos().x() == m_moveStartPos.x() || event->pos().y() == m_moveStartPos.y())
                return;
            // determine direction
            if(event->pos().x() > m_moveStartPos.x())
                if(event->pos().y() > m_moveStartPos.y())
                    m_cursorAction = DRAG_BOTTOMRIGHT;
                else
                    m_cursorAction = DRAG_TOPRIGHT;
            else if(event->pos().y() > m_moveStartPos.y())
                m_cursorAction = DRAG_BOTTOMLEFT;
            else
                m_cursorAction = DRAG_TOPLEFT;
        }
        if(m_cursorAction == NO_DRAG || m_cursorAction == SELECTION_START)
            return;
        // continue with resizing
        QPointF delta = QPointF(event->pos() - m_moveStartPos) * dpr();
        if(m_cursorAction == DRAG_MOVE) { // moving selection
            setCursor(Qt::ClosedHandCursor);
            m_moveStartPos = event->pos();
            m_selectionRect.translate(delta.toPoint() / imageScale());
            if(!imageRectLive().contains(m_selectionRect)) {
                m_selectionRect = placeInside(m_selectionRect, imageRectLive());
            }
            updateSelectionDrawRect();
            updateHandlePositions();
            update();
            /*} else if(cursorAction == NO_DRAG || cursorAction == SELECTION_START) {
            // unneeded?
            endPos = event->pos();
            //build selection rectangle
            QPoint tl, br;
            startPos.x() >= endPos.x() ? tl.setX(endPos.x()) : tl.setX(startPos.x());
            startPos.y() >= endPos.y() ? tl.setY(endPos.y()) : tl.setY(startPos.y());

            startPos.x() <= endPos.x() ? br.setX(endPos.x()) : br.setX(startPos.x());
            startPos.y() <= endPos.y() ? br.setY(endPos.y()) : br.setY(startPos.y());

            selectionRect.setTopLeft(mapPointToImage(tl * dpr()));
            selectionRect.setBottomRight(mapPointToImage(br * dpr()));

            updateSelectionDrawRect();
            updateHandlePositions();
            update();
        */
        } else { // resizing selection
            resizeSelection(delta.toPoint() / imageScale());
            m_moveStartPos = event->pos();
            update();
        }
        emit selectionChanged(m_selectionRect);
    } else {
        if(!hasSelection())
            setCursor(QCursor(Qt::ArrowCursor));
        else
            setCursorAction(hoverTarget(event->pos() * dpr()));
    }
}

//------------------------------------------------------------------------------
void CropOverlay::mouseReleaseEvent(QMouseEvent *event) {
    // user just clicked without moving the mouse, clear
    if(m_cursorAction == SELECTION_START)
        clearSelection();
    m_cursorAction = NO_DRAG;
    setCursorAction(hoverTarget(event->pos() * dpr()));
    update();
}

//------------------------------------------------------------------------------
void CropOverlay::updateHandlePositions() {
    // top left
    m_handles[0].moveTopLeft(m_selectionDrawRect.topLeft());
    // top right
    m_handles[1].moveTopRight(m_selectionDrawRect.topRight() + QPoint(1, 0));
    // bottom left
    m_handles[2].moveBottomLeft(m_selectionDrawRect.bottomLeft() + QPoint(0, 1));
    // bottom right
    m_handles[3].moveBottomRight(m_selectionDrawRect.bottomRight() + QPoint(1, 1));
    // left
    m_handles[4].moveTopLeft(QPoint(m_selectionDrawRect.left(), m_selectionDrawRect.center().y() - handleSize()));
    // right
    m_handles[5].moveTopRight(QPoint(m_selectionDrawRect.right() + 1, m_selectionDrawRect.center().y() - handleSize()));
    // top
    m_handles[6].moveTopLeft(QPoint(m_selectionDrawRect.center().x() - handleSize(), m_selectionDrawRect.top()));
    // bottom
    m_handles[7].moveBottomLeft(
        QPoint(m_selectionDrawRect.center().x() - handleSize(), m_selectionDrawRect.bottom() + 1));
    for(int i = 0; i < 8; i++) {
        m_handlesDpi[i] = QRectF(m_handles[i].topLeft() / dpr(), m_handles[i].size() / dpr());
    }
    // top left
    m_handlesDpi[0].moveTopLeft(m_selectionDrawRectDpi.topLeft());
    // top right
    m_handlesDpi[1].moveTopRight(m_selectionDrawRectDpi.topRight() + QPoint(1, 0));
    // bottom left
    m_handlesDpi[2].moveBottomLeft(m_selectionDrawRectDpi.bottomLeft() + QPoint(0, 1));
    // bottom right
    m_handlesDpi[3].moveBottomRight(m_selectionDrawRectDpi.bottomRight() + QPoint(1, 1));
    // left
    m_handlesDpi[4].moveLeft(m_selectionDrawRectDpi.left());
    // right
    m_handlesDpi[5].moveRight(m_selectionDrawRectDpi.right() + 1);
    // top
    m_handlesDpi[6].moveTop(m_selectionDrawRectDpi.top());
    // bottom
    m_handlesDpi[7].moveBottom(m_selectionDrawRectDpi.bottom() + 1);
}

//------------------------------------------------------------------------------
// TODO: use aspect ratio
void CropOverlay::onSelectionOutsideChange(QRect selection) {
    if(selection.width() > 0 && selection.height() > 0) {
        QRect boundedSelection = placeInside(selection, imageRectLive());
        m_selectionRect = boundedSelection;
        updateSelectionDrawRect();
        updateHandlePositions();
        update();
        if(m_selectionRect != selection)
            emit selectionChanged(m_selectionRect);
    } else {
        //selectAll();
    }
}

//------------------------------------------------------------------------------
// The save chord is dual-purpose here: with a pending selection it commits
// the crop and then saves, in that order — an order only this surface can
// guarantee, so with a selection the chord is vetoed out of QShortcutMap and
// sequenced by keyPressEvent below. Without a selection the chord rides the
// map like everywhere else. The walk keys stay vetoed for the same reason
// they are vetoed at every floating surface.
bool CropOverlay::event(QEvent *event) {
    if(event->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(event);
        const QKeySequence pressed(ke->key() | ke->modifiers());
        bool saveChord = false;
        for(const QString &token : actionManager->shortcutsForAction(QStringLiteral("save"))) {
            // the projection's own token->sequence expansion: "Enter" claims
            // both Returns. (const copy, so clazy's range-loop-detach stays
            // quiet on the actionManager call's return value.)
            const QList<QKeySequence> seqs = ActionManager::sequencesForToken(token);
            for(const QKeySequence &seq : seqs)
                saveChord |= pressed == seq;
        }
        if(actionManager->keyNamesSubjectChangingAction(ke) || (saveChord && hasSelection()))
            event->accept();
    }
    return FloatingWidget::event(event);
}

void CropOverlay::keyPressEvent(QKeyEvent *event) {
    event->accept();
    // the save chords imply committing the pending crop: "save" while a
    // selection stands means save it WITH the crop. The chord first applies
    // the in-memory crop and then falls through below, so the global table
    // still does the writing — the disk belongs to Ctrl+S / Ctrl+Shift+S,
    // the tool just answers them.
    if(hasSelection() && event->key() == Qt::Key_S && (event->modifiers() & Qt::ControlModifier)) {
        emit cropApply();
        // the chord's second half is deliberate sequencing, not a fall-through:
        // the veto kept it out of QShortcutMap, so the writing happens here —
        // processEvent resolves save or saveAs, whichever the user bound
        actionManager->processEvent(event);
        return;
    }
    if(const std::optional<CropAnswer> answer = cropAnswerForKey(event)) {
        // Enter is the crop's own key, with or without a selection: with
        // nothing selected there is nothing to apply, and the key is consumed
        // — it must not resolve as folder view (its global binding) or trip
        // the pending-work lock. Owned keys do not pass through.
        if(*answer == CropAnswer::Apply && !hasSelection())
            return;
        if(*answer == CropAnswer::Apply) {
            emit cropApply(); // the in-memory crop only; the save is Ctrl+S's job
            return;
        }
        clearSelection();
        emit escPressed();
        return;
    }
    if(event->matches(QKeySequence::SelectAll)) {
        selectAll();
    } else if(actionManager->keyNamesSubjectChangingAction(event)) {
        // a different image clears the selection (onImageChanged), so yielding
        // the walk here would discard the crop the user is framing. Consumed.
    }
    // everything else was either fired by QShortcutMap before delivery or is
    // consumed above — the manual funnel has no keyboard jurisdiction here
}

//------------------------------------------------------------------------------
void CropOverlay::refreshMapping() {
    updateSelectionDrawRect();
    updateHandlePositions();
    update();
}

//------------------------------------------------------------------------------
void CropOverlay::resizeEvent(QResizeEvent *event) {
    refreshMapping();
    QWidget::resizeEvent(event);
}

//------------------------------------------------------------------------------
void CropOverlay::recalculateGeometry() {
    setGeometry(0, 0, containerSize().width(), containerSize().height());
}
