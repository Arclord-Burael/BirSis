#pragma once

#include <vector>
#include <optional>
#include <wx/wx.h>
#include <wx/timer.h>
#include <wx/glcanvas.h>
#include <wx/geometry.h>
#include "ToolType.h"
#include "dialog/StrokeDialog.h"
#include "Document.h"

class Document;
struct DocumentLayer;

class DocumentCanvas : public wxGLCanvas
{
public:
    explicit DocumentCanvas(Document* owner, wxWindow* parent);
    ~DocumentCanvas() override;

    void MarkAllGLLayersDirty();
    void MarkGLLayerDirty(int layerIndex);
    void MarkGLLayerDirtyRect(int layerIndex, const wxRect& localRect);

    void SelectAllMarquee();
    void ClearMarqueeSelection();

    bool HasMarqueeSelection() const;
    wxRect GetMarqueeRect() const;
    bool GetSelectionMask(wxRect& outDocRect, std::vector<unsigned char>& outMask) const;

    void ClearCropSelection();
    bool HasCropSelection() const;
    wxRect GetCropRect() const;

    void ShowFreeTransformBox();
    void HideFreeTransformBox();
    void CancelFreeTransformBox();
    bool IsFreeTransformBoxVisible() const { return m_freeTransformVisible; }

    void SetBrushCursorSize(int size);
    int GetBrushCursorSize() const { return m_brushCursorSize; }

    void FinishPenPath();
    void CancelPenPath();
    void MakePenPathSelection();
    bool HasPenPath() const;
    bool StrokePenPath(int width, const wxColour& color, int opacity);
    bool StrokeSelection(int width, const wxColour& color, int opacity, StrokeDialog::StrokeLocation location);

private:
    enum class CropHandle { None, Inside, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };
    enum class TransformHandle { None, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };
    enum class PenHitType { None, Anchor, InHandle, OutHandle };

    struct PenHit
    {
        PenHitType type = PenHitType::None;
        int index = -1;
    };

    struct PenPoint
    {
        wxPoint anchor = wxPoint(0, 0);
        wxPoint inHandle = wxPoint(0, 0);
        wxPoint outHandle = wxPoint(0, 0);
        bool hasInHandle = false;
        bool hasOutHandle = false;
    };

    struct GLLayerCache
    {
        unsigned int textureId = 0;
        int width = 0;
        int height = 0;
        bool dirty = true;

        // Do not union all brush dabs into one huge rect.
        // OpenGL upload becomes very slow if a long stroke creates one giant dirty rectangle.
        std::vector<wxRect> dirtyRects;
    };

private:
    void OnPaint(wxPaintEvent& event);
    void DrawCheckerboard(wxDC& dc, const wxRect& visibleRect, const wxPoint& pageOrigin);

    void RenderGL(const wxRect* repaintRect = nullptr);
    void InitializeGLIfNeeded();
    void EnsureGLLayerCacheSize();
    void DestroyGLTextures();
    void UploadLayerTexture(int layerIndex, const DocumentLayer& layer);
    void DrawGLQuad(double x, double y, double w, double h);
    void DrawGLRectOutline(double x, double y, double w, double h, double r, double g, double b, double a);
    void DrawGLRoundedRectOutline(double x, double y, double w, double h, double radius, double r, double g, double b, double a, float width = 1.0f);
    void DrawGLLine(double x1, double y1, double x2, double y2, double r, double g, double b, double a, float width = 1.0f);
    void DrawGLCheckerboard(const wxRect& visibleRect, const wxPoint& pageOrigin);
    void DrawGLOverlays();
    void DrawGLBrushCursor();
    void DrawGLCropOverlay();

    void DrawGLMarchingAnts(int x, int y, int w, int h);
    void DrawGLMarchingAntsLine(double x1, double y1, double x2, double y2);
    void DrawGLEllipseMarchingAnts(int x, int y, int w, int h);
    void DrawGLLassoMarchingAnts();

    void DrawGLPenOverlay();
    void DrawGLBezier(const wxPoint& p0, const wxPoint& c1, const wxPoint& c2, const wxPoint& p1, double r, double g, double b, double a, float width = 1.0f);
    void DrawGLFilledRect(double x, double y, double w, double h, double r, double g, double b, double a);
    void DrawGLFreeTransformOverlay();
    void DrawGLShapeOverlay();
    void DrawGLEllipseOutline(double x, double y, double w, double h, double r, double g, double b, double a);

    std::vector<unsigned char> BuildRGBAFromImageRect(const wxImage& image, const wxRect& rect);

    void DrawMarqueeOverlay(wxDC& dc);
    void DrawLassoOverlay(wxDC& dc);
    void DrawMarchingAnts(wxDC& dc, int x, int y, int w, int h);
    void DrawMarchingAntsLine(wxDC& dc, const wxPoint& a, const wxPoint& b);
    void DrawBrushCursor(wxDC& dc);
    void DrawGradientOverlay(wxDC& dc);
    void StopGradientDrag(bool apply);
    void DrawGradientDragLine(wxDC& dc);
    void DrawFreeTransformOverlay(wxDC& dc);

    void DrawShapeOverlay(wxDC& dc);
    void BeginShapeDrag(const wxPoint& mousePos);
    void UpdateShapeDrag(const wxPoint& mousePos);
    void StopShapeDrag(bool apply);
    wxRect GetShapeRect() const;
    std::vector<unsigned char> BuildShapeMask(const wxRect& rect, bool rounded, int radius) const;
    std::vector<unsigned char> BuildShapeStrokeMask(const wxRect& rect, bool rounded, int radius, int strokeWidth) const;

    void DrawPenOverlay(wxDC& dc);
    void BeginPenPoint(const wxPoint& mousePos);
    void UpdatePenDrag(const wxPoint& mousePos);
    void FinishPenPoint(bool keepPath);
    void ClosePenPath();
    void ClearPenPath();
    bool HitTestFirstPenPoint(const wxPoint& mousePos) const;
    wxPoint PenDocToScreen(const wxPoint& docPoint) const;
    void DrawBezier(wxDC& dc, const wxPoint& p0, const wxPoint& c1, const wxPoint& c2, const wxPoint& p1);
    std::vector<wxPoint> FlattenPenPathToPolygon() const;

    PenHit HitTestPenObject(const wxPoint& mousePos) const;
    bool HitTestPenPath(const wxPoint& mousePos) const;

    bool BeginPenEdit(const wxPoint& mousePos);
    void UpdatePenEdit(const wxPoint& mousePos);
    void StopPenEdit();

    bool BeginPathSelectionDrag(const wxPoint& mousePos);
    void UpdatePathSelectionDrag(const wxPoint& mousePos);
    void StopPathSelectionDrag();

    void ShowPenContextMenu(const wxPoint& mousePos);

    void OnMouseWheel(wxMouseEvent& event);

    void OnLeftDown(wxMouseEvent& event);
    void OnLeftUp(wxMouseEvent& event);
    void OnMouseMove(wxMouseEvent& event);
    void OnRightDown(wxMouseEvent& event);
    void OnMouseLeave(wxMouseEvent& event);
    void OnMouseCaptureLost(wxMouseCaptureLostEvent& event);

    void OnSelectionAnimTimer(wxTimerEvent& event);

    void StopPanning();
    void StopMovingLayer();
    void StopMarqueeDrag(bool keepSelection);
    void StopLassoDrag(bool keepSelection);
    wxRect GetLassoBoundingRect() const;

    bool ShouldDrawBrushCursor() const;
    wxPoint GetToolAnchorDocPoint(const wxPoint& mousePos) const;
    bool IsPixelInToolShape(int bx, int by, int brushSize) const;
    wxRect GetBrushCursorBounds(const wxPoint& mousePos) const;
    wxRect GetToolStrokeDocBounds(const wxPoint& docPoint) const;
    void RefreshBrushCursorAt(const wxPoint& mousePos);
    void RefreshOldAndNewBrushCursor(const wxPoint& oldPos, const wxPoint& newPos);

    bool PickDropperColorAt(const wxPoint& mousePos);
    void SetStampSourceAt(const wxPoint& mousePos);
    bool HasStampSource() const { return m_stampHasSource; }

    bool BeginToolStroke(const wxPoint& mousePos);
    void ContinueToolStroke(const wxPoint& mousePos);
    void StopDrawingStroke();
    void FinishPendingStrokeHistory();
    void BuildCachedBrushDab(int size, int hardness);
    void ClearCachedBrushDab();

    wxColour GetActiveStrokeColor() const;
    bool ActiveToolUsesSquareBrush() const;
    bool ActiveToolIsErase() const;

    void StartInlineTextEdit(const wxPoint& canvasPoint);
    void CommitInlineTextEdit();
    void CancelInlineTextEdit();

    TransformHandle HitTestFreeTransformHandle(const wxPoint& mousePos) const;
    bool HitTestFreeTransformPivot(const wxPoint& mousePos) const;
    wxCursor GetFreeTransformHandleCursor(TransformHandle handle) const;
    wxRect GetSelectedLayerDocRect() const;
    wxPoint GetFreeTransformPivotDocPoint() const;
    void SetFreeTransformPivotDocPoint(const wxPoint& docPoint);
    void ApplyFreeTransformScale(const wxPoint& mousePos);
    void StopFreeTransformScale();
    void StopFreeTransformPivotDrag();

    void DrawCropOverlay(wxDC& dc);
    wxRect GetCropScreenRect() const;
    wxPoint ClampDocPointToPage(const wxPoint& p) const;
    wxRect NormalizedRectFromPoints(const wxPoint& a, const wxPoint& b) const;
    CropHandle HitTestCropHandle(const wxPoint& mousePos) const;
    wxCursor GetCropHandleCursor(CropHandle handle) const;
    void SetCropRectFromEdges(int left, int top, int right, int bottom);
    void StopCropDrag(bool keepSelection);

private:
    Document* m_owner = nullptr;
    wxGLContext* m_glContext = nullptr;
    bool m_glInitialized = false;
    std::vector<GLLayerCache> m_glLayerCaches;

    bool m_freeTransformVisible = false;

    bool m_transformScaling = false;
    TransformHandle m_activeTransformHandle = TransformHandle::None;
    wxRect m_transformStartRect = wxRect();
    wxImage m_transformStartImage;

    wxRect m_transformSessionStartRect = wxRect();
    wxImage m_transformSessionStartImage;

    bool m_transformPivotDragging = false;
    bool m_transformPivotCustom = false;
    wxPoint m_transformPivotDoc = wxPoint(0, 0);

    bool m_panning = false;
    wxPoint m_panStartScreen = wxPoint(0, 0);
    wxPoint m_panStartViewOffset = wxPoint(0, 0);

    bool m_movingLayer = false;
    wxPoint m_moveStartScreen = wxPoint(0, 0);
    wxPoint m_moveStartLayerOffset = wxPoint(0, 0);

    bool m_marqueeDragging = false;
    bool m_marqueeHasSelection = false;
    wxPoint m_marqueeStartDoc = wxPoint(0, 0);
    wxPoint m_marqueeCurrentDoc = wxPoint(0, 0);
    bool m_marqueeConstrainSquare = false;
    bool m_marqueeElliptical = false;

    bool m_lassoDragging = false;
    bool m_lassoHasSelection = false;
    std::vector<wxPoint> m_lassoPoints;

    bool m_shapeDragging = false;
    wxPoint m_shapeStartDoc = wxPoint(0, 0);
    wxPoint m_shapeCurrentDoc = wxPoint(0, 0);
    ToolType m_shapeDragTool = ToolType::Rectangle;
    bool m_shapeConstrainSquare = false;

    std::vector<PenPoint> m_penPoints;
    bool m_penPathActive = false;
    bool m_penDraggingPoint = false;
    bool m_penClosed = false;
    int m_penDragIndex = -1;
    wxPoint m_penPreviewDoc = wxPoint(0, 0);

    bool m_penEditing = false;
    PenHitType m_penEditType = PenHitType::None;
    int m_penEditIndex = -1;
    int m_penSelectedIndex = -1;
    PenHitType m_penSelectedType = PenHitType::None;
    wxPoint m_penEditStartDoc = wxPoint(0, 0);
    wxPoint m_penEditAnchorStart = wxPoint(0, 0);
    wxPoint m_penEditInStart = wxPoint(0, 0);
    wxPoint m_penEditOutStart = wxPoint(0, 0);
    bool m_penEditHadInHandle = false;
    bool m_penEditHadOutHandle = false;

    bool m_pathSelected = false;
    bool m_pathDragging = false;
    wxPoint m_pathDragStartDoc = wxPoint(0, 0);
    std::vector<PenPoint> m_pathDragStartPoints;

    bool m_cropDragging = false;
    bool m_cropHasSelection = false;
    wxPoint m_cropStartDoc = wxPoint(0, 0);
    wxPoint m_cropCurrentDoc = wxPoint(0, 0);
    wxPoint m_cropDragStartDoc = wxPoint(0, 0);
    wxRect m_cropDragStartRect = wxRect();
    CropHandle m_activeCropHandle = CropHandle::None;

    wxTimer m_selectionAnimTimer;
    int m_selectionAnimOffset = 0;

    bool m_mouseInside = false;
    wxPoint m_lastMousePos = wxPoint(0, 0);
    int m_brushCursorSize = 20;

    bool m_drawingStroke = false;
    wxPoint m_lastDrawDocPoint = wxPoint(0, 0);
    wxImage m_strokeBaseImage;
    std::vector<double> m_brushStrokeMask;
    int m_brushStrokeMaskW = 0;
    int m_brushStrokeMaskH = 0;

    bool m_pendingStrokeHistory = false;
    bool m_pendingStrokeHistoryUI = false;
    wxImage m_pendingStrokeBeforeImage;

    std::vector<BrushDabPixel> m_cachedBrushDab;
    int m_cachedBrushDabSize = 0;
    int m_cachedBrushDabHardness = -1;

    double m_brushSpacingAccumulator = 0.0;
    int m_brushSpacingPercent = 12;
    wxPoint2DDouble m_lastDrawExactDocPoint = wxPoint2DDouble(0.0, 0.0);

    bool m_stampHasSource = false;
    wxPoint m_stampSourceDocPoint = wxPoint(0, 0);
    wxPoint m_stampStrokeStartDocPoint = wxPoint(0, 0);

    bool m_gradientDragging = false;
    wxPoint m_gradientStartDoc = wxPoint(0, 0);
    wxPoint m_gradientCurrentDoc = wxPoint(0, 0);

    wxTextCtrl* m_inlineTextCtrl = nullptr;
    wxPoint m_inlineTextDocPoint = wxPoint(0, 0);
    wxFont m_inlineTextFont;

private:
    wxDECLARE_EVENT_TABLE();
};
