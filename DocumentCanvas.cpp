#include "DocumentCanvas.h"
#include "Document.h"
#include "MainFrame.h"
#include "dialog/StrokeDialog.h"

#include <wx/dcbuffer.h>
#include <wx/textctrl.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <GL/gl.h>

namespace
{
    bool IsCloneSourceModifierDown(const wxMouseEvent& event)
    {
        const int modifiers = event.GetModifiers();

        return event.ControlDown() || event.CmdDown() || (modifiers & wxMOD_CONTROL) || wxGetKeyState(WXK_CONTROL);
    }

    bool IsBrushLikeTool(ToolType tool)
    {
        return tool == ToolType::Brush || tool == ToolType::Pencil || tool == ToolType::Eraser || tool == ToolType::Stamp;
    }

    void RestoreDocumentToolCursor(wxWindow* window)
    {
        MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(window), MainFrame);
        if (mainFrame)
            mainFrame->ApplyCurrentToolCursor(window);
    }

    void RefreshMainFrameDocumentUI(wxWindow* window)
    {
        MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(window), MainFrame);
        if (mainFrame)
            mainFrame->RefreshDocumentDependentUI();
    }

    bool PointInsidePolygon(const std::vector<wxPoint>& polygon, int x, int y)
    {
        if (polygon.size() < 3)
            return false;

        bool inside = false;
        const double px = static_cast<double>(x) + 0.5;
        const double py = static_cast<double>(y) + 0.5;

        size_t j = polygon.size() - 1;

        for (size_t i = 0; i < polygon.size(); ++i)
        {
            const double xi = static_cast<double>(polygon[i].x);
            const double yi = static_cast<double>(polygon[i].y);
            const double xj = static_cast<double>(polygon[j].x);
            const double yj = static_cast<double>(polygon[j].y);

            const bool intersect = ((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / ((yj - yi) == 0.0 ? 1.0 : (yj - yi)) + xi);

            if (intersect)
                inside = !inside;

            j = i;
        }

        return inside;
    }

    bool PointInsideEllipseLocal(int x, int y, int w, int h)
    {
        if (w <= 0 || h <= 0)
            return false;

        const double cx = static_cast<double>(w) * 0.5;
        const double cy = static_cast<double>(h) * 0.5;
        const double rx = static_cast<double>(w) * 0.5;
        const double ry = static_cast<double>(h) * 0.5;

        if (rx <= 0.0 || ry <= 0.0)
            return false;

        const double px = static_cast<double>(x) + 0.5;
        const double py = static_cast<double>(y) + 0.5;
        const double nx = (px - cx) / rx;
        const double ny = (py - cy) / ry;

        return (nx * nx + ny * ny) <= 1.0;
    }

    wxPoint BezierPoint(const wxPoint& p0, const wxPoint& c1, const wxPoint& c2, const wxPoint& p1, double t)
    {
        const double u = 1.0 - t;
        const double tt = t * t;
        const double uu = u * u;
        const double uuu = uu * u;
        const double ttt = tt * t;

        const double x = uuu * p0.x + 3.0 * uu * t * c1.x + 3.0 * u * tt * c2.x + ttt * p1.x;
        const double y = uuu * p0.y + 3.0 * uu * t * c1.y + 3.0 * u * tt * c2.y + ttt * p1.y;

        return wxPoint(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)));
    }

    bool PointInsideRoundedRectLocal(int x, int y, int w, int h, int radius)
    {
        if (w <= 0 || h <= 0)
            return false;

        if (radius <= 0)
            return x >= 0 && y >= 0 && x < w && y < h;

        radius = std::max(0, std::min(radius, std::min(w, h) / 2));

        if (x < 0 || y < 0 || x >= w || y >= h)
            return false;

        const int left = radius;
        const int right = w - radius - 1;
        const int top = radius;
        const int bottom = h - radius - 1;

        if ((x >= left && x <= right) || (y >= top && y <= bottom))
            return true;

        int cx = x < left ? left : right;
        int cy = y < top ? top : bottom;

        const int dx = x - cx;
        const int dy = y - cy;

        return (dx * dx + dy * dy) <= radius * radius;
    }

    std::vector<unsigned char> BuildRGBAFromImage(const wxImage& image, bool showRed, bool showGreen, bool showBlue)
    {
        std::vector<unsigned char> rgba;

        if (!image.IsOk() || !image.GetData())
            return rgba;

        const int w = image.GetWidth();
        const int h = image.GetHeight();

        if (w <= 0 || h <= 0)
            return rgba;

        const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
        rgba.resize(pixelCount * 4u);

        const unsigned char* rgb = image.GetData();
        const unsigned char* alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;

        const bool anyChannelVisible = showRed || showGreen || showBlue;
        const bool singleChannelVisible =
            (showRed && !showGreen && !showBlue) ||
            (!showRed && showGreen && !showBlue) ||
            (!showRed && !showGreen && showBlue);

        for (size_t i = 0; i < pixelCount; ++i)
        {
            const size_t srcRgb = i * 3u;
            const size_t dst = i * 4u;

            if (!anyChannelVisible)
            {
                rgba[dst + 0u] = 0;
                rgba[dst + 1u] = 0;
                rgba[dst + 2u] = 0;
                rgba[dst + 3u] = 0;
                continue;
            }

            if (singleChannelVisible)
            {
                unsigned char value = 0;

                if (showRed)
                    value = rgb[srcRgb + 0u];
                else if (showGreen)
                    value = rgb[srcRgb + 1u];
                else if (showBlue)
                    value = rgb[srcRgb + 2u];

                rgba[dst + 0u] = value;
                rgba[dst + 1u] = value;
                rgba[dst + 2u] = value;
                rgba[dst + 3u] = alpha ? alpha[i] : 255;
                continue;
            }

            rgba[dst + 0u] = showRed ? rgb[srcRgb + 0u] : 0;
            rgba[dst + 1u] = showGreen ? rgb[srcRgb + 1u] : 0;
            rgba[dst + 2u] = showBlue ? rgb[srcRgb + 2u] : 0;
            rgba[dst + 3u] = alpha ? alpha[i] : 255;
        }

        return rgba;
    }

    enum
    {
        ID_PEN_CONTEXT_MAKE_SELECTION = wxID_HIGHEST + 5001,
        ID_PEN_CONTEXT_STROKE_PATH,
        ID_PEN_CONTEXT_DELETE_PATH,
        ID_PEN_CONTEXT_FINISH_PATH
    };
}

wxBEGIN_EVENT_TABLE(DocumentCanvas, wxGLCanvas)
    EVT_PAINT(DocumentCanvas::OnPaint)
    EVT_MOUSEWHEEL(DocumentCanvas::OnMouseWheel)
    EVT_LEFT_DOWN(DocumentCanvas::OnLeftDown)
    EVT_LEFT_UP(DocumentCanvas::OnLeftUp)
    EVT_RIGHT_DOWN(DocumentCanvas::OnRightDown)
    EVT_MOTION(DocumentCanvas::OnMouseMove)
    EVT_LEAVE_WINDOW(DocumentCanvas::OnMouseLeave)
    EVT_MOUSE_CAPTURE_LOST(DocumentCanvas::OnMouseCaptureLost)
    EVT_TIMER(wxID_ANY, DocumentCanvas::OnSelectionAnimTimer)
wxEND_EVENT_TABLE()

DocumentCanvas::DocumentCanvas(Document* owner, wxWindow* parent)
: wxGLCanvas(parent, wxID_ANY, nullptr, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
, m_owner(owner)
{
    m_glContext = new wxGLContext(this);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_selectionAnimTimer.SetOwner(this);
}

DocumentCanvas::~DocumentCanvas()
{
    FinishPendingStrokeHistory();

    DestroyGLTextures();

    if (m_glContext)
    {
        delete m_glContext;
        m_glContext = nullptr;
    }
}

void DocumentCanvas::SetBrushCursorSize(int size)
{
    const int clamped = std::max(1, std::min(256, size));
    if (m_brushCursorSize == clamped)
        return;

    const wxPoint oldPos = m_lastMousePos;
    m_brushCursorSize = clamped;
    RefreshOldAndNewBrushCursor(oldPos, m_lastMousePos);
}

void DocumentCanvas::FinishPenPath()
{
    if (m_penPoints.empty())
        return;

    m_penPathActive = false;
    m_penDraggingPoint = false;
    m_penEditing = false;
    m_penDragIndex = -1;
    m_penEditIndex = -1;
    m_penEditType = PenHitType::None;

    if (HasCapture())
        ReleaseMouse();

    RefreshMainFrameDocumentUI(this);
    Refresh(false);
}

void DocumentCanvas::CancelPenPath()
{
    ClearPenPath();
}

void DocumentCanvas::MakePenPathSelection()
{
    MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);

    if (!m_owner || m_penPoints.size() < 3 || !m_penClosed)
    {
        if (mainFrame && mainFrame->GetStatusBar())
            mainFrame->GetStatusBar()->SetStatusText("Close the Pen path before making a selection", 0);

        return;
    }

    std::vector<wxPoint> polygon = FlattenPenPathToPolygon();

    if (polygon.size() < 3)
        return;

    m_lassoPoints = polygon;
    m_lassoDragging = false;
    m_lassoHasSelection = true;

    m_marqueeDragging = false;
    m_marqueeHasSelection = false;
    m_marqueeStartDoc = wxPoint(0, 0);
    m_marqueeCurrentDoc = wxPoint(0, 0);

    m_penPoints.clear();
    m_penPathActive = false;
    m_penDraggingPoint = false;
    m_penClosed = false;
    m_penEditing = false;
    m_penDragIndex = -1;
    m_penEditIndex = -1;
    m_penEditType = PenHitType::None;
    m_penSelectedIndex = -1;
    m_penSelectedType = PenHitType::None;
    m_penPreviewDoc = wxPoint(0, 0);

    if (HasCapture())
        ReleaseMouse();

    if (!m_selectionAnimTimer.IsRunning())
        m_selectionAnimTimer.Start(120);

    if (mainFrame)
        mainFrame->RefreshDocumentDependentUI();

    RestoreDocumentToolCursor(this);
    Refresh(false);
}

bool DocumentCanvas::HasPenPath() const
{
    return !m_penPoints.empty();
}

bool DocumentCanvas::StrokePenPath(int width, const wxColour& color, int opacity)
{
    if (!m_owner)
        return false;

    if (!m_owner->CanDrawOnSelectedLayer())
        return false;

    if (m_penPoints.size() < 2)
        return false;

    const int strokeWidth = std::max(1, width);
    const int strokeOpacity = std::max(0, std::min(100, opacity));

    std::vector<wxPoint> points = FlattenPenPathToPolygon();

    if (points.size() < 2)
        return false;

    m_owner->BeginHistoryTransaction("Stroke Path");

    wxImage strokeBaseImage = m_owner->CopySelectedLayerImage();
    const int maskW = strokeBaseImage.IsOk() ? strokeBaseImage.GetWidth() : 0;
    const int maskH = strokeBaseImage.IsOk() ? strokeBaseImage.GetHeight() : 0;
    std::vector<double> strokeMask(static_cast<size_t>(maskW) * static_cast<size_t>(maskH), 0.0);

    bool changed = false;

    for (size_t i = 1; i < points.size(); ++i)
    {
        if (m_owner->DrawBrushOnSelectedLayerLine(points[i - 1], points[i], strokeWidth, 100, strokeOpacity, 100, color, &strokeBaseImage, &strokeMask, false))
            changed = true;
    }

    if (m_penClosed && points.size() > 2)
    {
        if (m_owner->DrawBrushOnSelectedLayerLine(points.back(), points.front(), strokeWidth, 100, strokeOpacity, 100, color, &strokeBaseImage, &strokeMask, false))
            changed = true;
    }

    m_owner->EndHistoryTransaction();

    if (!changed)
        return false;

    MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
    if (mainFrame)
        mainFrame->RefreshDocumentDependentUI();

    Refresh(false);
    return true;
}

bool DocumentCanvas::StrokeSelection(int width, const wxColour& color, int opacity, StrokeDialog::StrokeLocation location)
{
    if (!m_owner)
        return false;

    if (!m_owner->CanDrawOnSelectedLayer())
        return false;

    wxRect selectionRect;
    std::vector<unsigned char> selectionMask;

    if (!GetSelectionMask(selectionRect, selectionMask))
        return false;

    if (selectionRect.width <= 0 || selectionRect.height <= 0)
        return false;

    const int strokeWidth = std::max(1, width);

    if (selectionMask.empty())
    {
        selectionMask.assign(
            static_cast<size_t>(selectionRect.width) * static_cast<size_t>(selectionRect.height),
            255
        );
    }

    int grow = 0;

    if (location == StrokeDialog::StrokeLocation::Center)
        grow = (strokeWidth + 1) / 2;
    else if (location == StrokeDialog::StrokeLocation::Outside)
        grow = strokeWidth;

    wxRect workRect(
        selectionRect.x - grow,
        selectionRect.y - grow,
        selectionRect.width + grow * 2,
        selectionRect.height + grow * 2
    );

    const int pageW = m_owner->GetPageWidth();
    const int pageH = m_owner->GetPageHeight();

    if (workRect.x < 0)
    {
        workRect.width += workRect.x;
        workRect.x = 0;
    }

    if (workRect.y < 0)
    {
        workRect.height += workRect.y;
        workRect.y = 0;
    }

    if (workRect.x + workRect.width > pageW)
        workRect.width = pageW - workRect.x;

    if (workRect.y + workRect.height > pageH)
        workRect.height = pageH - workRect.y;

    if (workRect.width <= 0 || workRect.height <= 0)
        return false;

    std::vector<unsigned char> insideMask(
        static_cast<size_t>(workRect.width) * static_cast<size_t>(workRect.height),
        0
    );

    for (int y = 0; y < selectionRect.height; ++y)
    {
        for (int x = 0; x < selectionRect.width; ++x)
        {
            const size_t srcIndex = static_cast<size_t>(y) * static_cast<size_t>(selectionRect.width) + static_cast<size_t>(x);

            if (selectionMask[srcIndex] == 0)
                continue;

            const int dstX = selectionRect.x + x - workRect.x;
            const int dstY = selectionRect.y + y - workRect.y;

            if (dstX < 0 || dstY < 0 || dstX >= workRect.width || dstY >= workRect.height)
                continue;

            insideMask[static_cast<size_t>(dstY) * static_cast<size_t>(workRect.width) + static_cast<size_t>(dstX)] = 255;
        }
    }

    auto isInside = [&](int x, int y) -> bool
    {
        if (x < 0 || y < 0 || x >= workRect.width || y >= workRect.height)
            return false;

        return insideMask[static_cast<size_t>(y) * static_cast<size_t>(workRect.width) + static_cast<size_t>(x)] > 0;
    };

    std::vector<unsigned char> strokeMask(
        static_cast<size_t>(workRect.width) * static_cast<size_t>(workRect.height),
        0
    );

    const int outsideWidth =
        (location == StrokeDialog::StrokeLocation::Inside) ? 0 :
        (location == StrokeDialog::StrokeLocation::Center) ? (strokeWidth + 1) / 2 :
        strokeWidth;

    const int insideWidth =
        (location == StrokeDialog::StrokeLocation::Inside) ? strokeWidth :
        (location == StrokeDialog::StrokeLocation::Center) ? (strokeWidth / 2) :
        0;

    for (int y = 0; y < workRect.height; ++y)
    {
        for (int x = 0; x < workRect.width; ++x)
        {
            const bool currentInside = isInside(x, y);
            bool paint = false;

            if (insideWidth > 0 && currentInside)
            {
                for (int yy = -insideWidth; yy <= insideWidth && !paint; ++yy)
                {
                    for (int xx = -insideWidth; xx <= insideWidth; ++xx)
                    {
                        if (std::abs(xx) > insideWidth || std::abs(yy) > insideWidth)
                            continue;

                        if (!isInside(x + xx, y + yy))
                        {
                            paint = true;
                            break;
                        }
                    }
                }
            }

            if (outsideWidth > 0 && !currentInside)
            {
                for (int yy = -outsideWidth; yy <= outsideWidth && !paint; ++yy)
                {
                    for (int xx = -outsideWidth; xx <= outsideWidth; ++xx)
                    {
                        if (std::abs(xx) > outsideWidth || std::abs(yy) > outsideWidth)
                            continue;

                        if (isInside(x + xx, y + yy))
                        {
                            paint = true;
                            break;
                        }
                    }
                }
            }

            if (paint)
                strokeMask[static_cast<size_t>(y) * static_cast<size_t>(workRect.width) + static_cast<size_t>(x)] = 255;
        }
    }

    bool hasStroke = false;

    for (unsigned char v : strokeMask)
    {
        if (v)
        {
            hasStroke = true;
            break;
        }
    }

    if (!hasStroke)
        return false;

    if (m_owner->BlendSelectionOnSelectedLayer(workRect, color, opacity, &strokeMask))
    {
        RefreshMainFrameDocumentUI(this);
        Refresh(false);
        return true;
    }

    return false;
}

bool DocumentCanvas::ShouldDrawBrushCursor() const
{
    if (!m_owner || m_panning)
        return false;

    wxTopLevelWindow* topWindow = wxDynamicCast(wxGetTopLevelParent(const_cast<DocumentCanvas*>(this)), wxTopLevelWindow);

    if (topWindow && !topWindow->IsActive())
        return false;

    const ToolType tool = m_owner->GetActiveTool();

    if (!IsBrushLikeTool(tool))
        return false;

    const wxPoint mouseScreen = wxGetMousePosition();
    const wxPoint mouseClient = const_cast<DocumentCanvas*>(this)->ScreenToClient(mouseScreen);
    const wxSize clientSize = GetClientSize();
    const wxRect clientRect(wxPoint(0, 0), clientSize);

    return clientRect.Contains(mouseClient);
}

wxPoint DocumentCanvas::GetToolAnchorDocPoint(const wxPoint& mousePos) const
{
    if (!m_owner)
        return wxPoint(0, 0);

    return m_owner->ScreenToWorldPixel(mousePos);
}

bool DocumentCanvas::IsPixelInToolShape(int bx, int by, int brushSize) const
{
    if (brushSize <= 0)
        return false;

    const double cx = static_cast<double>(brushSize) * 0.5;
    const double cy = static_cast<double>(brushSize) * 0.5;
    const double px = static_cast<double>(bx) + 0.5;
    const double py = static_cast<double>(by) + 0.5;
    const double dx = px - cx;
    const double dy = py - cy;
    const double r = static_cast<double>(brushSize) * 0.5;

    return (dx * dx + dy * dy) <= (r * r);
}

wxRect DocumentCanvas::GetBrushCursorBounds(const wxPoint& mousePos) const
{
    if (!m_owner)
        return wxRect();

    const wxPoint docPt = GetToolAnchorDocPoint(mousePos);
    const wxPoint pageOrigin = m_owner->GetPageOriginInVirtualPixels();
    const double zoom = m_owner->GetZoom();

    const int bs = std::max(1, m_brushCursorSize);
    const int half = bs / 2;

    bool first = true;
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;

    for (int by = 0; by < bs; ++by)
    {
        for (int bx = 0; bx < bs; ++bx)
        {
            if (!IsPixelInToolShape(bx, by, bs))
                continue;

            const int docX = docPt.x - half + bx;
            const int docY = docPt.y - half + by;

            const int left   = static_cast<int>(std::floor(static_cast<double>(pageOrigin.x) + static_cast<double>(docX) * zoom));
            const int top    = static_cast<int>(std::floor(static_cast<double>(pageOrigin.y) + static_cast<double>(docY) * zoom));
            const int right  = static_cast<int>(std::ceil (static_cast<double>(pageOrigin.x) + static_cast<double>(docX + 1) * zoom));
            const int bottom = static_cast<int>(std::ceil (static_cast<double>(pageOrigin.y) + static_cast<double>(docY + 1) * zoom));

            if (first)
            {
                minX = left;
                minY = top;
                maxX = right - 1;
                maxY = bottom - 1;
                first = false;
            }
            else
            {
                minX = std::min(minX, left);
                minY = std::min(minY, top);
                maxX = std::max(maxX, right - 1);
                maxY = std::max(maxY, bottom - 1);
            }
        }
    }

    if (first)
        return wxRect();

    const int margin = 4;
    return wxRect(minX - margin, minY - margin, std::max(1, maxX - minX + 1) + margin * 2, std::max(1, maxY - minY + 1) + margin * 2);
}

wxRect DocumentCanvas::GetToolStrokeDocBounds(const wxPoint& docPoint) const
{
    const int bs = std::max(1, m_brushCursorSize);
    const int half = bs / 2;

    wxRect r(docPoint.x - half - 4, docPoint.y - half - 4, bs + 8, bs + 8);

    if (m_owner)
        r.Intersect(wxRect(0, 0, m_owner->GetPageWidth(), m_owner->GetPageHeight()));

    return r;
}

void DocumentCanvas::RefreshBrushCursorAt(const wxPoint& mousePos)
{
    const wxRect r = GetBrushCursorBounds(mousePos);

    if (!r.IsEmpty())
        RefreshRect(r, false);
}

void DocumentCanvas::RefreshOldAndNewBrushCursor(const wxPoint& oldPos, const wxPoint& newPos)
{
    const wxRect oldRect = GetBrushCursorBounds(oldPos);
    const wxRect newRect = GetBrushCursorBounds(newPos);

    if (!oldRect.IsEmpty())
        RefreshRect(oldRect, false);

    if (!newRect.IsEmpty())
        RefreshRect(newRect, false);
}

bool DocumentCanvas::PickDropperColorAt(const wxPoint& mousePos)
{
    if (!m_owner)
        return false;

    const wxPoint docPt = m_owner->ScreenToWorldPixel(mousePos);

    wxColour pickedColor;
    if (!m_owner->PickVisibleColorAt(docPt, pickedColor))
        return false;

    MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
    if (!mainFrame)
        return false;

    mainFrame->SetForegroundColor(pickedColor);
    return true;
}

void DocumentCanvas::SetStampSourceAt(const wxPoint& mousePos)
{
    if (!m_owner)
        return;

    m_stampSourceDocPoint = m_owner->ScreenToWorldPixel(mousePos);
    m_stampHasSource = true;

    MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
    if (mainFrame && mainFrame->GetStatusBar())
        mainFrame->GetStatusBar()->SetStatusText(wxString::Format("Clone source: %d, %d", m_stampSourceDocPoint.x, m_stampSourceDocPoint.y), 0);

    Refresh(false);
}

wxColour DocumentCanvas::GetActiveStrokeColor() const
{
    MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(const_cast<DocumentCanvas*>(this)), MainFrame);
    return mainFrame ? mainFrame->GetForegroundColor() : *wxBLACK;
}

bool DocumentCanvas::ActiveToolUsesSquareBrush() const
{
    return m_owner && m_owner->GetActiveTool() == ToolType::Pencil;
}

bool DocumentCanvas::ActiveToolIsErase() const
{
    return m_owner && m_owner->GetActiveTool() == ToolType::Eraser;
}

void DocumentCanvas::StartInlineTextEdit(const wxPoint& canvasPoint)
{
    if (!m_owner)
        return;

    if (m_inlineTextCtrl)
        CommitInlineTextEdit();

    MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);

    const wxString fontFamily = mainFrame ? mainFrame->GetTextFontFamily() : "Sans";
    const wxString fontStyleName = mainFrame ? mainFrame->GetTextFontStyle() : "Normal";
    const int fontSize = mainFrame ? std::max(1, mainFrame->GetTextFontSize()) : 32;

    wxFontStyle fontStyle = wxFONTSTYLE_NORMAL;
    wxFontWeight fontWeight = wxFONTWEIGHT_NORMAL;

    if (fontStyleName == "Bold")
    {
        fontWeight = wxFONTWEIGHT_BOLD;
    }
    else if (fontStyleName == "Italic")
    {
        fontStyle = wxFONTSTYLE_ITALIC;
    }
    else if (fontStyleName == "Bold Italic")
    {
        fontStyle = wxFONTSTYLE_ITALIC;
        fontWeight = wxFONTWEIGHT_BOLD;
    }

    m_inlineTextDocPoint = m_owner->ScreenToWorldPixel(canvasPoint);

    m_inlineTextFont = wxFont(
        wxFontInfo(fontSize)
            .Family(wxFONTFAMILY_DEFAULT)
            .FaceName(fontFamily)
            .Style(fontStyle)
            .Weight(fontWeight)
    );

    if (!m_inlineTextFont.IsOk())
    {
        m_inlineTextFont = wxFont(
            wxFontInfo(fontSize)
                .Family(wxFONTFAMILY_SWISS)
                .Style(fontStyle)
                .Weight(fontWeight)
        );
    }

    const double zoom = std::max(0.01, m_owner->GetZoom());
    const int editorPointSize = std::max(1, static_cast<int>(static_cast<double>(fontSize) * zoom + 0.5));

    wxFont editorFont = wxFont(
        wxFontInfo(editorPointSize)
            .Family(wxFONTFAMILY_DEFAULT)
            .FaceName(fontFamily)
            .Style(fontStyle)
            .Weight(fontWeight)
    );

    if (!editorFont.IsOk())
    {
        editorFont = wxFont(
            wxFontInfo(editorPointSize)
                .Family(wxFONTFAMILY_SWISS)
                .Style(fontStyle)
                .Weight(fontWeight)
        );
    }

    const int editorW = std::max(260, static_cast<int>(360.0 * zoom + 0.5));
    const int editorH = std::max(34, static_cast<int>((static_cast<double>(fontSize) + 18.0) * zoom + 0.5));

    const wxColour textColor = mainFrame ? mainFrame->GetForegroundColor() : *wxBLACK;

    m_inlineTextCtrl = new wxTextCtrl(
        this,
        wxID_ANY,
        "",
        canvasPoint,
        wxSize(editorW, editorH),
        wxTE_PROCESS_ENTER | wxBORDER_NONE
    );

    m_inlineTextCtrl->SetFont(editorFont);
    m_inlineTextCtrl->SetForegroundColour(textColor);
    m_inlineTextCtrl->SetBackgroundColour(wxColour(255, 255, 255));
    m_inlineTextCtrl->SetInsertionPointEnd();
    m_inlineTextCtrl->SetFocus();

    m_inlineTextCtrl->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&)
    {
        CommitInlineTextEdit();
    });

    m_inlineTextCtrl->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event)
    {
        CommitInlineTextEdit();
        event.Skip();
    });

    m_inlineTextCtrl->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event)
    {
        if (event.GetKeyCode() == WXK_ESCAPE)
        {
            CancelInlineTextEdit();
            return;
        }

        if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER)
        {
            CommitInlineTextEdit();
            return;
        }

        event.Skip();
    });
}

void DocumentCanvas::CommitInlineTextEdit()
{
    if (!m_inlineTextCtrl)
        return;

    wxString text = m_inlineTextCtrl->GetValue();
    text.Trim(true);
    text.Trim(false);

    wxTextCtrl* ctrl = m_inlineTextCtrl;
    m_inlineTextCtrl = nullptr;

    ctrl->Hide();
    ctrl->Destroy();

    if (text.IsEmpty())
    {
        RestoreDocumentToolCursor(this);
        Refresh(false);
        return;
    }

    MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
    const wxColour color = mainFrame ? mainFrame->GetForegroundColor() : *wxBLACK;

    if (m_owner && m_owner->InsertTextLayerAt(m_inlineTextDocPoint, text, m_inlineTextFont, color))
    {
        if (mainFrame)
            mainFrame->RefreshDocumentDependentUI();

        Refresh(false);
    }

    RestoreDocumentToolCursor(this);
}

void DocumentCanvas::CancelInlineTextEdit()
{
    if (!m_inlineTextCtrl)
        return;

    wxTextCtrl* ctrl = m_inlineTextCtrl;
    m_inlineTextCtrl = nullptr;

    ctrl->Hide();
    ctrl->Destroy();

    RestoreDocumentToolCursor(this);
    Refresh(false);
}

bool DocumentCanvas::BeginToolStroke(const wxPoint& mousePos)
{
    if (!m_owner)
        return false;

    if (m_pendingStrokeHistory)
        FinishPendingStrokeHistory();

    const ToolType tool = m_owner->GetActiveTool();

    if (tool != ToolType::Brush && tool != ToolType::Pencil && tool != ToolType::Eraser && tool != ToolType::Stamp)
        return false;

    if (!m_owner->CanDrawOnSelectedLayer())
        return false;

    if (tool == ToolType::Stamp && !m_stampHasSource)
    {
        MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
        if (mainFrame && mainFrame->GetStatusBar())
            mainFrame->GetStatusBar()->SetStatusText("Ctrl + Click to choose clone source first", 0);

        return false;
    }

    RefreshBrushCursorAt(mousePos);
    m_lastMousePos = mousePos;
    Update();

    m_drawingStroke = true;
    m_lastDrawDocPoint = GetToolAnchorDocPoint(mousePos);
    m_stampStrokeStartDocPoint = m_lastDrawDocPoint;
    m_lastDrawExactDocPoint = wxPoint2DDouble(static_cast<double>(m_lastDrawDocPoint.x), static_cast<double>(m_lastDrawDocPoint.y));
    m_brushSpacingAccumulator = 0.0;

    const wxString historyLabel = (tool == ToolType::Brush) ? "Brush" : (tool == ToolType::Pencil) ? "Pencil" : (tool == ToolType::Eraser) ? "Eraser" : "Clone Stamp";

    m_owner->BeginLayerPatchHistoryTransaction(historyLabel);

    m_strokeBaseImage = (tool == ToolType::Stamp) ? m_owner->CopyVisibleMergedImage() : m_owner->CopySelectedLayerImage();

    if (tool == ToolType::Stamp)
    {
        wxImage selectedLayerImage = m_owner->CopySelectedLayerImage();
        m_brushStrokeMaskW = selectedLayerImage.IsOk() ? selectedLayerImage.GetWidth() : 0;
        m_brushStrokeMaskH = selectedLayerImage.IsOk() ? selectedLayerImage.GetHeight() : 0;
    }
    else
    {
        m_brushStrokeMaskW = m_strokeBaseImage.IsOk() ? m_strokeBaseImage.GetWidth() : 0;
        m_brushStrokeMaskH = m_strokeBaseImage.IsOk() ? m_strokeBaseImage.GetHeight() : 0;
    }

    m_brushStrokeMask.assign(static_cast<size_t>(m_brushStrokeMaskW) * static_cast<size_t>(m_brushStrokeMaskH), 0.0);

    ClearCachedBrushDab();

    if (tool == ToolType::Brush || tool == ToolType::Eraser || tool == ToolType::Stamp)
    {
        MainFrame* frame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);

        const bool erase = (tool == ToolType::Eraser);

        const int hardness =
            erase ? (frame ? frame->GetEraserHardness() : 100) :
            (tool == ToolType::Stamp) ? (frame ? frame->GetStampHardness() : 100) :
            (frame ? frame->GetBrushHardness() : 100);

        BuildCachedBrushDab(m_brushCursorSize, hardness);
    }

    if (tool == ToolType::Pencil)
    {
        m_owner->DrawPencilOnSelectedLayerLine(m_lastDrawDocPoint, m_lastDrawDocPoint, m_brushCursorSize, GetActiveStrokeColor());
    }
    else if (tool == ToolType::Brush || tool == ToolType::Eraser)
    {
        MainFrame* frame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);

        const bool erase = (tool == ToolType::Eraser);
        const int hardness = erase ? (frame ? frame->GetEraserHardness() : 100) : (frame ? frame->GetBrushHardness() : 100);
        const int opacity = erase ? (frame ? frame->GetEraserOpacity() : 100) : (frame ? frame->GetBrushOpacity() : 100);
        const int flow = erase ? (frame ? frame->GetEraserFlow() : 100) : (frame ? frame->GetBrushFlow() : 100);

        m_owner->DrawBrushOnSelectedLayerLine(m_lastDrawDocPoint, m_lastDrawDocPoint, m_brushCursorSize, hardness, opacity, flow, GetActiveStrokeColor(), &m_strokeBaseImage, &m_brushStrokeMask, erase, &m_cachedBrushDab);
    }
    else if (tool == ToolType::Stamp)
    {
        MainFrame* frame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);

        const int hardness = frame ? frame->GetStampHardness() : 100;
        const int opacity = frame ? frame->GetStampOpacity() : 100;
        const int flow = frame ? frame->GetStampFlow() : 100;

        m_owner->DrawCloneStampOnSelectedLayerLine(m_lastDrawDocPoint, m_lastDrawDocPoint, m_stampSourceDocPoint, m_stampSourceDocPoint, m_brushCursorSize, hardness, opacity, flow, &m_strokeBaseImage, &m_brushStrokeMask, &m_cachedBrushDab);
    }

    m_owner->ExpandLayerPatchHistoryRect(GetToolStrokeDocBounds(m_lastDrawDocPoint));

    return true;
}

void DocumentCanvas::BuildCachedBrushDab(int size, int hardness)
{
    const int brushSizeInt = std::max(1, size);
    const int hardnessClamped = std::max(0, std::min(100, hardness));

    if (m_cachedBrushDabSize == brushSizeInt && m_cachedBrushDabHardness == hardnessClamped && !m_cachedBrushDab.empty())
        return;

    m_cachedBrushDab.clear();
    m_cachedBrushDabSize = brushSizeInt;
    m_cachedBrushDabHardness = hardnessClamped;

    const int half = brushSizeInt / 2;
    const double brushSize = static_cast<double>(brushSizeInt);
    const double radius = brushSize * 0.5;
    const double radiusSq = radius * radius;

    const double hard01 = static_cast<double>(hardnessClamped) / 100.0;
    const double hardRadius = radius * (0.03 + hard01 * 0.45);
    const double softRange = std::max(1.0, radius - hardRadius);
    const double center = brushSize * 0.5;

    m_cachedBrushDab.reserve(static_cast<size_t>(brushSizeInt) * static_cast<size_t>(brushSizeInt));

    for (int by = 0; by < brushSizeInt; ++by)
    {
        for (int bx = 0; bx < brushSizeInt; ++bx)
        {
            const double px = static_cast<double>(bx) + 0.5;
            const double py = static_cast<double>(by) + 0.5;
            const double dx = px - center;
            const double dy = py - center;
            const double distSq = dx * dx + dy * dy;

            if (distSq > radiusSq)
                continue;

            const double dist = std::sqrt(distSq);

            double strength = 1.0;

            if (hardnessClamped == 0)
            {
                // Photoshop 7 / GIMP-style: squared cosine bell.
                // Falls to ~6% at 50% radius and ~0.4% at 70% radius,
                // producing a soft feathered spot with no hard core.
                const double norm = dist / radius;
                const double bell = 0.5 * (1.0 + std::cos(M_PI * norm));
                strength = bell * bell;
            }
            else if (dist > hardRadius)
            {
                double t = (dist - hardRadius) / softRange;
                t = std::max(0.0, std::min(1.0, t));
                t = t * t * (3.0 - 2.0 * t);
                strength = 1.0 - t;
            }

            if (strength <= 0.0)
                continue;

            BrushDabPixel p;
            p.ox = bx - half;
            p.oy = by - half;
            p.strength = strength;
            m_cachedBrushDab.push_back(p);
        }
    }
}

void DocumentCanvas::ClearCachedBrushDab()
{
    m_cachedBrushDab.clear();
    m_cachedBrushDabSize = 0;
    m_cachedBrushDabHardness = -1;
}

void DocumentCanvas::ContinueToolStroke(const wxPoint& mousePos)
{
    if (!m_owner || !m_drawingStroke)
        return;

    const wxPoint oldMousePos = m_lastMousePos;
    m_lastMousePos = mousePos;
    RefreshOldAndNewBrushCursor(oldMousePos, mousePos);

    const wxPoint docPt = GetToolAnchorDocPoint(mousePos);

    if (docPt == m_lastDrawDocPoint)
        return;

    const ToolType tool = m_owner->GetActiveTool();

    if (tool == ToolType::Pencil)
    {
        m_owner->DrawPencilOnSelectedLayerLine(m_lastDrawDocPoint, docPt, m_brushCursorSize, GetActiveStrokeColor());

        wxRect changedRect = GetToolStrokeDocBounds(m_lastDrawDocPoint);
        changedRect.Union(GetToolStrokeDocBounds(docPt));
        m_owner->ExpandLayerPatchHistoryRect(changedRect);

        m_lastDrawDocPoint = docPt;
        m_lastDrawExactDocPoint = wxPoint2DDouble(static_cast<double>(docPt.x), static_cast<double>(docPt.y));
        return;
    }

    const double spacing = std::max(1.0, static_cast<double>(m_brushCursorSize) * (static_cast<double>(m_brushSpacingPercent) / 100.0));

    wxPoint2DDouble currentExact(static_cast<double>(docPt.x), static_cast<double>(docPt.y));
    wxPoint2DDouble fromExact = m_lastDrawExactDocPoint;

    double dx = currentExact.m_x - fromExact.m_x;
    double dy = currentExact.m_y - fromExact.m_y;
    double distance = std::sqrt(dx * dx + dy * dy);

    if (distance <= 0.0)
        return;

    MainFrame* frame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);

    const bool erase = (tool == ToolType::Eraser);

    int hardness = 100;
    int opacity = 100;
    int flow = 100;

    if (tool == ToolType::Brush || tool == ToolType::Eraser)
    {
        hardness = erase ? (frame ? frame->GetEraserHardness() : 100) : (frame ? frame->GetBrushHardness() : 100);
        opacity = erase ? (frame ? frame->GetEraserOpacity() : 100) : (frame ? frame->GetBrushOpacity() : 100);
        flow = erase ? (frame ? frame->GetEraserFlow() : 100) : (frame ? frame->GetBrushFlow() : 100);
    }
    else if (tool == ToolType::Stamp)
    {
        hardness = frame ? frame->GetStampHardness() : 100;
        opacity = frame ? frame->GetStampOpacity() : 100;
        flow = frame ? frame->GetStampFlow() : 100;
    }
    else
    {
        m_lastDrawDocPoint = docPt;
        m_lastDrawExactDocPoint = currentExact;
        return;
    }

    bool drewSomething = false;

    while (m_brushSpacingAccumulator + distance >= spacing)
    {
        const double need = spacing - m_brushSpacingAccumulator;
        const double t = (distance > 0.0) ? (need / distance) : 1.0;

        wxPoint2DDouble nextExact(fromExact.m_x + dx * t, fromExact.m_y + dy * t);

        wxPoint nextDoc(static_cast<int>(std::lround(nextExact.m_x)), static_cast<int>(std::lround(nextExact.m_y)));

        if (nextDoc != m_lastDrawDocPoint)
        {
            if (tool == ToolType::Brush || tool == ToolType::Eraser)
            {
                m_owner->DrawBrushOnSelectedLayerLine(m_lastDrawDocPoint, nextDoc, m_brushCursorSize, hardness, opacity, flow, GetActiveStrokeColor(), &m_strokeBaseImage, &m_brushStrokeMask, erase, &m_cachedBrushDab);
            }
            else if (tool == ToolType::Stamp)
            {
                const wxPoint sourceStart(
                    m_stampSourceDocPoint.x + (m_lastDrawDocPoint.x - m_stampStrokeStartDocPoint.x),
                    m_stampSourceDocPoint.y + (m_lastDrawDocPoint.y - m_stampStrokeStartDocPoint.y)
                );

                const wxPoint sourceEnd(
                    m_stampSourceDocPoint.x + (nextDoc.x - m_stampStrokeStartDocPoint.x),
                    m_stampSourceDocPoint.y + (nextDoc.y - m_stampStrokeStartDocPoint.y)
                );

                m_owner->DrawCloneStampOnSelectedLayerLine(m_lastDrawDocPoint, nextDoc, sourceStart, sourceEnd, m_brushCursorSize, hardness, opacity, flow, &m_strokeBaseImage, &m_brushStrokeMask, &m_cachedBrushDab);
            }

            wxRect changedRect = GetToolStrokeDocBounds(m_lastDrawDocPoint);
            changedRect.Union(GetToolStrokeDocBounds(nextDoc));
            m_owner->ExpandLayerPatchHistoryRect(changedRect);

            m_lastDrawDocPoint = nextDoc;
            drewSomething = true;
        }

        fromExact = nextExact;
        m_lastDrawExactDocPoint = nextExact;
        m_brushSpacingAccumulator = 0.0;

        dx = currentExact.m_x - fromExact.m_x;
        dy = currentExact.m_y - fromExact.m_y;
        distance = std::sqrt(dx * dx + dy * dy);

        if (distance <= 0.0)
            break;
    }

    m_brushSpacingAccumulator += distance;

    if (!drewSomething)
        return;
}

void DocumentCanvas::StopDrawingStroke()
{
    if (!m_drawingStroke)
        return;

    wxImage beforeImage = m_strokeBaseImage;

    m_drawingStroke = false;
    m_strokeBaseImage = wxImage();
    m_brushStrokeMask.clear();
    m_brushStrokeMaskW = 0;
    m_brushStrokeMaskH = 0;
    m_brushSpacingAccumulator = 0.0;
    m_lastDrawExactDocPoint = wxPoint2DDouble(0.0, 0.0);
    ClearCachedBrushDab();

    if (HasCapture())
        ReleaseMouse();

    Refresh(false);

    if (beforeImage.IsOk())
    {
        m_pendingStrokeBeforeImage = beforeImage;
        m_pendingStrokeHistory = true;
        m_pendingStrokeHistoryUI = false;

        FinishPendingStrokeHistory();
    }
    else
    {
        FinishPendingStrokeHistory();
    }
}

void DocumentCanvas::FinishPendingStrokeHistory()
{
    if (!m_owner)
        return;

    if (!m_pendingStrokeHistory)
        return;

    if (m_drawingStroke || wxGetMouseState().LeftIsDown())
        return;

    wxImage beforeImage = m_pendingStrokeBeforeImage;

    m_pendingStrokeHistory = false;
    m_pendingStrokeBeforeImage = wxImage();

    m_owner->EndLayerPatchHistoryTransaction(beforeImage.IsOk() ? &beforeImage : nullptr);

    m_pendingStrokeHistoryUI = true;

    wxTheApp->CallAfter([this]()
    {
        if (!this)
            return;

        if (!m_pendingStrokeHistoryUI)
            return;

        m_pendingStrokeHistoryUI = false;

        MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
        if (mainFrame)
            mainFrame->RefreshAfterBrushStroke();
    });
}

void DocumentCanvas::DrawBrushCursor(wxDC& dc)
{
    if (!ShouldDrawBrushCursor() || !m_owner)
        return;

    const wxPoint docPt = GetToolAnchorDocPoint(m_lastMousePos);
    const wxPoint pageOrigin = m_owner->GetPageOriginInVirtualPixels();
    const double zoom = m_owner->GetZoom();

    const int bs = std::max(1, m_brushCursorSize);
    const int half = bs / 2;

    const bool lowZoomPreview = (zoom < 4.0);

    auto drawDashLine = [&dc, lowZoomPreview](int x1, int y1, int x2, int y2, bool horizontal)
    {
        const int dashLen = lowZoomPreview ? 2 : 4;
        const int gapLen = lowZoomPreview ? 2 : 4;
        const int patternLen = dashLen + gapLen;

        if (horizontal)
        {
            if (x2 < x1)
                std::swap(x1, x2);

            const int length = x2 - x1 + 1;
            if (length <= 0)
                return;

            for (int pos = 0; pos < length; pos += patternLen)
            {
                const int blackStart = pos;
                const int blackEnd = std::min(length, pos + dashLen);
                const int whiteStart = std::min(length, pos + dashLen);
                const int whiteEnd = std::min(length, pos + dashLen + dashLen);

                if (blackStart < blackEnd)
                {
                    dc.SetPen(wxPen(*wxBLACK, 1));
                    dc.DrawLine(x1 + blackStart, y1, x1 + blackEnd - 1, y1);
                }

                if (whiteStart < whiteEnd)
                {
                    dc.SetPen(wxPen(*wxWHITE, 1));
                    dc.DrawLine(x1 + whiteStart, y1, x1 + whiteEnd - 1, y1);
                }
            }
        }
        else
        {
            if (y2 < y1)
                std::swap(y1, y2);

            const int length = y2 - y1 + 1;
            if (length <= 0)
                return;

            for (int pos = 0; pos < length; pos += patternLen)
            {
                const int blackStart = pos;
                const int blackEnd = std::min(length, pos + dashLen);
                const int whiteStart = std::min(length, pos + dashLen);
                const int whiteEnd = std::min(length, pos + dashLen + dashLen);

                if (blackStart < blackEnd)
                {
                    dc.SetPen(wxPen(*wxBLACK, 1));
                    dc.DrawLine(x1, y1 + blackStart, x1, y1 + blackEnd - 1);
                }

                if (whiteStart < whiteEnd)
                {
                    dc.SetPen(wxPen(*wxWHITE, 1));
                    dc.DrawLine(x1, y1 + whiteStart, x1, y1 + whiteEnd - 1);
                }
            }
        }
    };

    dc.SetBrush(*wxTRANSPARENT_BRUSH);

    for (int by = 0; by < bs; ++by)
    {
        for (int bx = 0; bx < bs; ++bx)
        {
            if (!IsPixelInToolShape(bx, by, bs))
                continue;

            const int docPX = docPt.x - half + bx;
            const int docPY = docPt.y - half + by;

            const int left   = static_cast<int>(std::floor(static_cast<double>(pageOrigin.x) + static_cast<double>(docPX) * zoom));
            const int top    = static_cast<int>(std::floor(static_cast<double>(pageOrigin.y) + static_cast<double>(docPY) * zoom));
            const int right  = static_cast<int>(std::ceil (static_cast<double>(pageOrigin.x) + static_cast<double>(docPX + 1) * zoom)) - 1;
            const int bottom = static_cast<int>(std::ceil (static_cast<double>(pageOrigin.y) + static_cast<double>(docPY + 1) * zoom)) - 1;

            const bool topEdge    = (by == 0)      || !IsPixelInToolShape(bx, by - 1, bs);
            const bool bottomEdge = (by == bs - 1) || !IsPixelInToolShape(bx, by + 1, bs);
            const bool leftEdge   = (bx == 0)      || !IsPixelInToolShape(bx - 1, by, bs);
            const bool rightEdge  = (bx == bs - 1) || !IsPixelInToolShape(bx + 1, by, bs);

            if (topEdge)
                drawDashLine(left - 1, top - 1, right + 1, top - 1, true);

            if (bottomEdge)
                drawDashLine(left - 1, bottom + 1, right + 1, bottom + 1, true);

            if (leftEdge)
                drawDashLine(left - 1, top - 1, left - 1, bottom + 1, false);

            if (rightEdge)
                drawDashLine(right + 1, top - 1, right + 1, bottom + 1, false);
        }
    }
}

void DocumentCanvas::DrawGradientOverlay(wxDC& dc)
{
    if (!m_owner || !m_gradientDragging)
        return;

    const wxPoint start = m_owner->WorldToCanvasVirtual(m_gradientStartDoc);
    const wxPoint end = m_owner->WorldToCanvasVirtual(m_gradientCurrentDoc);

    dc.SetPen(wxPen(wxColour(0, 0, 0), 3));
    dc.DrawLine(start, end);

    dc.SetPen(wxPen(wxColour(255, 255, 255), 1));
    dc.DrawLine(start, end);

    dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
    dc.SetPen(wxPen(wxColour(0, 0, 0), 1));
    dc.DrawCircle(start, 4);
    dc.DrawCircle(end, 4);
}

void DocumentCanvas::StopGradientDrag(bool apply)
{
    if (!m_gradientDragging)
        return;

    m_gradientDragging = false;

    if (HasCapture())
        ReleaseMouse();

    if (apply && m_owner && m_gradientStartDoc != m_gradientCurrentDoc)
    {
        MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);

        const GradientData gradient = mainFrame ? mainFrame->GetGradientData() : GradientData();

        wxRect selectionRect;
        std::vector<unsigned char> selectionMask;

        bool changed = false;

        if (GetSelectionMask(selectionRect, selectionMask))
            changed = m_owner->ApplyLinearGradientToSelectedLayer(m_gradientStartDoc, m_gradientCurrentDoc, gradient, &selectionRect, &selectionMask);
        else
            changed = m_owner->ApplyLinearGradientToSelectedLayer(m_gradientStartDoc, m_gradientCurrentDoc, gradient, nullptr, nullptr);

        if (changed && mainFrame)
            mainFrame->RefreshDocumentDependentUI();
    }

    RestoreDocumentToolCursor(this);
    Refresh(false);
}

void DocumentCanvas::DrawGradientDragLine(wxDC& dc)
{
    if (!m_owner || !m_gradientDragging)
        return;

    const wxPoint start = m_owner->WorldToCanvasVirtual(m_gradientStartDoc);
    const wxPoint end = m_owner->WorldToCanvasVirtual(m_gradientCurrentDoc);

    dc.SetPen(wxPen(wxColour(0, 0, 0), 3));
    dc.DrawLine(start, end);

    dc.SetPen(wxPen(wxColour(255, 255, 255), 1));
    dc.DrawLine(start, end);

    dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
    dc.SetPen(wxPen(wxColour(0, 0, 0), 1));
    dc.DrawCircle(start, 3);
    dc.DrawCircle(end, 3);
}

void DocumentCanvas::DrawCheckerboard(wxDC& dc, const wxRect& visibleRect, const wxPoint& pageOrigin)
{
    // visibleRect is already clipped to the canvas window, so cell count is
    // bounded by the viewport size (~5 000 cells max) regardless of zoom.
    // The pattern is phase-aligned to pageOrigin so it is stable while panning.
    const int cellSize = 14;

    // Use floor-division so negative relative coords work correctly
    auto floorDiv = [](int a, int b) -> int {
        return a / b - (a % b != 0 && (a ^ b) < 0);
    };

    const int firstCX = floorDiv(visibleRect.x - pageOrigin.x, cellSize);
    const int firstCY = floorDiv(visibleRect.y - pageOrigin.y, cellSize);
    const int lastCX  = floorDiv(visibleRect.GetRight()  - 1 - pageOrigin.x, cellSize);
    const int lastCY  = floorDiv(visibleRect.GetBottom() - 1 - pageOrigin.y, cellSize);

    const wxBrush brushA(wxColour(215, 215, 215));
    const wxBrush brushB(wxColour(245, 245, 245));
    dc.SetPen(*wxTRANSPARENT_PEN);

    for (int cy = firstCY; cy <= lastCY; ++cy)
    {
        const int cellTop = pageOrigin.y + cy * cellSize;
        const int y0 = std::max(cellTop,            visibleRect.y);
        const int y1 = std::min(cellTop + cellSize, visibleRect.GetBottom());
        if (y0 >= y1) continue;

        for (int cx = firstCX; cx <= lastCX; ++cx)
        {
            const int cellLeft = pageOrigin.x + cx * cellSize;
            const int x0 = std::max(cellLeft,            visibleRect.x);
            const int x1 = std::min(cellLeft + cellSize, visibleRect.GetRight());
            if (x0 >= x1) continue;

            // (cx ^ cy) & 1 handles negative indices correctly
            dc.SetBrush(((cx ^ cy) & 1) == 0 ? brushA : brushB);
            dc.DrawRectangle(x0, y0, x1 - x0, y1 - y0);
        }
    }
}

void DocumentCanvas::DrawMarchingAnts(wxDC& dc, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;

    const int left = x;
    const int top = y;
    const int right = x + w - 1;
    const int bottom = y + h - 1;

    DrawMarchingAntsLine(dc, wxPoint(left, top), wxPoint(right, top));
    DrawMarchingAntsLine(dc, wxPoint(right, top), wxPoint(right, bottom));
    DrawMarchingAntsLine(dc, wxPoint(right, bottom), wxPoint(left, bottom));
    DrawMarchingAntsLine(dc, wxPoint(left, bottom), wxPoint(left, top));
}

void DocumentCanvas::DrawMarchingAntsLine(wxDC& dc, const wxPoint& a, const wxPoint& b)
{
    const int dashLen = 4;
    const int gapLen = 4;
    const int patternLen = dashLen + gapLen;

    const double dx = static_cast<double>(b.x - a.x);
    const double dy = static_cast<double>(b.y - a.y);
    const double length = std::sqrt(dx * dx + dy * dy);

    if (length <= 0.0)
        return;

    const double ux = dx / length;
    const double uy = dy / length;

    for (int pos = -m_selectionAnimOffset; pos < static_cast<int>(length); pos += patternLen)
    {
        const int blackStart = std::max(0, pos);
        const int blackEnd = std::min(static_cast<int>(length), pos + dashLen / 2);
        const int whiteStart = std::max(0, pos + dashLen / 2);
        const int whiteEnd = std::min(static_cast<int>(length), pos + dashLen);

        if (blackStart < blackEnd)
        {
            dc.SetPen(wxPen(*wxBLACK, 1));
            dc.DrawLine(
                static_cast<int>(std::round(a.x + ux * blackStart)),
                static_cast<int>(std::round(a.y + uy * blackStart)),
                static_cast<int>(std::round(a.x + ux * blackEnd)),
                static_cast<int>(std::round(a.y + uy * blackEnd))
            );
        }

        if (whiteStart < whiteEnd)
        {
            dc.SetPen(wxPen(*wxWHITE, 1));
            dc.DrawLine(
                static_cast<int>(std::round(a.x + ux * whiteStart)),
                static_cast<int>(std::round(a.y + uy * whiteStart)),
                static_cast<int>(std::round(a.x + ux * whiteEnd)),
                static_cast<int>(std::round(a.y + uy * whiteEnd))
            );
        }
    }
}

void DocumentCanvas::DrawMarqueeOverlay(wxDC& dc)
{
    if (!m_owner)
        return;

    if (m_lassoDragging || m_lassoHasSelection)
    {
        DrawLassoOverlay(dc);
        return;
    }

    if (!m_marqueeDragging && !m_marqueeHasSelection)
        return;

    const wxRect marqueeRect = GetMarqueeRect();

    if (marqueeRect.width <= 0 || marqueeRect.height <= 0)
        return;

    const wxPoint pageOrigin = m_owner->GetPageOriginInVirtualPixels();
    const double zoom = m_owner->GetZoom();

    const int borderX = pageOrigin.x + static_cast<int>(std::round(static_cast<double>(marqueeRect.x) * zoom));
    const int borderY = pageOrigin.y + static_cast<int>(std::round(static_cast<double>(marqueeRect.y) * zoom));
    const int borderW = std::max(1, static_cast<int>(std::round(static_cast<double>(marqueeRect.width) * zoom)));
    const int borderH = std::max(1, static_cast<int>(std::round(static_cast<double>(marqueeRect.height) * zoom)));

    if (!m_marqueeElliptical)
    {
        DrawMarchingAnts(dc, borderX, borderY, borderW, borderH);
        return;
    }

    const double cx = static_cast<double>(borderX) + static_cast<double>(borderW) * 0.5;
    const double cy = static_cast<double>(borderY) + static_cast<double>(borderH) * 0.5;
    const double rx = static_cast<double>(borderW) * 0.5;
    const double ry = static_cast<double>(borderH) * 0.5;

    if (rx <= 0.0 || ry <= 0.0)
        return;

    const int steps = std::max(96, static_cast<int>((rx + ry) * 1.4));
    const int dashLen = 4;
    const int gapLen = 4;
    const int patternLen = dashLen + gapLen;

    wxPoint prev(
        static_cast<int>(std::round(cx + rx)),
        static_cast<int>(std::round(cy))
    );

    double distanceAlong = 0.0;

    for (int i = 1; i <= steps; ++i)
    {
        const double t = (static_cast<double>(i) / static_cast<double>(steps)) * 6.28318530717958647692;

        wxPoint cur(
            static_cast<int>(std::round(cx + std::cos(t) * rx)),
            static_cast<int>(std::round(cy + std::sin(t) * ry))
        );

        const int dx = cur.x - prev.x;
        const int dy = cur.y - prev.y;
        const double segmentLen = std::sqrt(static_cast<double>(dx * dx + dy * dy));

        if (segmentLen > 0.0)
        {
            const int pattern = (static_cast<int>(distanceAlong) + m_selectionAnimOffset) % patternLen;

            if (pattern < dashLen)
                dc.SetPen(wxPen(*wxBLACK, 1));
            else
                dc.SetPen(wxPen(*wxWHITE, 1));

            dc.DrawLine(prev, cur);
            distanceAlong += segmentLen;
        }

        prev = cur;
    }
}

void DocumentCanvas::DrawLassoOverlay(wxDC& dc)
{
    if (!m_owner)
        return;

    if (m_lassoPoints.size() < 2)
        return;

    const wxPoint pageOrigin = m_owner->GetPageOriginInVirtualPixels();
    const double zoom = m_owner->GetZoom();

    auto toScreen = [&](const wxPoint& p) -> wxPoint
    {
        return wxPoint(
            pageOrigin.x + static_cast<int>(std::round(static_cast<double>(p.x) * zoom)),
            pageOrigin.y + static_cast<int>(std::round(static_cast<double>(p.y) * zoom))
        );
    };

    for (size_t i = 1; i < m_lassoPoints.size(); ++i)
        DrawMarchingAntsLine(dc, toScreen(m_lassoPoints[i - 1]), toScreen(m_lassoPoints[i]));

    if (m_lassoHasSelection && m_lassoPoints.size() > 2)
        DrawMarchingAntsLine(dc, toScreen(m_lassoPoints.back()), toScreen(m_lassoPoints.front()));
}

wxPoint DocumentCanvas::ClampDocPointToPage(const wxPoint& p) const
{
    if (!m_owner)
        return p;

    return wxPoint(
        std::max(0, std::min(m_owner->GetPageWidth(), p.x)),
        std::max(0, std::min(m_owner->GetPageHeight(), p.y))
    );
}

wxRect DocumentCanvas::NormalizedRectFromPoints(const wxPoint& a, const wxPoint& b) const
{
    const int x1 = std::min(a.x, b.x);
    const int y1 = std::min(a.y, b.y);
    const int x2 = std::max(a.x, b.x);
    const int y2 = std::max(a.y, b.y);

    return wxRect(x1, y1, x2 - x1, y2 - y1);
}

wxRect DocumentCanvas::GetCropRect() const
{
    if (!m_cropHasSelection && !m_cropDragging)
        return wxRect();

    return NormalizedRectFromPoints(m_cropStartDoc, m_cropCurrentDoc);
}

void DocumentCanvas::ShowFreeTransformBox()
{
    if (!m_owner)
        return;

    if (m_freeTransformVisible)
        return;

    m_transformSessionStartRect = GetSelectedLayerDocRect();
    m_transformSessionStartImage = m_owner->CopySelectedLayerImage();

    if (m_transformSessionStartRect.width <= 0 || m_transformSessionStartRect.height <= 0 || !m_transformSessionStartImage.IsOk())
        return;

    m_freeTransformVisible = true;
    m_transformPivotDragging = false;
    m_transformScaling = false;
    m_activeTransformHandle = TransformHandle::None;
    m_transformPivotCustom = false;
    m_transformPivotDoc = wxPoint(
        m_transformSessionStartRect.x + m_transformSessionStartRect.width / 2,
        m_transformSessionStartRect.y + m_transformSessionStartRect.height / 2
    );

    Refresh(false);
}

void DocumentCanvas::HideFreeTransformBox()
{
    if (!m_freeTransformVisible)
        return;

    StopFreeTransformScale();
    StopFreeTransformPivotDrag();

    m_freeTransformVisible = false;
    m_transformPivotCustom = false;
    m_transformPivotDoc = wxPoint(0, 0);
    m_transformSessionStartRect = wxRect();
    m_transformSessionStartImage = wxImage();

    RestoreDocumentToolCursor(this);
    Refresh(false);
}

void DocumentCanvas::CancelFreeTransformBox()
{
    if (!m_owner || !m_freeTransformVisible)
        return;

    StopFreeTransformPivotDrag();

    if (m_transformScaling)
    {
        m_transformScaling = false;
        m_activeTransformHandle = TransformHandle::None;

        if (HasCapture())
            ReleaseMouse();

        if (m_owner)
            m_owner->EndHistoryTransaction();
    }

    if (m_transformSessionStartImage.IsOk() && m_transformSessionStartRect.width > 0 && m_transformSessionStartRect.height > 0)
    {
        m_owner->BeginHistoryTransaction("Cancel Free Transform");
        m_owner->TransformSelectedLayerToRect(m_transformSessionStartImage, m_transformSessionStartRect);
        m_owner->EndHistoryTransaction();
    }

    m_freeTransformVisible = false;
    m_transformPivotDragging = false;
    m_transformPivotCustom = false;
    m_transformPivotDoc = wxPoint(0, 0);
    m_transformStartRect = wxRect();
    m_transformStartImage = wxImage();
    m_transformSessionStartRect = wxRect();
    m_transformSessionStartImage = wxImage();

    MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
    if (mainFrame)
        mainFrame->RefreshDocumentDependentUI();

    RestoreDocumentToolCursor(this);
    Refresh(false);
}

bool DocumentCanvas::HasCropSelection() const
{
    const wxRect r = GetCropRect();
    return m_cropHasSelection && r.width > 0 && r.height > 0;
}

void DocumentCanvas::ClearCropSelection()
{
    const bool hadSelection = m_cropDragging || m_cropHasSelection;

    m_cropDragging = false;
    m_cropHasSelection = false;
    m_cropStartDoc = wxPoint(0, 0);
    m_cropCurrentDoc = wxPoint(0, 0);
    m_cropDragStartDoc = wxPoint(0, 0);
    m_cropDragStartRect = wxRect();
    m_activeCropHandle = CropHandle::None;

    const bool hasOtherAnimatedSelection =
        m_marqueeDragging ||
        m_marqueeHasSelection ||
        m_lassoDragging ||
        m_lassoHasSelection;

    if (m_selectionAnimTimer.IsRunning() && !hasOtherAnimatedSelection)
        m_selectionAnimTimer.Stop();

    if (hadSelection)
    {
        MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
        if (mainFrame)
            mainFrame->RefreshDocumentDependentUI();
    }

    Refresh(false);
}

wxRect DocumentCanvas::GetCropScreenRect() const
{
    if (!m_owner)
        return wxRect();

    const wxRect cropRect = GetCropRect();

    if (cropRect.width <= 0 || cropRect.height <= 0)
        return wxRect();

    const wxPoint p1 = m_owner->WorldToCanvasVirtual(wxPoint(cropRect.x, cropRect.y));
    const wxPoint p2 = m_owner->WorldToCanvasVirtual(wxPoint(cropRect.x + cropRect.width, cropRect.y + cropRect.height));

    const int left = std::min(p1.x, p2.x);
    const int top = std::min(p1.y, p2.y);
    const int right = std::max(p1.x, p2.x);
    const int bottom = std::max(p1.y, p2.y);

    if (right <= left || bottom <= top)
        return wxRect();

    return wxRect(left, top, right - left, bottom - top);
}

DocumentCanvas::CropHandle DocumentCanvas::HitTestCropHandle(const wxPoint& mousePos) const
{
    if (!HasCropSelection())
        return CropHandle::None;

    const wxRect r = GetCropScreenRect();

    if (r.width <= 0 || r.height <= 0)
        return CropHandle::None;

    const int hs = 12;
    const int half = hs / 2;

    const int left = r.x;
    const int top = r.y;
    const int right = r.x + r.width;
    const int bottom = r.y + r.height;
    const int cx = left + r.width / 2;
    const int cy = top + r.height / 2;

    auto handleRect = [&](int x, int y)
    {
        return wxRect(x - half, y - half, hs, hs);
    };

    if (handleRect(left, top).Contains(mousePos))
        return CropHandle::TopLeft;

    if (handleRect(cx, top).Contains(mousePos))
        return CropHandle::Top;

    if (handleRect(right, top).Contains(mousePos))
        return CropHandle::TopRight;

    if (handleRect(right, cy).Contains(mousePos))
        return CropHandle::Right;

    if (handleRect(right, bottom).Contains(mousePos))
        return CropHandle::BottomRight;

    if (handleRect(cx, bottom).Contains(mousePos))
        return CropHandle::Bottom;

    if (handleRect(left, bottom).Contains(mousePos))
        return CropHandle::BottomLeft;

    if (handleRect(left, cy).Contains(mousePos))
        return CropHandle::Left;

    if (r.Contains(mousePos))
        return CropHandle::Inside;

    return CropHandle::None;
}

wxCursor DocumentCanvas::GetCropHandleCursor(CropHandle handle) const
{
    switch (handle)
    {
        case CropHandle::TopLeft:
        case CropHandle::BottomRight:
            return wxCursor(wxCURSOR_SIZENWSE);

        case CropHandle::TopRight:
        case CropHandle::BottomLeft:
            return wxCursor(wxCURSOR_SIZENESW);

        case CropHandle::Top:
        case CropHandle::Bottom:
            return wxCursor(wxCURSOR_SIZENS);

        case CropHandle::Left:
        case CropHandle::Right:
            return wxCursor(wxCURSOR_SIZEWE);

        case CropHandle::Inside:
            return wxCursor(wxCURSOR_SIZING);

        default:
            return wxCursor(wxCURSOR_ARROW);
    }
}

void DocumentCanvas::SetCropRectFromEdges(int left, int top, int right, int bottom)
{
    if (!m_owner)
        return;

    left = std::max(0, std::min(m_owner->GetPageWidth(), left));
    right = std::max(0, std::min(m_owner->GetPageWidth(), right));
    top = std::max(0, std::min(m_owner->GetPageHeight(), top));
    bottom = std::max(0, std::min(m_owner->GetPageHeight(), bottom));

    m_cropStartDoc = wxPoint(left, top);
    m_cropCurrentDoc = wxPoint(right, bottom);
}

void DocumentCanvas::StopCropDrag(bool keepSelection)
{
    if (!m_cropDragging && !m_cropHasSelection)
        return;

    m_cropDragging = false;
    m_cropHasSelection = keepSelection;
    m_activeCropHandle = CropHandle::None;

    if (HasCapture() && !m_panning && !m_movingLayer && !m_marqueeDragging && !m_lassoDragging && !m_drawingStroke)
        ReleaseMouse();

    MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
    if (mainFrame)
        mainFrame->RefreshDocumentDependentUI();

    if (m_cropHasSelection && !m_selectionAnimTimer.IsRunning())
        m_selectionAnimTimer.Start(100);

    Refresh(false);
}

void DocumentCanvas::DrawCropOverlay(wxDC& dc)
{
    if (!m_owner)
        return;

    if (!m_cropDragging && !m_cropHasSelection)
        return;

    const wxRect r = GetCropScreenRect();
    if (r.width <= 0 || r.height <= 0)
        return;

    const int left = r.x;
    const int top = r.y;
    const int right = r.x + r.width;
    const int bottom = r.y + r.height;

    DrawMarchingAnts(dc, left, top, r.width, r.height);

    dc.SetPen(wxPen(wxColour(255, 255, 255), 1));
    dc.SetBrush(wxBrush(wxColour(35, 35, 35)));

    const int hs = 8;
    const int half = hs / 2;
    const int cx = left + r.width / 2;
    const int cy = top + r.height / 2;

    auto drawHandle = [&](int x, int y)
    {
        dc.DrawRectangle(x - half, y - half, hs, hs);
    };

    drawHandle(left, top);
    drawHandle(cx, top);
    drawHandle(right, top);
    drawHandle(right, cy);
    drawHandle(right, bottom);
    drawHandle(cx, bottom);
    drawHandle(left, bottom);
    drawHandle(left, cy);
}

void DocumentCanvas::DrawFreeTransformOverlay(wxDC& dc)
{
    if (!m_owner || !m_freeTransformVisible)
        return;

    const int layerIndex = m_owner->GetSelectedLayer();
    const DocumentLayer* layer = m_owner->GetLayer(layerIndex);

    if (!layer || !layer->image.IsOk())
        return;

    const wxPoint pageOrigin = m_owner->GetPageOriginInVirtualPixels();
    const double zoom = m_owner->GetZoom();

    const int layerLeft = layer->offsetX;
    const int layerTop = layer->offsetY;
    const int layerRight = layer->offsetX + layer->image.GetWidth();
    const int layerBottom = layer->offsetY + layer->image.GetHeight();

    const int left = static_cast<int>(std::floor(static_cast<double>(pageOrigin.x) + static_cast<double>(layerLeft) * zoom));
    const int top = static_cast<int>(std::floor(static_cast<double>(pageOrigin.y) + static_cast<double>(layerTop) * zoom));
    const int right = static_cast<int>(std::ceil(static_cast<double>(pageOrigin.x) + static_cast<double>(layerRight) * zoom));
    const int bottom = static_cast<int>(std::ceil(static_cast<double>(pageOrigin.y) + static_cast<double>(layerBottom) * zoom));

    if (right <= left || bottom <= top)
        return;

    const wxRect boxRect(left, top, right - left, bottom - top);

    dc.SetPen(wxPen(wxColour(0, 0, 0), 1, wxPENSTYLE_SOLID));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(boxRect);

    const int handleSize = 7;
    const int half = handleSize / 2;

    auto drawHandle = [&](int cx, int cy)
    {
        wxRect r(cx - half, cy - half, handleSize, handleSize);
        dc.SetPen(wxPen(wxColour(0, 0, 0), 1));
        dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
        dc.DrawRectangle(r);
    };

    const int cx = left + (right - left) / 2;
    const int cy = top + (bottom - top) / 2;

    drawHandle(left, top);
    drawHandle(cx, top);
    drawHandle(right, top);
    drawHandle(right, cy);
    drawHandle(right, bottom);
    drawHandle(cx, bottom);
    drawHandle(left, bottom);
    drawHandle(left, cy);

    const wxPoint pivotDoc = GetFreeTransformPivotDocPoint();
    const int pivotX = static_cast<int>(std::round(static_cast<double>(pageOrigin.x) + static_cast<double>(pivotDoc.x) * zoom));
    const int pivotY = static_cast<int>(std::round(static_cast<double>(pageOrigin.y) + static_cast<double>(pivotDoc.y) * zoom));

    wxRect pivotRect(pivotX - half, pivotY - half, handleSize, handleSize);
    dc.SetPen(wxPen(wxColour(0, 0, 0), 1));
    dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
    dc.DrawEllipse(pivotRect);
}

DocumentCanvas::TransformHandle DocumentCanvas::HitTestFreeTransformHandle(const wxPoint& mousePos) const
{
    if (!m_owner || !m_freeTransformVisible)
        return TransformHandle::None;

    const int layerIndex = m_owner->GetSelectedLayer();
    const DocumentLayer* layer = m_owner->GetLayer(layerIndex);

    if (!layer || !layer->image.IsOk())
        return TransformHandle::None;

    const wxPoint pageOrigin = m_owner->GetPageOriginInVirtualPixels();
    const double zoom = m_owner->GetZoom();

    const int layerLeft = layer->offsetX;
    const int layerTop = layer->offsetY;
    const int layerRight = layer->offsetX + layer->image.GetWidth();
    const int layerBottom = layer->offsetY + layer->image.GetHeight();

    const int left = static_cast<int>(std::floor(static_cast<double>(pageOrigin.x) + static_cast<double>(layerLeft) * zoom));
    const int top = static_cast<int>(std::floor(static_cast<double>(pageOrigin.y) + static_cast<double>(layerTop) * zoom));
    const int right = static_cast<int>(std::ceil(static_cast<double>(pageOrigin.x) + static_cast<double>(layerRight) * zoom));
    const int bottom = static_cast<int>(std::ceil(static_cast<double>(pageOrigin.y) + static_cast<double>(layerBottom) * zoom));

    if (right <= left || bottom <= top)
        return TransformHandle::None;

    const int cx = left + (right - left) / 2;
    const int cy = top + (bottom - top) / 2;

    const int hitRadius = 8;

    auto hit = [&](int x, int y) -> bool
    {
        return std::abs(mousePos.x - x) <= hitRadius && std::abs(mousePos.y - y) <= hitRadius;
    };

    if (hit(left, top)) return TransformHandle::TopLeft;
    if (hit(cx, top)) return TransformHandle::Top;
    if (hit(right, top)) return TransformHandle::TopRight;
    if (hit(right, cy)) return TransformHandle::Right;
    if (hit(right, bottom)) return TransformHandle::BottomRight;
    if (hit(cx, bottom)) return TransformHandle::Bottom;
    if (hit(left, bottom)) return TransformHandle::BottomLeft;
    if (hit(left, cy)) return TransformHandle::Left;

    return TransformHandle::None;
}

bool DocumentCanvas::HitTestFreeTransformPivot(const wxPoint& mousePos) const
{
    if (!m_owner || !m_freeTransformVisible)
        return false;

    const wxPoint pivotDoc = GetFreeTransformPivotDocPoint();
    const wxPoint pageOrigin = m_owner->GetPageOriginInVirtualPixels();
    const double zoom = m_owner->GetZoom();

    const int pivotX = static_cast<int>(std::round(static_cast<double>(pageOrigin.x) + static_cast<double>(pivotDoc.x) * zoom));
    const int pivotY = static_cast<int>(std::round(static_cast<double>(pageOrigin.y) + static_cast<double>(pivotDoc.y) * zoom));

    const int hitRadius = 8;

    return std::abs(mousePos.x - pivotX) <= hitRadius && std::abs(mousePos.y - pivotY) <= hitRadius;
}

wxPoint DocumentCanvas::GetFreeTransformPivotDocPoint() const
{
    if (m_transformPivotCustom)
        return m_transformPivotDoc;

    const wxRect r = GetSelectedLayerDocRect();

    if (r.width <= 0 || r.height <= 0)
        return wxPoint(0, 0);

    return wxPoint(r.x + r.width / 2, r.y + r.height / 2);
}

void DocumentCanvas::SetFreeTransformPivotDocPoint(const wxPoint& docPoint)
{
    if (!m_owner)
        return;

    m_transformPivotDoc = docPoint;
    m_transformPivotCustom = true;
    Refresh(false);
}

void DocumentCanvas::StopFreeTransformPivotDrag()
{
    if (!m_transformPivotDragging)
        return;

    m_transformPivotDragging = false;

    if (HasCapture() && !m_panning && !m_movingLayer && !m_marqueeDragging && !m_lassoDragging && !m_drawingStroke && !m_transformScaling)
        ReleaseMouse();

    RestoreDocumentToolCursor(this);
    Refresh(false);
}

wxCursor DocumentCanvas::GetFreeTransformHandleCursor(TransformHandle handle) const
{
    switch (handle)
    {
        case TransformHandle::TopLeft:
        case TransformHandle::BottomRight:
            return wxCursor(wxCURSOR_SIZENWSE);

        case TransformHandle::TopRight:
        case TransformHandle::BottomLeft:
            return wxCursor(wxCURSOR_SIZENESW);

        case TransformHandle::Top:
        case TransformHandle::Bottom:
            return wxCursor(wxCURSOR_SIZENS);

        case TransformHandle::Left:
        case TransformHandle::Right:
            return wxCursor(wxCURSOR_SIZEWE);

        default:
            return wxCursor(wxCURSOR_ARROW);
    }
}

wxRect DocumentCanvas::GetSelectedLayerDocRect() const
{
    if (!m_owner)
        return wxRect();

    const int layerIndex = m_owner->GetSelectedLayer();
    const DocumentLayer* layer = m_owner->GetLayer(layerIndex);

    if (!layer || !layer->image.IsOk())
        return wxRect();

    return wxRect(layer->offsetX, layer->offsetY, layer->image.GetWidth(), layer->image.GetHeight());
}

void DocumentCanvas::ApplyFreeTransformScale(const wxPoint& mousePos)
{
    if (!m_owner || !m_transformScaling || !m_transformStartImage.IsOk())
        return;

    wxRect r = m_transformStartRect;

    if (r.width <= 0 || r.height <= 0)
        return;

    const wxPoint docPt = m_owner->ScreenToWorldPixel(mousePos);

    int left = r.x;
    int top = r.y;
    int right = r.x + r.width;
    int bottom = r.y + r.height;

    switch (m_activeTransformHandle)
    {
        case TransformHandle::TopLeft:
            left = docPt.x;
            top = docPt.y;
            break;

        case TransformHandle::Top:
            top = docPt.y;
            break;

        case TransformHandle::TopRight:
            right = docPt.x;
            top = docPt.y;
            break;

        case TransformHandle::Right:
            right = docPt.x;
            break;

        case TransformHandle::BottomRight:
            right = docPt.x;
            bottom = docPt.y;
            break;

        case TransformHandle::Bottom:
            bottom = docPt.y;
            break;

        case TransformHandle::BottomLeft:
            left = docPt.x;
            bottom = docPt.y;
            break;

        case TransformHandle::Left:
            left = docPt.x;
            break;

        default:
            return;
    }

    const bool proportional = wxGetKeyState(WXK_SHIFT);

    if (proportional)
    {
        const double aspect = static_cast<double>(r.width) / static_cast<double>(std::max(1, r.height));

        if (m_activeTransformHandle == TransformHandle::TopLeft || m_activeTransformHandle == TransformHandle::TopRight || m_activeTransformHandle == TransformHandle::BottomRight || m_activeTransformHandle == TransformHandle::BottomLeft)
        {
            int newW = std::abs(right - left);
            int newH = std::abs(bottom - top);

            if (newW <= 0)
                newW = 1;

            if (newH <= 0)
                newH = 1;

            if (static_cast<double>(newW) / static_cast<double>(newH) > aspect)
                newH = std::max(1, static_cast<int>(static_cast<double>(newW) / aspect + 0.5));
            else
                newW = std::max(1, static_cast<int>(static_cast<double>(newH) * aspect + 0.5));

            switch (m_activeTransformHandle)
            {
                case TransformHandle::TopLeft:
                    left = r.x + r.width - newW;
                    top = r.y + r.height - newH;
                    right = r.x + r.width;
                    bottom = r.y + r.height;
                    break;

                case TransformHandle::TopRight:
                    left = r.x;
                    top = r.y + r.height - newH;
                    right = r.x + newW;
                    bottom = r.y + r.height;
                    break;

                case TransformHandle::BottomRight:
                    left = r.x;
                    top = r.y;
                    right = r.x + newW;
                    bottom = r.y + newH;
                    break;

                case TransformHandle::BottomLeft:
                    left = r.x + r.width - newW;
                    top = r.y;
                    right = r.x + r.width;
                    bottom = r.y + newH;
                    break;

                default:
                    break;
            }
        }
        else if (m_activeTransformHandle == TransformHandle::Left || m_activeTransformHandle == TransformHandle::Right)
        {
            const int oldCenterY = r.y + r.height / 2;
            int newW = std::max(1, std::abs(right - left));
            int newH = std::max(1, static_cast<int>(static_cast<double>(newW) / aspect + 0.5));

            top = oldCenterY - newH / 2;
            bottom = top + newH;

            if (m_activeTransformHandle == TransformHandle::Left)
            {
                right = r.x + r.width;
                left = right - newW;
            }
            else
            {
                left = r.x;
                right = left + newW;
            }
        }
        else if (m_activeTransformHandle == TransformHandle::Top || m_activeTransformHandle == TransformHandle::Bottom)
        {
            const int oldCenterX = r.x + r.width / 2;
            int newH = std::max(1, std::abs(bottom - top));
            int newW = std::max(1, static_cast<int>(static_cast<double>(newH) * aspect + 0.5));

            left = oldCenterX - newW / 2;
            right = left + newW;

            if (m_activeTransformHandle == TransformHandle::Top)
            {
                bottom = r.y + r.height;
                top = bottom - newH;
            }
            else
            {
                top = r.y;
                bottom = top + newH;
            }
        }
    }

    if (right < left)
        std::swap(left, right);

    if (bottom < top)
        std::swap(top, bottom);

    const int newW = std::max(1, right - left);
    const int newH = std::max(1, bottom - top);

    wxRect targetRect(left, top, newW, newH);
    m_owner->TransformSelectedLayerToRect(m_transformStartImage, targetRect);

    SetCursor(GetFreeTransformHandleCursor(m_activeTransformHandle));
    Refresh(false);
}

void DocumentCanvas::StopFreeTransformScale()
{
    if (!m_transformScaling)
        return;

    m_transformScaling = false;
    m_activeTransformHandle = TransformHandle::None;
    m_transformStartRect = wxRect();
    m_transformStartImage = wxImage();

    if (m_owner)
        m_owner->EndHistoryTransaction();

    if (HasCapture() && !m_panning && !m_movingLayer && !m_marqueeDragging && !m_lassoDragging && !m_drawingStroke)
        ReleaseMouse();

    MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
    if (mainFrame)
        mainFrame->RefreshDocumentDependentUI();

    RestoreDocumentToolCursor(this);
    Refresh(false);
}

wxPoint DocumentCanvas::PenDocToScreen(const wxPoint& docPoint) const
{
    return m_owner ? m_owner->WorldToCanvasVirtual(docPoint) : wxPoint(0, 0);
}

void DocumentCanvas::DrawBezier(wxDC& dc, const wxPoint& p0, const wxPoint& c1, const wxPoint& c2, const wxPoint& p1)
{
    wxPoint prev = p0;

    for (int i = 1; i <= 32; ++i)
    {
        const double t = static_cast<double>(i) / 32.0;
        const wxPoint cur = BezierPoint(p0, c1, c2, p1, t);
        dc.DrawLine(prev, cur);
        prev = cur;
    }
}

std::vector<wxPoint> DocumentCanvas::FlattenPenPathToPolygon() const
{
    std::vector<wxPoint> polygon;

    if (m_penPoints.size() < 2)
        return polygon;

    auto appendSegment = [&](const PenPoint& a, const PenPoint& b)
    {
        const wxPoint p0 = a.anchor;
        const wxPoint p1 = b.anchor;
        const wxPoint c1 = a.hasOutHandle ? a.outHandle : a.anchor;
        const wxPoint c2 = b.hasInHandle ? b.inHandle : b.anchor;

        if (polygon.empty())
            polygon.push_back(p0);

        for (int i = 1; i <= 32; ++i)
        {
            const double t = static_cast<double>(i) / 32.0;
            const wxPoint pt = BezierPoint(p0, c1, c2, p1, t);

            if (polygon.empty() || polygon.back() != pt)
                polygon.push_back(pt);
        }
    };

    for (size_t i = 1; i < m_penPoints.size(); ++i)
        appendSegment(m_penPoints[i - 1], m_penPoints[i]);

    if (m_penClosed)
        appendSegment(m_penPoints.back(), m_penPoints.front());

    if (polygon.size() > 1 && polygon.front() == polygon.back())
        polygon.pop_back();

    return polygon;
}

DocumentCanvas::PenHit DocumentCanvas::HitTestPenObject(const wxPoint& mousePos) const
{
    PenHit hit;

    if (!m_owner || m_penPoints.empty())
        return hit;

    auto nearPoint = [&mousePos](const wxPoint& screenPt, int radius)
    {
        const int dx = mousePos.x - screenPt.x;
        const int dy = mousePos.y - screenPt.y;
        return (dx * dx + dy * dy) <= radius * radius;
    };

    for (int i = static_cast<int>(m_penPoints.size()) - 1; i >= 0; --i)
    {
        const PenPoint& p = m_penPoints[static_cast<size_t>(i)];

        if (p.hasInHandle && nearPoint(PenDocToScreen(p.inHandle), 6))
        {
            hit.type = PenHitType::InHandle;
            hit.index = i;
            return hit;
        }

        if (p.hasOutHandle && nearPoint(PenDocToScreen(p.outHandle), 6))
        {
            hit.type = PenHitType::OutHandle;
            hit.index = i;
            return hit;
        }
    }

    for (int i = static_cast<int>(m_penPoints.size()) - 1; i >= 0; --i)
    {
        const PenPoint& p = m_penPoints[static_cast<size_t>(i)];

        if (nearPoint(PenDocToScreen(p.anchor), 7))
        {
            hit.type = PenHitType::Anchor;
            hit.index = i;
            return hit;
        }
    }

    return hit;
}

bool DocumentCanvas::HitTestPenPath(const wxPoint& mousePos) const
{
    if (!m_owner || m_penPoints.empty())
        return false;

    const PenHit objectHit = HitTestPenObject(mousePos);

    if (objectHit.type != PenHitType::None)
        return true;

    std::vector<wxPoint> polygon = FlattenPenPathToPolygon();

    if (polygon.size() < 2)
        return false;

    if (m_penClosed && polygon.size() >= 3)
    {
        const wxPoint docPt = m_owner->ScreenToWorldPixel(mousePos);

        if (PointInsidePolygon(polygon, docPt.x, docPt.y))
            return true;
    }

    auto distanceToSegmentSq = [](const wxPoint& p, const wxPoint& a, const wxPoint& b) -> double
    {
        const double px = static_cast<double>(p.x);
        const double py = static_cast<double>(p.y);
        const double ax = static_cast<double>(a.x);
        const double ay = static_cast<double>(a.y);
        const double bx = static_cast<double>(b.x);
        const double by = static_cast<double>(b.y);

        const double vx = bx - ax;
        const double vy = by - ay;
        const double wx = px - ax;
        const double wy = py - ay;

        const double lenSq = vx * vx + vy * vy;

        if (lenSq <= 0.0001)
        {
            const double dx = px - ax;
            const double dy = py - ay;
            return dx * dx + dy * dy;
        }

        double t = (wx * vx + wy * vy) / lenSq;
        t = std::max(0.0, std::min(1.0, t));

        const double cx = ax + t * vx;
        const double cy = ay + t * vy;
        const double dx = px - cx;
        const double dy = py - cy;

        return dx * dx + dy * dy;
    };

    const int hitRadius = 7;
    const double hitRadiusSq = static_cast<double>(hitRadius * hitRadius);

    for (size_t i = 1; i < polygon.size(); ++i)
    {
        const wxPoint a = PenDocToScreen(polygon[i - 1]);
        const wxPoint b = PenDocToScreen(polygon[i]);

        if (distanceToSegmentSq(mousePos, a, b) <= hitRadiusSq)
            return true;
    }

    if (m_penClosed && polygon.size() > 2)
    {
        const wxPoint a = PenDocToScreen(polygon.back());
        const wxPoint b = PenDocToScreen(polygon.front());

        if (distanceToSegmentSq(mousePos, a, b) <= hitRadiusSq)
            return true;
    }

    return false;
}

bool DocumentCanvas::BeginPenEdit(const wxPoint& mousePos)
{
    if (!m_owner || m_penPoints.empty())
        return false;

    const PenHit hit = HitTestPenObject(mousePos);

    if (hit.type == PenHitType::None || hit.index < 0 || hit.index >= static_cast<int>(m_penPoints.size()))
        return false;

    PenPoint& p = m_penPoints[static_cast<size_t>(hit.index)];

    m_penEditing = true;
    m_penEditType = hit.type;
    m_penEditIndex = hit.index;
    m_penSelectedIndex = hit.index;
    m_penSelectedType = hit.type;

    m_penEditStartDoc = m_owner->ScreenToWorldPixel(mousePos);
    m_penEditAnchorStart = p.anchor;
    m_penEditInStart = p.inHandle;
    m_penEditOutStart = p.outHandle;
    m_penEditHadInHandle = p.hasInHandle;
    m_penEditHadOutHandle = p.hasOutHandle;

    if (!HasCapture())
        CaptureMouse();

    Refresh(false);
    return true;
}

void DocumentCanvas::UpdatePenEdit(const wxPoint& mousePos)
{
    if (!m_owner || !m_penEditing || m_penEditIndex < 0 || m_penEditIndex >= static_cast<int>(m_penPoints.size()))
        return;

    const wxPoint docPt = m_owner->ScreenToWorldPixel(mousePos);
    const int dx = docPt.x - m_penEditStartDoc.x;
    const int dy = docPt.y - m_penEditStartDoc.y;

    PenPoint& p = m_penPoints[static_cast<size_t>(m_penEditIndex)];

    if (m_penEditType == PenHitType::Anchor)
    {
        p.anchor = wxPoint(m_penEditAnchorStart.x + dx, m_penEditAnchorStart.y + dy);

        if (m_penEditHadInHandle)
            p.inHandle = wxPoint(m_penEditInStart.x + dx, m_penEditInStart.y + dy);

        if (m_penEditHadOutHandle)
            p.outHandle = wxPoint(m_penEditOutStart.x + dx, m_penEditOutStart.y + dy);
    }
    else if (m_penEditType == PenHitType::InHandle)
    {
        p.inHandle = docPt;
        p.hasInHandle = true;
    }
    else if (m_penEditType == PenHitType::OutHandle)
    {
        p.outHandle = docPt;
        p.hasOutHandle = true;
    }

    Refresh(false);
}

void DocumentCanvas::StopPenEdit()
{
    if (!m_penEditing)
        return;

    m_penEditing = false;
    m_penEditType = PenHitType::None;
    m_penEditIndex = -1;

    if (HasCapture() && !m_panning && !m_movingLayer && !m_marqueeDragging && !m_lassoDragging && !m_drawingStroke && !m_gradientDragging && !m_transformScaling && !m_transformPivotDragging && !m_penDraggingPoint && !m_pathDragging)
        ReleaseMouse();

    RefreshMainFrameDocumentUI(this);
    RestoreDocumentToolCursor(this);
    Refresh(false);
}

bool DocumentCanvas::BeginPathSelectionDrag(const wxPoint& mousePos)
{
    if (!m_owner || m_penPoints.empty())
        return false;

    if (!HitTestPenPath(mousePos))
    {
        m_pathSelected = false;
        m_pathDragging = false;
        m_pathDragStartDoc = wxPoint(0, 0);
        m_pathDragStartPoints.clear();

        m_penSelectedIndex = -1;
        m_penSelectedType = PenHitType::None;

        Refresh(false);
        return false;
    }

    const ToolType tool = m_owner->GetActiveTool();

    m_pathSelected = (tool == ToolType::PathSelection || tool == ToolType::DirectSelection);
    m_pathDragging = true;
    m_pathDragStartDoc = m_owner->ScreenToWorldPixel(mousePos);
    m_pathDragStartPoints = m_penPoints;

    if (m_pathSelected)
    {
        m_penSelectedIndex = -1;
        m_penSelectedType = PenHitType::None;
    }

    if (!HasCapture())
        CaptureMouse();

    Refresh(false);
    return true;
}

void DocumentCanvas::UpdatePathSelectionDrag(const wxPoint& mousePos)
{
    if (!m_owner || !m_pathDragging)
        return;

    const wxPoint docPt = m_owner->ScreenToWorldPixel(mousePos);
    const int dx = docPt.x - m_pathDragStartDoc.x;
    const int dy = docPt.y - m_pathDragStartDoc.y;

    if (m_pathDragStartPoints.size() != m_penPoints.size())
        return;

    for (size_t i = 0; i < m_penPoints.size(); ++i)
    {
        PenPoint& dst = m_penPoints[i];
        const PenPoint& src = m_pathDragStartPoints[i];

        dst.anchor = wxPoint(src.anchor.x + dx, src.anchor.y + dy);

        if (src.hasInHandle)
            dst.inHandle = wxPoint(src.inHandle.x + dx, src.inHandle.y + dy);

        if (src.hasOutHandle)
            dst.outHandle = wxPoint(src.outHandle.x + dx, src.outHandle.y + dy);

        dst.hasInHandle = src.hasInHandle;
        dst.hasOutHandle = src.hasOutHandle;
    }

    Refresh(false);
}

void DocumentCanvas::StopPathSelectionDrag()
{
    if (!m_pathDragging)
        return;

    m_pathDragging = false;
    m_pathDragStartDoc = wxPoint(0, 0);
    m_pathDragStartPoints.clear();

    if (HasCapture() && !m_panning && !m_movingLayer && !m_marqueeDragging && !m_lassoDragging && !m_drawingStroke && !m_gradientDragging && !m_transformScaling && !m_transformPivotDragging && !m_penDraggingPoint && !m_penEditing && !m_shapeDragging)
        ReleaseMouse();

    RefreshMainFrameDocumentUI(this);
    RestoreDocumentToolCursor(this);
    Refresh(false);
}

void DocumentCanvas::ShowPenContextMenu(const wxPoint& mousePos)
{
    if (!m_owner)
        return;

    if (m_penPoints.empty())
        return;

    wxMenu menu;

    wxMenuItem* makeSelectionItem = menu.Append(ID_PEN_CONTEXT_MAKE_SELECTION, "Make Selection");
    wxMenuItem* finishPathItem = menu.Append(ID_PEN_CONTEXT_FINISH_PATH, "Finish Path");
    menu.AppendSeparator();
    wxMenuItem* deletePathItem = menu.Append(ID_PEN_CONTEXT_DELETE_PATH, "Delete Path");
    wxMenuItem* strokePathItem = menu.Append(ID_PEN_CONTEXT_STROKE_PATH, "Stroke Path...");

    makeSelectionItem->Enable(m_penPoints.size() >= 3 && m_penClosed);
    finishPathItem->Enable(m_penPathActive || m_penEditing || m_penDraggingPoint);
    deletePathItem->Enable(!m_penPoints.empty());
    strokePathItem->Enable(m_penPoints.size() >= 2);

    const int selectedId = GetPopupMenuSelectionFromUser(menu, mousePos);

    if (selectedId == ID_PEN_CONTEXT_MAKE_SELECTION)
    {
        MakePenPathSelection();
        return;
    }

    if (selectedId == ID_PEN_CONTEXT_FINISH_PATH)
    {
        FinishPenPath();
        return;
    }

    if (selectedId == ID_PEN_CONTEXT_DELETE_PATH)
    {
        ClearPenPath();
        return;
    }

    if (selectedId == ID_PEN_CONTEXT_STROKE_PATH)
    {
        StrokeDialog dlg(this);

        if (dlg.ShowModal() == wxID_OK)
            StrokePenPath(dlg.GetStrokeWidth(), dlg.GetStrokeColor(), dlg.GetOpacity());

        return;
    }
}

bool DocumentCanvas::HitTestFirstPenPoint(const wxPoint& mousePos) const
{
    if (!m_owner || m_penPoints.size() < 2)
        return false;

    const wxPoint first = PenDocToScreen(m_penPoints.front().anchor);
    const int dx = mousePos.x - first.x;
    const int dy = mousePos.y - first.y;

    return (dx * dx + dy * dy) <= 64;
}

void DocumentCanvas::BeginPenPoint(const wxPoint& mousePos)
{
    if (!m_owner)
        return;

    const wxPoint docPt = m_owner->ScreenToWorldPixel(mousePos);

    if (m_penPathActive && HitTestFirstPenPoint(mousePos))
    {
        ClosePenPath();
        return;
    }

    if (!m_penPathActive && !m_penClosed && !m_penPoints.empty())
        ClearPenPath();

    if (!m_penPathActive && m_penClosed && !m_penPoints.empty())
        ClearPenPath();

    PenPoint point;
    point.anchor = docPt;
    point.inHandle = docPt;
    point.outHandle = docPt;

    m_penPoints.push_back(point);
    m_penPathActive = true;
    m_penClosed = false;
    m_penDraggingPoint = true;
    m_penDragIndex = static_cast<int>(m_penPoints.size()) - 1;
    m_penSelectedIndex = m_penDragIndex;
    m_penSelectedType = PenHitType::Anchor;
    m_penPreviewDoc = docPt;

    if (!HasCapture())
        CaptureMouse();

    RefreshMainFrameDocumentUI(this);
    Refresh(false);
}

void DocumentCanvas::UpdatePenDrag(const wxPoint& mousePos)
{
    if (!m_owner || !m_penDraggingPoint || m_penDragIndex < 0 || m_penDragIndex >= static_cast<int>(m_penPoints.size()))
        return;

    const wxPoint docPt = m_owner->ScreenToWorldPixel(mousePos);
    PenPoint& point = m_penPoints[static_cast<size_t>(m_penDragIndex)];

    const int dx = docPt.x - point.anchor.x;
    const int dy = docPt.y - point.anchor.y;

    if ((dx * dx + dy * dy) < 4)
        return;

    point.outHandle = docPt;
    point.inHandle = wxPoint(point.anchor.x - dx, point.anchor.y - dy);
    point.hasOutHandle = true;
    point.hasInHandle = true;
    m_penPreviewDoc = docPt;

    Refresh(false);
}

void DocumentCanvas::FinishPenPoint(bool keepPath)
{
    if (!m_penDraggingPoint)
        return;

    m_penDraggingPoint = false;
    m_penDragIndex = -1;

    if (HasCapture() && !m_panning && !m_movingLayer && !m_marqueeDragging && !m_lassoDragging && !m_drawingStroke && !m_gradientDragging && !m_transformScaling && !m_transformPivotDragging)
        ReleaseMouse();

    if (!keepPath)
        ClearPenPath();

    Refresh(false);
}

void DocumentCanvas::ClosePenPath()
{
    if (m_penPoints.size() < 2)
        return;

    m_penClosed = true;
    m_penPathActive = false;
    m_penDraggingPoint = false;
    m_penDragIndex = -1;

    if (HasCapture())
        ReleaseMouse();

    RefreshMainFrameDocumentUI(this);
    Refresh(false);
}

void DocumentCanvas::ClearPenPath()
{
    m_penPoints.clear();
    m_penPathActive = false;
    m_penDraggingPoint = false;
    m_penClosed = false;
    m_penEditing = false;
    m_penDragIndex = -1;
    m_penEditIndex = -1;
    m_penEditType = PenHitType::None;
    m_penSelectedIndex = -1;
    m_penSelectedType = PenHitType::None;
    m_penPreviewDoc = wxPoint(0, 0);

    m_pathSelected = false;
    m_pathDragging = false;
    m_pathDragStartDoc = wxPoint(0, 0);
    m_pathDragStartPoints.clear();

    if (HasCapture() && !m_panning && !m_movingLayer && !m_marqueeDragging && !m_lassoDragging && !m_drawingStroke && !m_gradientDragging && !m_transformScaling && !m_transformPivotDragging)
        ReleaseMouse();

    RefreshMainFrameDocumentUI(this);
    Refresh(false);
}

void DocumentCanvas::DrawPenOverlay(wxDC& dc)
{
    if (!m_owner || m_penPoints.empty())
        return;

    dc.DestroyClippingRegion();

    const wxColour lineColor(255, 255, 255);
    const wxColour lineShadow(0, 0, 0);
    const wxColour handleColor(120, 170, 255);
    const wxColour normalPointColor(255, 255, 255);
    const wxColour selectedPointColor(255, 220, 80);
    const wxColour pointBorderColor(0, 0, 0);

    const ToolType activeTool = m_owner->GetActiveTool();

    auto drawSegment = [&](const PenPoint& a, const PenPoint& b)
    {
        const wxPoint p0 = PenDocToScreen(a.anchor);
        const wxPoint p1 = PenDocToScreen(b.anchor);
        const wxPoint c1 = PenDocToScreen(a.hasOutHandle ? a.outHandle : a.anchor);
        const wxPoint c2 = PenDocToScreen(b.hasInHandle ? b.inHandle : b.anchor);

        dc.SetPen(wxPen(lineShadow, 3));
        DrawBezier(dc, p0, c1, c2, p1);

        dc.SetPen(wxPen(lineColor, 1));
        DrawBezier(dc, p0, c1, c2, p1);
    };

    for (size_t i = 1; i < m_penPoints.size(); ++i)
        drawSegment(m_penPoints[i - 1], m_penPoints[i]);

    if (m_penClosed && m_penPoints.size() > 2)
        drawSegment(m_penPoints.back(), m_penPoints.front());

    if (m_penPathActive && !m_penClosed && !m_penPoints.empty())
    {
        PenPoint preview;
        preview.anchor = m_penPreviewDoc;
        preview.inHandle = m_penPreviewDoc;
        preview.outHandle = m_penPreviewDoc;

        drawSegment(m_penPoints.back(), preview);
    }

    for (int i = 0; i < static_cast<int>(m_penPoints.size()); ++i)
    {
        const PenPoint& p = m_penPoints[static_cast<size_t>(i)];
        const wxPoint anchor = PenDocToScreen(p.anchor);

        const bool pathIsVisiblySelected = m_pathSelected || m_pathDragging;

        const bool pointIsSelected = (m_penSelectedIndex == i);
        const bool selectedAnchor = pathIsVisiblySelected || (pointIsSelected && m_penSelectedType == PenHitType::Anchor);
        const bool selectedInHandle = pointIsSelected && m_penSelectedType == PenHitType::InHandle;
        const bool selectedOutHandle = pointIsSelected && m_penSelectedType == PenHitType::OutHandle;

        const bool showPointHandles =
            pathIsVisiblySelected ||
            (activeTool == ToolType::Pen) ||
            m_penEditing ||
            pointIsSelected;

        if (showPointHandles && p.hasInHandle)
        {
            const wxPoint inH = PenDocToScreen(p.inHandle);

            dc.SetPen(wxPen(handleColor, 1));
            dc.DrawLine(anchor, inH);

            dc.SetBrush(wxBrush(selectedInHandle ? selectedPointColor : normalPointColor));
            dc.SetPen(wxPen(pointBorderColor, 1));
            dc.DrawRectangle(inH.x - 3, inH.y - 3, 6, 6);
        }

        if (showPointHandles && p.hasOutHandle)
        {
            const wxPoint outH = PenDocToScreen(p.outHandle);

            dc.SetPen(wxPen(handleColor, 1));
            dc.DrawLine(anchor, outH);

            dc.SetBrush(wxBrush(selectedOutHandle ? selectedPointColor : normalPointColor));
            dc.SetPen(wxPen(pointBorderColor, 1));
            dc.DrawRectangle(outH.x - 3, outH.y - 3, 6, 6);
        }

        dc.SetBrush(wxBrush(selectedAnchor ? selectedPointColor : normalPointColor));
        dc.SetPen(wxPen(pointBorderColor, 1));
        dc.DrawRectangle(anchor.x - 4, anchor.y - 4, 8, 8);
    }

    if (m_penPathActive && HitTestFirstPenPoint(m_lastMousePos))
    {
        const wxPoint first = PenDocToScreen(m_penPoints.front().anchor);

        dc.SetBrush(*wxTRANSPARENT_BRUSH);

        dc.SetPen(wxPen(wxColour(0, 0, 0), 3));
        dc.DrawCircle(first, 8);

        dc.SetPen(wxPen(wxColour(255, 255, 255), 1));
        dc.DrawCircle(first, 8);
    }
}

wxRect DocumentCanvas::GetShapeRect() const
{
    wxPoint end = m_shapeCurrentDoc;

    if (m_shapeConstrainSquare)
    {
        const int dx = m_shapeCurrentDoc.x - m_shapeStartDoc.x;
        const int dy = m_shapeCurrentDoc.y - m_shapeStartDoc.y;

        const int size = std::max(std::abs(dx), std::abs(dy));

        end.x = m_shapeStartDoc.x + (dx < 0 ? -size : size);
        end.y = m_shapeStartDoc.y + (dy < 0 ? -size : size);
    }

    const int x1 = std::min(m_shapeStartDoc.x, end.x);
    const int y1 = std::min(m_shapeStartDoc.y, end.y);
    const int x2 = std::max(m_shapeStartDoc.x, end.x);
    const int y2 = std::max(m_shapeStartDoc.y, end.y);

    return wxRect(x1, y1, x2 - x1, y2 - y1);
}

std::vector<unsigned char> DocumentCanvas::BuildShapeMask(const wxRect& rect, bool rounded, int radius) const
{
    std::vector<unsigned char> mask;

    if (rect.width <= 0 || rect.height <= 0)
        return mask;

    mask.resize(static_cast<size_t>(rect.width) * static_cast<size_t>(rect.height), 0);

    for (int y = 0; y < rect.height; ++y)
    {
        for (int x = 0; x < rect.width; ++x)
        {
            bool inside = true;

            if (rounded)
                inside = PointInsideRoundedRectLocal(x, y, rect.width, rect.height, radius);

            if (inside)
                mask[static_cast<size_t>(y) * static_cast<size_t>(rect.width) + static_cast<size_t>(x)] = 255;
        }
    }

    return mask;
}

std::vector<unsigned char> DocumentCanvas::BuildShapeStrokeMask(const wxRect& rect, bool rounded, int radius, int strokeWidth) const
{
    std::vector<unsigned char> mask;

    if (rect.width <= 0 || rect.height <= 0)
        return mask;

    const int sw = std::max(1, strokeWidth);

    mask.resize(static_cast<size_t>(rect.width) * static_cast<size_t>(rect.height), 0);

    for (int y = 0; y < rect.height; ++y)
    {
        for (int x = 0; x < rect.width; ++x)
        {
            bool outerInside = true;

            if (rounded)
                outerInside = PointInsideRoundedRectLocal(x, y, rect.width, rect.height, radius);

            if (!outerInside)
                continue;

            const int innerX = x - sw;
            const int innerY = y - sw;
            const int innerW = rect.width - sw * 2;
            const int innerH = rect.height - sw * 2;

            bool innerInside = false;

            if (innerW > 0 && innerH > 0)
            {
                if (rounded)
                    innerInside = PointInsideRoundedRectLocal(innerX, innerY, innerW, innerH, std::max(0, radius - sw));
                else
                    innerInside = innerX >= 0 && innerY >= 0 && innerX < innerW && innerY < innerH;
            }

            if (!innerInside)
                mask[static_cast<size_t>(y) * static_cast<size_t>(rect.width) + static_cast<size_t>(x)] = 255;
        }
    }

    return mask;
}

void DocumentCanvas::BeginShapeDrag(const wxPoint& mousePos)
{
    if (!m_owner)
        return;

    const ToolType tool = m_owner->GetActiveTool();

    if (tool != ToolType::Rectangle && tool != ToolType::RoundRect)
        return;

    if (!m_owner->CanDrawOnSelectedLayer())
        return;

    m_shapeDragging = true;
    m_shapeDragTool = tool;
    m_shapeStartDoc = m_owner->ScreenToWorldPixel(mousePos);
    m_shapeCurrentDoc = m_shapeStartDoc;
    m_shapeConstrainSquare = wxGetKeyState(WXK_SHIFT);

    if (!HasCapture())
        CaptureMouse();

    Refresh(false);
}

void DocumentCanvas::UpdateShapeDrag(const wxPoint& mousePos)
{
    if (!m_owner || !m_shapeDragging)
        return;

    m_shapeCurrentDoc = m_owner->ScreenToWorldPixel(mousePos);
    m_shapeConstrainSquare = wxGetKeyState(WXK_SHIFT);

    Refresh(false);
}

void DocumentCanvas::StopShapeDrag(bool apply)
{
    if (!m_shapeDragging)
        return;

    m_shapeConstrainSquare = wxGetKeyState(WXK_SHIFT);

    m_shapeDragging = false;

    if (HasCapture() && !m_panning && !m_movingLayer && !m_marqueeDragging && !m_lassoDragging && !m_drawingStroke && !m_gradientDragging && !m_transformScaling && !m_transformPivotDragging && !m_penDraggingPoint && !m_penEditing)
        ReleaseMouse();

    if (apply && m_owner)
    {
        wxRect rect = GetShapeRect();

        if (rect.width > 0 && rect.height > 0)
        {
            rect.x = std::max(0, rect.x);
            rect.y = std::max(0, rect.y);

            const int pageW = m_owner->GetPageWidth();
            const int pageH = m_owner->GetPageHeight();

            if (rect.x + rect.width > pageW)
                rect.width = pageW - rect.x;

            if (rect.y + rect.height > pageH)
                rect.height = pageH - rect.y;

            if (rect.width > 0 && rect.height > 0)
            {
                const bool rounded = (m_shapeDragTool == ToolType::RoundRect);
                int radius = 0;

                MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);

                if (rounded && mainFrame)
                    radius = mainFrame->GetShapeCornerRadius();

                Options::ShapeFillMode fillMode = Options::ShapeFillMode::Foreground;
                Options::ShapeStrokeMode strokeMode = Options::ShapeStrokeMode::None;
                int strokeWidth = 1;

                if (mainFrame)
                {
                    fillMode = mainFrame->GetShapeFillMode();
                    strokeMode = mainFrame->GetShapeStrokeMode();
                    strokeWidth = mainFrame->GetShapeStrokeWidth();
                }

                bool changed = false;

                if (fillMode != Options::ShapeFillMode::None)
                {
                    std::vector<unsigned char> fillMask = BuildShapeMask(rect, rounded, radius);

                    const wxColour fillColor =
                        (fillMode == Options::ShapeFillMode::Background && mainFrame)
                        ? mainFrame->GetBackgroundColor()
                        : (mainFrame ? mainFrame->GetForegroundColor() : *wxBLACK);

                    if (!fillMask.empty() && m_owner->FillSelectionOnSelectedLayer(rect, fillColor, &fillMask))
                        changed = true;
                }

                if (strokeMode != Options::ShapeStrokeMode::None)
                {
                    std::vector<unsigned char> strokeMask = BuildShapeStrokeMask(rect, rounded, radius, strokeWidth);

                    const wxColour strokeColor =
                        (strokeMode == Options::ShapeStrokeMode::Background && mainFrame)
                        ? mainFrame->GetBackgroundColor()
                        : (mainFrame ? mainFrame->GetForegroundColor() : *wxBLACK);

                    if (!strokeMask.empty() && m_owner->FillSelectionOnSelectedLayer(rect, strokeColor, &strokeMask))
                        changed = true;
                }

                if (changed && mainFrame)
                    mainFrame->RefreshDocumentDependentUI();
            }
        }
    }

    RestoreDocumentToolCursor(this);
    Refresh(false);
}

void DocumentCanvas::DrawShapeOverlay(wxDC& dc)
{
    if (!m_owner || !m_shapeDragging)
        return;

    const wxRect rect = GetShapeRect();

    if (rect.width <= 0 || rect.height <= 0)
        return;

    const wxPoint pageOrigin = m_owner->GetPageOriginInVirtualPixels();
    const double zoom = m_owner->GetZoom();

    const int left = pageOrigin.x + static_cast<int>(std::round(static_cast<double>(rect.x) * zoom));
    const int top = pageOrigin.y + static_cast<int>(std::round(static_cast<double>(rect.y) * zoom));
    const int width = std::max(1, static_cast<int>(std::round(static_cast<double>(rect.width) * zoom)));
    const int height = std::max(1, static_cast<int>(std::round(static_cast<double>(rect.height) * zoom)));

    int radius = 0;

    if (m_shapeDragTool == ToolType::RoundRect)
    {
        MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
        radius = mainFrame ? mainFrame->GetShapeCornerRadius() : 12;
    }

    const double screenRadius = std::max(2.0, static_cast<double>(radius) * zoom);

    dc.DestroyClippingRegion();
    dc.SetBrush(*wxTRANSPARENT_BRUSH);

    dc.SetPen(wxPen(wxColour(0, 0, 0), 3));

    if (m_shapeDragTool == ToolType::RoundRect)
        dc.DrawRoundedRectangle(left, top, width, height, screenRadius);
    else
        dc.DrawRectangle(left, top, width, height);

    dc.SetPen(wxPen(wxColour(255, 255, 255), 1));

    if (m_shapeDragTool == ToolType::RoundRect)
        dc.DrawRoundedRectangle(left, top, width, height, screenRadius);
    else
        dc.DrawRectangle(left, top, width, height);
}

void DocumentCanvas::InitializeGLIfNeeded()
{
    if (m_glInitialized)
        return;

    SetCurrent(*m_glContext);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    m_glInitialized = true;
}

void DocumentCanvas::EnsureGLLayerCacheSize()
{
    if (!m_owner)
        return;

    const int layerCount = m_owner->GetLayerCount();

    if (static_cast<int>(m_glLayerCaches.size()) == layerCount)
        return;

    DestroyGLTextures();

    m_glLayerCaches.clear();
    m_glLayerCaches.resize(static_cast<size_t>(layerCount));

    for (GLLayerCache& cache : m_glLayerCaches)
        cache.dirty = true;
}

void DocumentCanvas::DestroyGLTextures()
{
    if (!m_glContext)
        return;

    if (IsShownOnScreen())
        SetCurrent(*m_glContext);

    for (GLLayerCache& cache : m_glLayerCaches)
    {
        if (cache.textureId != 0)
        {
            GLuint tex = static_cast<GLuint>(cache.textureId);
            glDeleteTextures(1, &tex);
            cache.textureId = 0;
        }

        cache.width = 0;
        cache.height = 0;
        cache.dirty = true;
        cache.dirtyRects.clear();
    }
}

void DocumentCanvas::MarkAllGLLayersDirty()
{
    for (GLLayerCache& cache : m_glLayerCaches)
    {
        cache.dirty = true;
        cache.dirtyRects.clear();
    }
}

void DocumentCanvas::MarkGLLayerDirty(int layerIndex)
{
    if (layerIndex < 0)
        return;

    EnsureGLLayerCacheSize();

    if (layerIndex >= static_cast<int>(m_glLayerCaches.size()))
        return;

    GLLayerCache& cache = m_glLayerCaches[static_cast<size_t>(layerIndex)];
    cache.dirty = true;
    cache.dirtyRects.clear();
}

void DocumentCanvas::MarkGLLayerDirtyRect(int layerIndex, const wxRect& localRect)
{
    if (layerIndex < 0 || localRect.IsEmpty())
        return;

    EnsureGLLayerCacheSize();

    if (layerIndex >= static_cast<int>(m_glLayerCaches.size()))
        return;

    if (!m_owner)
        return;

    const DocumentLayer* layer = m_owner->GetLayer(layerIndex);

    if (!layer || !layer->image.IsOk())
        return;

    wxRect clipped = localRect;
    clipped.Intersect(wxRect(0, 0, layer->image.GetWidth(), layer->image.GetHeight()));

    if (clipped.IsEmpty())
        return;

    GLLayerCache& cache = m_glLayerCaches[static_cast<size_t>(layerIndex)];

    // If a full texture upload is already needed, no need to store rects.
    if (cache.dirty && cache.dirtyRects.empty())
        return;

    cache.dirty = true;

    // Keep small brush updates small.
    // Do NOT union every dab into one giant stroke bounding box.
    constexpr int kMaxDirtyRects = 256;

    if (static_cast<int>(cache.dirtyRects.size()) < kMaxDirtyRects)
    {
        cache.dirtyRects.push_back(clipped);
        return;
    }

    // If there are too many tiny rects, merge only the last rect when nearby.
    // This avoids thousands of GL calls but still avoids one giant full-stroke upload.
    wxRect& last = cache.dirtyRects.back();
    wxRect merged = last;
    merged.Union(clipped);

    const int mergedArea = merged.width * merged.height;
    const int separateArea = (last.width * last.height) + (clipped.width * clipped.height);

    if (mergedArea <= separateArea * 3)
    {
        last = merged;
        return;
    }

    // If it is far away, replace the oldest small update.
    // The next full redraw will still make the final image correct.
    cache.dirtyRects.erase(cache.dirtyRects.begin());
    cache.dirtyRects.push_back(clipped);
}

void DocumentCanvas::UploadLayerTexture(int layerIndex, const DocumentLayer& layer)
{
    if (layerIndex < 0 || layerIndex >= static_cast<int>(m_glLayerCaches.size()))
        return;

    if (!layer.image.IsOk())
        return;

    GLLayerCache& cache = m_glLayerCaches[static_cast<size_t>(layerIndex)];

    const int w = layer.image.GetWidth();
    const int h = layer.image.GetHeight();

    if (w <= 0 || h <= 0)
        return;

    if (cache.textureId == 0)
    {
        GLuint tex = 0;
        glGenTextures(1, &tex);

        cache.textureId = tex;
        cache.width = 0;
        cache.height = 0;
        cache.dirty = true;
        cache.dirtyRects.clear();
    }

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(cache.textureId));

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

    const bool sizeChanged = cache.width != w || cache.height != h;

    if (!cache.dirty && !sizeChanged)
    {
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }

    if (sizeChanged || cache.dirtyRects.empty())
    {
        std::vector<unsigned char> rgba = BuildRGBAFromImage(
            layer.image,
            m_owner ? m_owner->IsRedChannelVisible() : true,
            m_owner ? m_owner->IsGreenChannelVisible() : true,
            m_owner ? m_owner->IsBlueChannelVisible() : true
        );

        if (!rgba.empty())
        {
            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_RGBA,
                w,
                h,
                0,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                rgba.data()
            );
        }

        cache.width = w;
        cache.height = h;
        cache.dirty = false;
        cache.dirtyRects.clear();

        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }

    for (const wxRect& srcRect : cache.dirtyRects)
    {
        wxRect dirty = srcRect;
        dirty.Intersect(wxRect(0, 0, w, h));

        if (dirty.IsEmpty())
            continue;

        std::vector<unsigned char> rgba = BuildRGBAFromImageRect(layer.image, dirty);

        if (rgba.empty())
            continue;

        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            dirty.x,
            dirty.y,
            dirty.width,
            dirty.height,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            rgba.data()
        );
    }

    cache.width = w;
    cache.height = h;
    cache.dirty = false;
    cache.dirtyRects.clear();

    glBindTexture(GL_TEXTURE_2D, 0);
}

void DocumentCanvas::DrawGLQuad(double x, double y, double w, double h)
{
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2d(x,     y);
    glTexCoord2f(1.0f, 0.0f); glVertex2d(x + w, y);
    glTexCoord2f(1.0f, 1.0f); glVertex2d(x + w, y + h);
    glTexCoord2f(0.0f, 1.0f); glVertex2d(x,     y + h);
    glEnd();
}

void DocumentCanvas::DrawGLRectOutline(double x, double y, double w, double h, double r, double g, double b, double a)
{
    glDisable(GL_TEXTURE_2D);
    glColor4d(r, g, b, a);

    glBegin(GL_LINE_LOOP);
    glVertex2d(x,     y);
    glVertex2d(x + w, y);
    glVertex2d(x + w, y + h);
    glVertex2d(x,     y + h);
    glEnd();
}

void DocumentCanvas::DrawGLRoundedRectOutline(double x, double y, double w, double h, double radius, double r, double g, double b, double a, float width)
{
    if (w <= 0.0 || h <= 0.0)
        return;

    radius = std::max(0.0, std::min(radius, std::min(w, h) * 0.5));

    if (radius <= 0.0)
    {
        DrawGLRectOutline(x, y, w, h, r, g, b, a);
        return;
    }

    glDisable(GL_TEXTURE_2D);
    glLineWidth(width);
    glColor4d(r, g, b, a);

    const int arcSteps = 12;
    const double pi = 3.14159265358979323846;

    auto addArc = [&](double cx, double cy, double startAngle, double endAngle)
    {
        for (int i = 0; i <= arcSteps; ++i)
        {
            const double t = static_cast<double>(i) / static_cast<double>(arcSteps);
            const double angle = startAngle + (endAngle - startAngle) * t;
            glVertex2d(cx + std::cos(angle) * radius, cy + std::sin(angle) * radius);
        }
    };

    glBegin(GL_LINE_LOOP);

    addArc(x + w - radius, y + radius, -pi * 0.5, 0.0);
    addArc(x + w - radius, y + h - radius, 0.0, pi * 0.5);
    addArc(x + radius, y + h - radius, pi * 0.5, pi);
    addArc(x + radius, y + radius, pi, pi * 1.5);

    glEnd();

    glLineWidth(1.0f);
}

void DocumentCanvas::DrawGLLine(double x1, double y1, double x2, double y2, double r, double g, double b, double a, float width)
{
    glDisable(GL_TEXTURE_2D);
    glLineWidth(width);
    glColor4d(r, g, b, a);

    glBegin(GL_LINES);
    glVertex2d(x1, y1);
    glVertex2d(x2, y2);
    glEnd();

    glLineWidth(1.0f);
}

void DocumentCanvas::DrawGLCheckerboard(const wxRect& visibleRect, const wxPoint& pageOrigin)
{
    const int cellSize = 14;

    auto floorDiv = [](int a, int b) -> int
    {
        return a / b - (a % b != 0 && (a ^ b) < 0);
    };

    const int firstCX = floorDiv(visibleRect.x - pageOrigin.x, cellSize);
    const int firstCY = floorDiv(visibleRect.y - pageOrigin.y, cellSize);
    const int lastCX = floorDiv(visibleRect.GetRight() - 1 - pageOrigin.x, cellSize);
    const int lastCY = floorDiv(visibleRect.GetBottom() - 1 - pageOrigin.y, cellSize);

    glDisable(GL_TEXTURE_2D);

    for (int cy = firstCY; cy <= lastCY; ++cy)
    {
        const int cellTop = pageOrigin.y + cy * cellSize;
        const int y0 = std::max(cellTop, visibleRect.y);
        const int y1 = std::min(cellTop + cellSize, visibleRect.GetBottom());

        if (y0 >= y1)
            continue;

        for (int cx = firstCX; cx <= lastCX; ++cx)
        {
            const int cellLeft = pageOrigin.x + cx * cellSize;
            const int x0 = std::max(cellLeft, visibleRect.x);
            const int x1 = std::min(cellLeft + cellSize, visibleRect.GetRight());

            if (x0 >= x1)
                continue;

            const bool odd = ((cx + cy) & 1) != 0;

            if (odd)
                glColor4d(0.84, 0.84, 0.84, 1.0);
            else
                glColor4d(0.96, 0.96, 0.96, 1.0);

            glBegin(GL_QUADS);
            glVertex2d(x0, y0);
            glVertex2d(x1, y0);
            glVertex2d(x1, y1);
            glVertex2d(x0, y1);
            glEnd();
        }
    }
}

void DocumentCanvas::DrawGLOverlays()
{
    if (!m_owner)
        return;

    if (m_lassoDragging || m_lassoHasSelection)
    {
        DrawGLLassoMarchingAnts();
    }
    else if (m_marqueeDragging || m_marqueeHasSelection)
    {
        const wxRect r = GetMarqueeRect();

        if (r.width > 0 && r.height > 0)
        {
            const wxPoint p1 = m_owner->WorldToCanvasVirtual(wxPoint(r.x, r.y));
            const wxPoint p2 = m_owner->WorldToCanvasVirtual(wxPoint(r.x + r.width, r.y + r.height));

            const int x = p1.x;
            const int y = p1.y;
            const int w = p2.x - p1.x;
            const int h = p2.y - p1.y;

            if (m_marqueeElliptical)
                DrawGLEllipseMarchingAnts(x, y, w, h);
            else
                DrawGLMarchingAnts(x, y, w, h);
        }
    }

    if (m_gradientDragging)
    {
        const wxPoint a = m_owner->WorldToCanvasVirtual(m_gradientStartDoc);
        const wxPoint b = m_owner->WorldToCanvasVirtual(m_gradientCurrentDoc);

        DrawGLLine(a.x, a.y, b.x, b.y, 0.0, 0.0, 0.0, 1.0, 3.0f);
        DrawGLLine(a.x, a.y, b.x, b.y, 1.0, 1.0, 1.0, 1.0, 1.0f);
    }

    DrawGLShapeOverlay();
    DrawGLCropOverlay();
    DrawGLFreeTransformOverlay();
    DrawGLPenOverlay();
    DrawGLBrushCursor();
}

void DocumentCanvas::DrawGLBrushCursor()
{
    if (!ShouldDrawBrushCursor() || !m_owner)
        return;

    const wxPoint docPt = GetToolAnchorDocPoint(m_lastMousePos);
    const wxPoint pageOrigin = m_owner->GetPageOriginInVirtualPixels();
    const double zoom = m_owner->GetZoom();

    const int bs = std::max(1, m_brushCursorSize);
    const int half = bs / 2;

    const bool lowZoomPreview = (zoom < 4.0);
    const int dashLen = lowZoomPreview ? 2 : 4;
    const int gapLen = lowZoomPreview ? 2 : 4;
    const int patternLen = dashLen + gapLen;

    auto drawGLDashLine = [&](int x1, int y1, int x2, int y2, bool horizontal)
    {
        if (horizontal)
        {
            if (x2 < x1)
                std::swap(x1, x2);

            const int length = x2 - x1 + 1;

            if (length <= 0)
                return;

            for (int pos = 0; pos < length; pos += patternLen)
            {
                const int blackStart = pos;
                const int blackEnd = std::min(length, pos + dashLen);
                const int whiteStart = std::min(length, pos + dashLen);
                const int whiteEnd = std::min(length, pos + dashLen + dashLen);

                if (blackStart < blackEnd)
                    DrawGLLine(x1 + blackStart, y1, x1 + blackEnd - 1, y1, 0.0, 0.0, 0.0, 1.0, 1.0f);

                if (whiteStart < whiteEnd)
                    DrawGLLine(x1 + whiteStart, y1, x1 + whiteEnd - 1, y1, 1.0, 1.0, 1.0, 1.0, 1.0f);
            }

            return;
        }

        if (y2 < y1)
            std::swap(y1, y2);

        const int length = y2 - y1 + 1;

        if (length <= 0)
            return;

        for (int pos = 0; pos < length; pos += patternLen)
        {
            const int blackStart = pos;
            const int blackEnd = std::min(length, pos + dashLen);
            const int whiteStart = std::min(length, pos + dashLen);
            const int whiteEnd = std::min(length, pos + dashLen + dashLen);

            if (blackStart < blackEnd)
                DrawGLLine(x1, y1 + blackStart, x1, y1 + blackEnd - 1, 0.0, 0.0, 0.0, 1.0, 1.0f);

            if (whiteStart < whiteEnd)
                DrawGLLine(x1, y1 + whiteStart, x1, y1 + whiteEnd - 1, 1.0, 1.0, 1.0, 1.0, 1.0f);
        }
    };

    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);

    for (int by = 0; by < bs; ++by)
    {
        for (int bx = 0; bx < bs; ++bx)
        {
            if (!IsPixelInToolShape(bx, by, bs))
                continue;

            const int docPX = docPt.x - half + bx;
            const int docPY = docPt.y - half + by;

            const int left   = static_cast<int>(std::floor(static_cast<double>(pageOrigin.x) + static_cast<double>(docPX) * zoom));
            const int top    = static_cast<int>(std::floor(static_cast<double>(pageOrigin.y) + static_cast<double>(docPY) * zoom));
            const int right  = static_cast<int>(std::ceil (static_cast<double>(pageOrigin.x) + static_cast<double>(docPX + 1) * zoom)) - 1;
            const int bottom = static_cast<int>(std::ceil (static_cast<double>(pageOrigin.y) + static_cast<double>(docPY + 1) * zoom)) - 1;

            if (left > right || top > bottom)
                continue;

            const bool topEdge    = (by == 0)      || !IsPixelInToolShape(bx, by - 1, bs);
            const bool bottomEdge = (by == bs - 1) || !IsPixelInToolShape(bx, by + 1, bs);
            const bool leftEdge   = (bx == 0)      || !IsPixelInToolShape(bx - 1, by, bs);
            const bool rightEdge  = (bx == bs - 1) || !IsPixelInToolShape(bx + 1, by, bs);

            if (topEdge)
                drawGLDashLine(left - 1, top - 1, right + 1, top - 1, true);

            if (bottomEdge)
                drawGLDashLine(left - 1, bottom + 1, right + 1, bottom + 1, true);

            if (leftEdge)
                drawGLDashLine(left - 1, top - 1, left - 1, bottom + 1, false);

            if (rightEdge)
                drawGLDashLine(right + 1, top - 1, right + 1, bottom + 1, false);
        }
    }
}

void DocumentCanvas::DrawGLMarchingAntsLine(double x1, double y1, double x2, double y2)
{
    const int dashLen = 4;
    const int gapLen = 4;
    const int patternLen = dashLen + gapLen;

    const double dx = x2 - x1;
    const double dy = y2 - y1;
    const double length = std::sqrt(dx * dx + dy * dy);

    if (length <= 0.0)
        return;

    const double ux = dx / length;
    const double uy = dy / length;

    glDisable(GL_TEXTURE_2D);

    for (int pos = -m_selectionAnimOffset; pos < static_cast<int>(length); pos += patternLen)
    {
        const int blackStart = std::max(0, pos);
        const int blackEnd = std::min(static_cast<int>(length), pos + dashLen / 2);
        const int whiteStart = std::max(0, pos + dashLen / 2);
        const int whiteEnd = std::min(static_cast<int>(length), pos + dashLen);

        if (blackStart < blackEnd)
        {
            DrawGLLine(
                x1 + ux * static_cast<double>(blackStart),
                y1 + uy * static_cast<double>(blackStart),
                x1 + ux * static_cast<double>(blackEnd),
                y1 + uy * static_cast<double>(blackEnd),
                0.0, 0.0, 0.0, 1.0, 1.0f
            );
        }

        if (whiteStart < whiteEnd)
        {
            DrawGLLine(
                x1 + ux * static_cast<double>(whiteStart),
                y1 + uy * static_cast<double>(whiteStart),
                x1 + ux * static_cast<double>(whiteEnd),
                y1 + uy * static_cast<double>(whiteEnd),
                1.0, 1.0, 1.0, 1.0, 1.0f
            );
        }
    }
}

void DocumentCanvas::DrawGLMarchingAnts(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;

    const int left = x;
    const int top = y;
    const int right = x + w - 1;
    const int bottom = y + h - 1;

    DrawGLMarchingAntsLine(left, top, right, top);
    DrawGLMarchingAntsLine(right, top, right, bottom);
    DrawGLMarchingAntsLine(right, bottom, left, bottom);
    DrawGLMarchingAntsLine(left, bottom, left, top);
}

void DocumentCanvas::DrawGLEllipseMarchingAnts(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;

    const double cx = static_cast<double>(x) + static_cast<double>(w) * 0.5;
    const double cy = static_cast<double>(y) + static_cast<double>(h) * 0.5;
    const double rx = static_cast<double>(w) * 0.5;
    const double ry = static_cast<double>(h) * 0.5;

    if (rx <= 0.0 || ry <= 0.0)
        return;

    const int steps = std::max(96, static_cast<int>((rx + ry) * 1.4));
    const int dashLen = 4;
    const int gapLen = 4;
    const int patternLen = dashLen + gapLen;

    double prevX = cx + rx;
    double prevY = cy;
    double distanceAlong = 0.0;

    glDisable(GL_TEXTURE_2D);

    for (int i = 1; i <= steps; ++i)
    {
        const double t = (static_cast<double>(i) / static_cast<double>(steps)) * 6.28318530717958647692;

        const double curX = cx + std::cos(t) * rx;
        const double curY = cy + std::sin(t) * ry;

        const double dx = curX - prevX;
        const double dy = curY - prevY;
        const double segmentLen = std::sqrt(dx * dx + dy * dy);

        if (segmentLen > 0.0)
        {
            const int pattern = (static_cast<int>(distanceAlong) + m_selectionAnimOffset) % patternLen;

            if (pattern < dashLen)
                DrawGLLine(prevX, prevY, curX, curY, 0.0, 0.0, 0.0, 1.0, 1.0f);
            else
                DrawGLLine(prevX, prevY, curX, curY, 1.0, 1.0, 1.0, 1.0, 1.0f);

            distanceAlong += segmentLen;
        }

        prevX = curX;
        prevY = curY;
    }
}

void DocumentCanvas::DrawGLLassoMarchingAnts()
{
    if (!m_owner)
        return;

    if (m_lassoPoints.size() < 2)
        return;

    const wxPoint pageOrigin = m_owner->GetPageOriginInVirtualPixels();
    const double zoom = m_owner->GetZoom();

    auto toScreen = [&](const wxPoint& p) -> wxPoint
    {
        return wxPoint(
            pageOrigin.x + static_cast<int>(std::round(static_cast<double>(p.x) * zoom)),
            pageOrigin.y + static_cast<int>(std::round(static_cast<double>(p.y) * zoom))
        );
    };

    for (size_t i = 1; i < m_lassoPoints.size(); ++i)
    {
        const wxPoint a = toScreen(m_lassoPoints[i - 1]);
        const wxPoint b = toScreen(m_lassoPoints[i]);
        DrawGLMarchingAntsLine(a.x, a.y, b.x, b.y);
    }

    if (m_lassoHasSelection && m_lassoPoints.size() > 2)
    {
        const wxPoint a = toScreen(m_lassoPoints.back());
        const wxPoint b = toScreen(m_lassoPoints.front());
        DrawGLMarchingAntsLine(a.x, a.y, b.x, b.y);
    }
}

void DocumentCanvas::DrawGLPenOverlay()
{
    if (!m_owner || m_penPoints.empty())
        return;

    const ToolType activeTool = m_owner->GetActiveTool();

    auto drawBox = [&](const wxPoint& p, bool selected)
    {
        const double x = static_cast<double>(p.x - 3);
        const double y = static_cast<double>(p.y - 3);

        if (selected)
            DrawGLFilledRect(x, y, 7.0, 7.0, 255.0 / 255.0, 220.0 / 255.0, 80.0 / 255.0, 1.0);
        else
            DrawGLFilledRect(x, y, 7.0, 7.0, 1.0, 1.0, 1.0, 1.0);

        DrawGLRectOutline(x, y, 7.0, 7.0, 0.0, 0.0, 0.0, 1.0);
    };

    auto drawSegment = [&](const PenPoint& a, const PenPoint& b)
    {
        const wxPoint p0 = PenDocToScreen(a.anchor);
        const wxPoint p1 = PenDocToScreen(b.anchor);
        const wxPoint c1 = PenDocToScreen(a.hasOutHandle ? a.outHandle : a.anchor);
        const wxPoint c2 = PenDocToScreen(b.hasInHandle ? b.inHandle : b.anchor);

        DrawGLBezier(p0, c1, c2, p1, 0.0, 0.0, 0.0, 1.0, 3.0f);
        DrawGLBezier(p0, c1, c2, p1, 1.0, 1.0, 1.0, 1.0, 1.0f);
    };

    for (size_t i = 1; i < m_penPoints.size(); ++i)
        drawSegment(m_penPoints[i - 1], m_penPoints[i]);

    if (m_penClosed && m_penPoints.size() > 2)
        drawSegment(m_penPoints.back(), m_penPoints.front());

    if (m_penPathActive && !m_penClosed && !m_penPoints.empty())
    {
        PenPoint preview;
        preview.anchor = m_penPreviewDoc;
        preview.inHandle = m_penPreviewDoc;
        preview.outHandle = m_penPreviewDoc;

        drawSegment(m_penPoints.back(), preview);
    }

    const bool pathIsVisiblySelected = m_pathSelected || m_pathDragging;

    for (int i = 0; i < static_cast<int>(m_penPoints.size()); ++i)
    {
        const PenPoint& p = m_penPoints[static_cast<size_t>(i)];
        const wxPoint anchor = PenDocToScreen(p.anchor);

        const bool pointIsSelected = (m_penSelectedIndex == i);
        const bool selectedAnchor = pathIsVisiblySelected || (pointIsSelected && m_penSelectedType == PenHitType::Anchor);
        const bool selectedInHandle = pointIsSelected && m_penSelectedType == PenHitType::InHandle;
        const bool selectedOutHandle = pointIsSelected && m_penSelectedType == PenHitType::OutHandle;

        const bool showPointHandles =
            pathIsVisiblySelected ||
            activeTool == ToolType::Pen ||
            m_penEditing ||
            pointIsSelected;

        if (showPointHandles && p.hasInHandle)
        {
            const wxPoint inH = PenDocToScreen(p.inHandle);

            DrawGLLine(anchor.x, anchor.y, inH.x, inH.y, 120.0 / 255.0, 170.0 / 255.0, 1.0, 1.0, 1.0f);
            drawBox(inH, selectedInHandle);
        }

        if (showPointHandles && p.hasOutHandle)
        {
            const wxPoint outH = PenDocToScreen(p.outHandle);

            DrawGLLine(anchor.x, anchor.y, outH.x, outH.y, 120.0 / 255.0, 170.0 / 255.0, 1.0, 1.0, 1.0f);
            drawBox(outH, selectedOutHandle);
        }

        drawBox(anchor, selectedAnchor);
    }
}

void DocumentCanvas::DrawGLBezier(const wxPoint& p0, const wxPoint& c1, const wxPoint& c2, const wxPoint& p1, double r, double g, double b, double a, float width)
{
    glDisable(GL_TEXTURE_2D);
    glLineWidth(width);
    glColor4d(r, g, b, a);

    glBegin(GL_LINE_STRIP);

    for (int i = 0; i <= 32; ++i)
    {
        const double t = static_cast<double>(i) / 32.0;
        const wxPoint p = BezierPoint(p0, c1, c2, p1, t);
        glVertex2d(p.x, p.y);
    }

    glEnd();

    glLineWidth(1.0f);
}

void DocumentCanvas::DrawGLFilledRect(double x, double y, double w, double h, double r, double g, double b, double a)
{
    glDisable(GL_TEXTURE_2D);
    glColor4d(r, g, b, a);

    glBegin(GL_QUADS);
    glVertex2d(x,     y);
    glVertex2d(x + w, y);
    glVertex2d(x + w, y + h);
    glVertex2d(x,     y + h);
    glEnd();
}

void DocumentCanvas::DrawGLFreeTransformOverlay()
{
    if (!m_owner || !m_freeTransformVisible)
        return;

    const int layerIndex = m_owner->GetSelectedLayer();
    const DocumentLayer* layer = m_owner->GetLayer(layerIndex);

    if (!layer || !layer->image.IsOk())
        return;

    const wxPoint pageOrigin = m_owner->GetPageOriginInVirtualPixels();
    const double zoom = m_owner->GetZoom();

    const int layerLeft = layer->offsetX;
    const int layerTop = layer->offsetY;
    const int layerRight = layer->offsetX + layer->image.GetWidth();
    const int layerBottom = layer->offsetY + layer->image.GetHeight();

    const int left = static_cast<int>(std::floor(static_cast<double>(pageOrigin.x) + static_cast<double>(layerLeft) * zoom));
    const int top = static_cast<int>(std::floor(static_cast<double>(pageOrigin.y) + static_cast<double>(layerTop) * zoom));
    const int right = static_cast<int>(std::ceil(static_cast<double>(pageOrigin.x) + static_cast<double>(layerRight) * zoom));
    const int bottom = static_cast<int>(std::ceil(static_cast<double>(pageOrigin.y) + static_cast<double>(layerBottom) * zoom));

    if (right <= left || bottom <= top)
        return;

    const int boxW = right - left;
    const int boxH = bottom - top;

    DrawGLRectOutline(left, top, boxW, boxH, 0.0, 0.0, 0.0, 1.0);

    const int handleSize = 7;
    const int half = handleSize / 2;

    auto drawHandle = [&](int cx, int cy)
    {
        DrawGLFilledRect(cx - half, cy - half, handleSize, handleSize, 1.0, 1.0, 1.0, 1.0);
        DrawGLRectOutline(cx - half, cy - half, handleSize, handleSize, 0.0, 0.0, 0.0, 1.0);
    };

    const int cx = left + boxW / 2;
    const int cy = top + boxH / 2;

    drawHandle(left, top);
    drawHandle(cx, top);
    drawHandle(right, top);
    drawHandle(right, cy);
    drawHandle(right, bottom);
    drawHandle(cx, bottom);
    drawHandle(left, bottom);
    drawHandle(left, cy);

    const wxPoint pivotDoc = GetFreeTransformPivotDocPoint();
    const int pivotX = static_cast<int>(std::round(static_cast<double>(pageOrigin.x) + static_cast<double>(pivotDoc.x) * zoom));
    const int pivotY = static_cast<int>(std::round(static_cast<double>(pageOrigin.y) + static_cast<double>(pivotDoc.y) * zoom));

    DrawGLFilledRect(pivotX - half, pivotY - half, handleSize, handleSize, 1.0, 1.0, 1.0, 1.0);
    DrawGLEllipseOutline(pivotX - half, pivotY - half, handleSize, handleSize, 0.0, 0.0, 0.0, 1.0);
}

void DocumentCanvas::DrawGLShapeOverlay()
{
    if (!m_owner || !m_shapeDragging)
        return;

    const wxRect r = GetShapeRect();

    if (r.width <= 0 || r.height <= 0)
        return;

    const wxPoint p1 = m_owner->WorldToCanvasVirtual(wxPoint(r.x, r.y));
    const wxPoint p2 = m_owner->WorldToCanvasVirtual(wxPoint(r.x + r.width, r.y + r.height));

    const int x = p1.x;
    const int y = p1.y;
    const int w = p2.x - p1.x;
    const int h = p2.y - p1.y;

    if (w <= 0 || h <= 0)
        return;

    if (m_shapeDragTool == ToolType::RoundRect)
    {
        MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
        const int docRadius = mainFrame ? std::max(0, mainFrame->GetShapeCornerRadius()) : 12;
        const double zoom = m_owner ? m_owner->GetZoom() : 1.0;
        const double screenRadius = std::max(1.0, static_cast<double>(docRadius) * zoom);

        DrawGLRoundedRectOutline(x, y, w, h, screenRadius, 0.0, 0.0, 0.0, 1.0, 3.0f);
        DrawGLRoundedRectOutline(x, y, w, h, screenRadius, 1.0, 1.0, 1.0, 1.0, 1.0f);
        return;
    }

    DrawGLMarchingAnts(x, y, w, h);
}

void DocumentCanvas::DrawGLEllipseOutline(double x, double y, double w, double h, double r, double g, double b, double a)
{
    if (w <= 0.0 || h <= 0.0)
        return;

    const double cx = x + w * 0.5;
    const double cy = y + h * 0.5;
    const double rx = w * 0.5;
    const double ry = h * 0.5;

    glDisable(GL_TEXTURE_2D);
    glColor4d(r, g, b, a);

    glBegin(GL_LINE_LOOP);

    for (int i = 0; i < 64; ++i)
    {
        const double t = (static_cast<double>(i) / 64.0) * 6.28318530717958647692;
        glVertex2d(cx + std::cos(t) * rx, cy + std::sin(t) * ry);
    }

    glEnd();
}

void DocumentCanvas::DrawGLCropOverlay()
{
    if (!m_owner)
        return;

    if (!m_cropDragging && !m_cropHasSelection)
        return;

    const wxRect r = GetCropRect();

    if (r.width <= 0 || r.height <= 0)
        return;

    const wxPoint pageOrigin = m_owner->GetPageOriginInVirtualPixels();
    const wxSize pageSize = m_owner->GetScaledPageSize();

    const int pageLeft = pageOrigin.x;
    const int pageTop = pageOrigin.y;
    const int pageRight = pageOrigin.x + pageSize.x;
    const int pageBottom = pageOrigin.y + pageSize.y;

    const wxPoint p1 = m_owner->WorldToCanvasVirtual(wxPoint(r.x, r.y));
    const wxPoint p2 = m_owner->WorldToCanvasVirtual(wxPoint(r.x + r.width, r.y + r.height));

    int cropLeft = p1.x;
    int cropTop = p1.y;
    int cropRight = p2.x;
    int cropBottom = p2.y;

    cropLeft = std::max(pageLeft, std::min(pageRight, cropLeft));
    cropTop = std::max(pageTop, std::min(pageBottom, cropTop));
    cropRight = std::max(pageLeft, std::min(pageRight, cropRight));
    cropBottom = std::max(pageTop, std::min(pageBottom, cropBottom));

    if (cropRight <= cropLeft || cropBottom <= cropTop)
        return;

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const double dimAlpha = 0.45;

    if (cropTop > pageTop)
        DrawGLFilledRect(pageLeft, pageTop, pageRight - pageLeft, cropTop - pageTop, 0.0, 0.0, 0.0, dimAlpha);

    if (cropBottom < pageBottom)
        DrawGLFilledRect(pageLeft, cropBottom, pageRight - pageLeft, pageBottom - cropBottom, 0.0, 0.0, 0.0, dimAlpha);

    if (cropLeft > pageLeft)
        DrawGLFilledRect(pageLeft, cropTop, cropLeft - pageLeft, cropBottom - cropTop, 0.0, 0.0, 0.0, dimAlpha);

    if (cropRight < pageRight)
        DrawGLFilledRect(cropRight, cropTop, pageRight - cropRight, cropBottom - cropTop, 0.0, 0.0, 0.0, dimAlpha);

    DrawGLMarchingAnts(cropLeft, cropTop, cropRight - cropLeft, cropBottom - cropTop);

    const int hs = 8;
    const int half = hs / 2;

    const int cx = cropLeft + (cropRight - cropLeft) / 2;
    const int cy = cropTop + (cropBottom - cropTop) / 2;

    auto drawHandle = [&](int px, int py)
    {
        DrawGLFilledRect(px - half, py - half, hs, hs, 35.0 / 255.0, 35.0 / 255.0, 35.0 / 255.0, 1.0);
        DrawGLRectOutline(px - half, py - half, hs, hs, 1.0, 1.0, 1.0, 1.0);
    };

    drawHandle(cropLeft, cropTop);
    drawHandle(cx, cropTop);
    drawHandle(cropRight, cropTop);
    drawHandle(cropRight, cy);
    drawHandle(cropRight, cropBottom);
    drawHandle(cx, cropBottom);
    drawHandle(cropLeft, cropBottom);
    drawHandle(cropLeft, cy);
}

std::vector<unsigned char> DocumentCanvas::BuildRGBAFromImageRect(const wxImage& image, const wxRect& rect)
{
    std::vector<unsigned char> rgba;

    if (!image.IsOk() || !image.GetData())
        return rgba;

    const int imageW = image.GetWidth();
    const int imageH = image.GetHeight();

    wxRect clipped = rect;
    clipped.Intersect(wxRect(0, 0, imageW, imageH));

    if (clipped.IsEmpty())
        return rgba;

    const unsigned char* rgb = image.GetData();
    const unsigned char* alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;

    const bool showRed = m_owner ? m_owner->IsRedChannelVisible() : true;
    const bool showGreen = m_owner ? m_owner->IsGreenChannelVisible() : true;
    const bool showBlue = m_owner ? m_owner->IsBlueChannelVisible() : true;

    const bool anyChannelVisible = showRed || showGreen || showBlue;
    const bool singleChannelVisible =
        (showRed && !showGreen && !showBlue) ||
        (!showRed && showGreen && !showBlue) ||
        (!showRed && !showGreen && showBlue);

    const size_t outPixels = static_cast<size_t>(clipped.width) * static_cast<size_t>(clipped.height);
    rgba.resize(outPixels * 4u);

    size_t out = 0;

    for (int y = 0; y < clipped.height; ++y)
    {
        const int srcY = clipped.y + y;

        for (int x = 0; x < clipped.width; ++x)
        {
            const int srcX = clipped.x + x;
            const int srcIndex = srcY * imageW + srcX;
            const int srcRgb = srcIndex * 3;

            if (!anyChannelVisible)
            {
                rgba[out + 0] = 0;
                rgba[out + 1] = 0;
                rgba[out + 2] = 0;
                rgba[out + 3] = 0;
                out += 4;
                continue;
            }

            if (singleChannelVisible)
            {
                unsigned char value = 0;

                if (showRed)
                    value = rgb[srcRgb + 0];
                else if (showGreen)
                    value = rgb[srcRgb + 1];
                else if (showBlue)
                    value = rgb[srcRgb + 2];

                rgba[out + 0] = value;
                rgba[out + 1] = value;
                rgba[out + 2] = value;
                rgba[out + 3] = alpha ? alpha[srcIndex] : 255;
                out += 4;
                continue;
            }

            rgba[out + 0] = showRed ? rgb[srcRgb + 0] : 0;
            rgba[out + 1] = showGreen ? rgb[srcRgb + 1] : 0;
            rgba[out + 2] = showBlue ? rgb[srcRgb + 2] : 0;
            rgba[out + 3] = alpha ? alpha[srcIndex] : 255;

            out += 4;
        }
    }

    return rgba;
}

void DocumentCanvas::RenderGL(const wxRect* repaintRect)
{
    if (!m_owner || !m_glContext)
        return;

    InitializeGLIfNeeded();
    SetCurrent(*m_glContext);

    const wxSize clientSize = GetClientSize();
    const int clientW = std::max(1, clientSize.x);
    const int clientH = std::max(1, clientSize.y);

    glViewport(0, 0, clientW, clientH);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(
        0.0,
        static_cast<double>(clientW),
        static_cast<double>(clientH),
        0.0,
        -1.0,
        1.0
    );

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glClearColor(64.0f / 255.0f, 64.0f / 255.0f, 64.0f / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    EnsureGLLayerCacheSize();

    const wxPoint pageOrigin = m_owner->GetPageOriginInVirtualPixels();
    const wxSize scaledPageSize = m_owner->GetScaledPageSize();
    const wxRect pageRect(pageOrigin, scaledPageSize);
    const wxRect clientRect(wxPoint(0, 0), clientSize);

    wxRect visiblePage = pageRect;
    visiblePage.Intersect(clientRect);

    if (!visiblePage.IsEmpty())
        DrawGLCheckerboard(visiblePage, pageOrigin);

    DrawGLRectOutline(
        pageRect.x,
        pageRect.y,
        pageRect.width,
        pageRect.height,
        30.0 / 255.0,
        30.0 / 255.0,
        30.0 / 255.0,
        1.0
    );

    const double zoom = m_owner->GetZoom();
    const int layerCount = m_owner->GetLayerCount();

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (!visiblePage.IsEmpty())
    {
        glEnable(GL_SCISSOR_TEST);
        glScissor(
            visiblePage.x,
            clientH - (visiblePage.y + visiblePage.height),
            visiblePage.width,
            visiblePage.height
        );
    }

    for (int i = 0; i < layerCount; ++i)
    {
        const DocumentLayer* layer = m_owner->GetLayer(i);

        if (!layer || !layer->visible || !layer->image.IsOk())
            continue;

        const int imageW = layer->image.GetWidth();
        const int imageH = layer->image.GetHeight();

        const double drawX = static_cast<double>(pageOrigin.x) + static_cast<double>(layer->offsetX) * zoom;
        const double drawY = static_cast<double>(pageOrigin.y) + static_cast<double>(layer->offsetY) * zoom;
        const double drawW = static_cast<double>(imageW) * zoom;
        const double drawH = static_cast<double>(imageH) * zoom;

        wxRect layerScreenRect(
            static_cast<int>(std::floor(drawX)),
            static_cast<int>(std::floor(drawY)),
            std::max(1, static_cast<int>(std::ceil(drawW))),
            std::max(1, static_cast<int>(std::ceil(drawH)))
        );

        layerScreenRect.Intersect(visiblePage);

        if (layerScreenRect.IsEmpty())
            continue;

        UploadLayerTexture(i, *layer);

        if (i < 0 || i >= static_cast<int>(m_glLayerCaches.size()))
            continue;

        const GLLayerCache& cache = m_glLayerCaches[static_cast<size_t>(i)];

        if (cache.textureId == 0)
            continue;

        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(cache.textureId));

        const double opacity = static_cast<double>(layer->opacity) / 100.0;
        glColor4d(1.0, 1.0, 1.0, opacity);

        DrawGLQuad(drawX, drawY, drawW, drawH);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_SCISSOR_TEST);

    DrawGLOverlays();

    SwapBuffers();
}

void DocumentCanvas::OnPaint(wxPaintEvent&)
{
    wxPaintDC dc(this);
    RenderGL(nullptr);
}

void DocumentCanvas::OnMouseWheel(wxMouseEvent& event)
{
    if (!m_owner)
    {
        event.Skip();
        return;
    }

    if (!event.ControlDown())
    {
        event.Skip();
        return;
    }

    const int rotation = event.GetWheelRotation();
    if (rotation == 0)
        return;

    m_owner->BeginInteractiveZoom();

    if (rotation > 0)
    {
        const double newZoom = m_owner->GetZoom() * 1.25;
        m_owner->SetZoomAroundPoint(newZoom, event.GetPosition());
    }
    else
    {
        const double newZoom = m_owner->GetZoom() / 1.25;
        m_owner->SetZoomAroundPoint(newZoom, event.GetPosition());
    }

    event.Skip(false);
}

void DocumentCanvas::OnLeftDown(wxMouseEvent& event)
{
    if (!m_owner)
    {
        event.Skip();
        return;
    }

    SetFocus();

    const wxPoint oldMousePos = m_lastMousePos;
    m_mouseInside = true;
    m_lastMousePos = event.GetPosition();

    if (wxGetKeyState(WXK_SPACE) && !event.ControlDown())
    {
        if (ShouldDrawBrushCursor())
            RefreshOldAndNewBrushCursor(oldMousePos, m_lastMousePos);

        m_panning = true;
        m_panStartScreen = event.GetPosition();
        m_panStartViewOffset = m_owner->GetViewOffset();

        SetCursor(wxCursor(wxCURSOR_HAND));

        if (!HasCapture())
            CaptureMouse();

        Refresh(false);
        return;
    }

    const ToolType tool = m_owner->GetActiveTool();

    if (m_freeTransformVisible)
    {
        if (HitTestFreeTransformPivot(event.GetPosition()))
        {
            m_transformPivotDragging = true;
            SetCursor(wxCursor(wxCURSOR_SIZING));

            if (!HasCapture())
                CaptureMouse();

            Refresh(false);
            return;
        }

        const TransformHandle transformHandle = HitTestFreeTransformHandle(event.GetPosition());

        if (transformHandle != TransformHandle::None)
        {
            const wxRect layerRect = GetSelectedLayerDocRect();

            if (layerRect.width > 0 && layerRect.height > 0)
            {
                m_transformScaling = true;
                m_activeTransformHandle = transformHandle;
                m_transformStartRect = layerRect;
                m_transformStartImage = m_owner->CopySelectedLayerImage();

                m_owner->BeginHistoryTransaction("Free Transform Scale");

                SetCursor(GetFreeTransformHandleCursor(transformHandle));

                if (!HasCapture())
                    CaptureMouse();

                Refresh(false);
                return;
            }
        }
    }

    if (tool == ToolType::Stamp && IsCloneSourceModifierDown(event))
    {
        SetStampSourceAt(event.GetPosition());
        RestoreDocumentToolCursor(this);
        return;
    }

    if (tool == ToolType::Dropper)
    {
        PickDropperColorAt(event.GetPosition());

        if (!HasCapture())
            CaptureMouse();

        RestoreDocumentToolCursor(this);
        return;
    }

    const bool temporaryZoomIn = event.ControlDown() && wxGetKeyState(WXK_SPACE);
    const bool temporaryZoomOut = event.ControlDown() && event.AltDown() && wxGetKeyState(WXK_SPACE);

    if (tool == ToolType::Zoom || temporaryZoomIn || temporaryZoomOut)
    {
        MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);

        if (mainFrame)
        {
            const bool zoomOut = event.AltDown() || temporaryZoomOut;
            mainFrame->ZoomActiveDocumentAtPoint(event.GetPosition(), zoomOut ? (1.0 / 1.25) : 1.25);
        }

        RestoreDocumentToolCursor(this);
        return;
    }

    if (tool == ToolType::Hand)
    {
        if (ShouldDrawBrushCursor())
            RefreshOldAndNewBrushCursor(oldMousePos, m_lastMousePos);

        m_panning = true;
        m_panStartScreen = event.GetPosition();
        m_panStartViewOffset = m_owner->GetViewOffset();

        SetCursor(wxCursor(wxCURSOR_HAND));

        if (!HasCapture())
            CaptureMouse();

        Refresh(false);
        return;
    }

    if (tool == ToolType::Text)
    {
        StartInlineTextEdit(event.GetPosition());
        RestoreDocumentToolCursor(this);
        return;
    }

    if (tool == ToolType::PathSelection)
    {
        BeginPathSelectionDrag(event.GetPosition());
        RestoreDocumentToolCursor(this);
        return;
    }

    if (tool == ToolType::DirectSelection)
    {
        PenHit hit = HitTestPenObject(event.GetPosition());

        if (hit.type != PenHitType::None)
        {
            if (BeginPenEdit(event.GetPosition()))
            {
                RestoreDocumentToolCursor(this);
                return;
            }
        }

        if (HitTestPenPath(event.GetPosition()))
        {
            BeginPathSelectionDrag(event.GetPosition());
            RestoreDocumentToolCursor(this);
            return;
        }

        m_pathSelected = false;
        m_penSelectedIndex = -1;
        m_penSelectedType = PenHitType::None;

        RestoreDocumentToolCursor(this);
        Refresh(false);
        return;
    }

    if (tool == ToolType::Pen)
    {
        if (m_penPathActive && HitTestFirstPenPoint(event.GetPosition()))
        {
            BeginPenPoint(event.GetPosition());
            RestoreDocumentToolCursor(this);
            return;
        }

        if (BeginPenEdit(event.GetPosition()))
        {
            RestoreDocumentToolCursor(this);
            return;
        }

        BeginPenPoint(event.GetPosition());
        RestoreDocumentToolCursor(this);
        return;
    }

    if (tool == ToolType::Rectangle || tool == ToolType::RoundRect)
    {
        BeginShapeDrag(event.GetPosition());
        RestoreDocumentToolCursor(this);
        return;
    }

    if (tool == ToolType::Gradient)
    {
        if (!m_owner->CanDrawOnSelectedLayer())
        {
            RestoreDocumentToolCursor(this);
            return;
        }

        m_gradientDragging = true;
        m_gradientStartDoc = m_owner->ScreenToWorldPixel(event.GetPosition());
        m_gradientCurrentDoc = m_gradientStartDoc;

        if (!HasCapture())
            CaptureMouse();

        Refresh(false);
        return;
    }

    if (tool == ToolType::Move && m_owner->CanMoveSelectedLayer())
    {
        m_movingLayer = true;
        m_moveStartScreen = event.GetPosition();
        m_moveStartLayerOffset = m_owner->GetSelectedLayerOffset();
        m_owner->BeginMoveHistoryGesture();

        if (m_owner && m_owner->GetActiveTool() == ToolType::Move)
            m_owner->BeginHistoryTransaction("Move Layer");

        if (!HasCapture())
            CaptureMouse();

        return;
    }

    if (tool == ToolType::Crop)
    {
        const wxPoint docPt = ClampDocPointToPage(m_owner->ScreenToWorldPixel(event.GetPosition()));

        if (HasCropSelection())
        {
            const CropHandle handle = HitTestCropHandle(event.GetPosition());

            if (handle != CropHandle::None)
            {
                m_cropDragging = true;
                m_activeCropHandle = handle;
                m_cropDragStartDoc = docPt;
                m_cropDragStartRect = GetCropRect();

                SetCursor(GetCropHandleCursor(handle));

                if (!HasCapture())
                    CaptureMouse();

                Refresh(false);
                return;
            }
        }

        m_cropDragging = true;
        m_cropHasSelection = false;
        m_activeCropHandle = CropHandle::BottomRight;
        m_cropStartDoc = docPt;
        m_cropCurrentDoc = docPt;
        m_cropDragStartDoc = docPt;
        m_cropDragStartRect = wxRect();

        m_selectionAnimOffset = 0;

        if (!m_selectionAnimTimer.IsRunning())
            m_selectionAnimTimer.Start(100);

        SetCursor(GetCropHandleCursor(m_activeCropHandle));

        if (!HasCapture())
            CaptureMouse();

        Refresh(false);
        return;
    }

    if (tool == ToolType::Marquee || tool == ToolType::Elliptical)
    {
        const wxPoint docPt = m_owner->ScreenToWorld(event.GetPosition());

        m_lassoDragging = false;
        m_lassoHasSelection = false;
        m_lassoPoints.clear();

        m_marqueeDragging = true;
        m_marqueeHasSelection = false;
        m_marqueeStartDoc = docPt;
        m_marqueeCurrentDoc = docPt;
        m_marqueeConstrainSquare = event.ShiftDown() || wxGetKeyState(WXK_SHIFT);
        m_marqueeElliptical = (tool == ToolType::Elliptical);
        m_selectionAnimOffset = 0;

        if (!m_selectionAnimTimer.IsRunning())
            m_selectionAnimTimer.Start(100);

        if (!HasCapture())
            CaptureMouse();

        Refresh(false);
        return;
    }

    if (tool == ToolType::Lasso)
    {
        const wxPoint docPt = m_owner->ScreenToWorld(event.GetPosition());

        m_marqueeDragging = false;
        m_marqueeHasSelection = false;

        m_lassoDragging = true;
        m_lassoHasSelection = false;
        m_lassoPoints.clear();
        m_lassoPoints.push_back(docPt);
        m_selectionAnimOffset = 0;

        if (!m_selectionAnimTimer.IsRunning())
            m_selectionAnimTimer.Start(100);

        if (!HasCapture())
            CaptureMouse();

        Refresh(false);
        return;
    }

    if (tool == ToolType::Bucket)
    {
        const wxPoint docPt = m_owner->ScreenToWorldPixel(event.GetPosition());

        MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);

        const int tolerance = mainFrame ? mainFrame->GetBucketTolerance() : 32;
        const bool antiAlias = mainFrame ? mainFrame->GetBucketAntiAlias() : true;
        const bool contiguous = mainFrame ? mainFrame->GetBucketContiguous() : true;
        const bool allLayers = mainFrame ? mainFrame->GetBucketAllLayers() : false;

        wxRect selectionRect;
        std::vector<unsigned char> selectionMask;

        const bool hasSelection = GetSelectionMask(selectionRect, selectionMask);

        if (hasSelection)
        {
            if (!selectionRect.Contains(docPt))
            {
                RestoreDocumentToolCursor(this);
                return;
            }

            if (!selectionMask.empty())
            {
                const int maskX = docPt.x - selectionRect.x;
                const int maskY = docPt.y - selectionRect.y;
                const size_t maskIndex = static_cast<size_t>(maskY) * static_cast<size_t>(selectionRect.width) + static_cast<size_t>(maskX);

                if (maskIndex >= selectionMask.size() || !selectionMask[maskIndex])
                {
                    RestoreDocumentToolCursor(this);
                    return;
                }
            }

            const std::vector<unsigned char>* maskPtr = selectionMask.empty() ? nullptr : &selectionMask;

            if (m_owner->BucketFillSelectedLayerAt(docPt, GetActiveStrokeColor(), tolerance, antiAlias, contiguous, allLayers, &selectionRect, maskPtr))
            {
                if (mainFrame)
                    mainFrame->RefreshDocumentDependentUI();

                Refresh(false);
                return;
            }
        }
        else
        {
            if (m_owner->BucketFillSelectedLayerAt(docPt, GetActiveStrokeColor(), tolerance, antiAlias, contiguous, allLayers, nullptr, nullptr))
            {
                if (mainFrame)
                    mainFrame->RefreshDocumentDependentUI();

                Refresh(false);
                return;
            }
        }

        RestoreDocumentToolCursor(this);
        return;
    }

    if (tool == ToolType::Stamp && event.AltDown())
    {
        SetStampSourceAt(event.GetPosition());
        RestoreDocumentToolCursor(this);
        return;
    }

    if (tool == ToolType::Brush || tool == ToolType::Pencil || tool == ToolType::Eraser || tool == ToolType::Stamp)
    {
        if (BeginToolStroke(event.GetPosition()))
        {
            if (!HasCapture())
                CaptureMouse();

            Refresh(false);
            return;
        }
    }

    RestoreDocumentToolCursor(this);
    event.Skip();
}

void DocumentCanvas::OnLeftUp(wxMouseEvent& event)
{
    bool selectionStateChanged = false;
    bool drawingStateChanged = false;

    if (m_pathDragging)
    {
        StopPathSelectionDrag();
        RestoreDocumentToolCursor(this);
        Refresh(false);
        return;
    }

    if (m_penEditing)
    {
        StopPenEdit();
        RestoreDocumentToolCursor(this);
        Refresh(false);
        return;
    }

    if (m_penDraggingPoint)
    {
        FinishPenPoint(true);
        RestoreDocumentToolCursor(this);
        Refresh(false);
        return;
    }

    if (m_shapeDragging)
    {
        StopShapeDrag(true);
        RestoreDocumentToolCursor(this);
        Refresh(false);
        return;
    }

    if (m_owner && m_owner->GetActiveTool() == ToolType::Dropper)
    {
        PickDropperColorAt(event.GetPosition());

        if (HasCapture())
            ReleaseMouse();

        RestoreDocumentToolCursor(this);
        Refresh(false);
        return;
    }

    if (m_gradientDragging)
    {
        m_gradientCurrentDoc = m_owner ? m_owner->ScreenToWorldPixel(event.GetPosition()) : m_gradientCurrentDoc;
        StopGradientDrag(true);

        MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
        if (mainFrame)
            mainFrame->RefreshDocumentDependentUI();

        event.Skip();
        return;
    }

    if (m_cropDragging)
    {
        const wxPoint docPt = m_owner ? ClampDocPointToPage(m_owner->ScreenToWorldPixel(event.GetPosition())) : m_cropCurrentDoc;

        if (m_activeCropHandle == CropHandle::BottomRight && m_cropDragStartRect.IsEmpty())
            m_cropCurrentDoc = docPt;

        const wxRect finalRect = GetCropRect();
        const bool hasArea = finalRect.width > 0 && finalRect.height > 0;

        StopCropDrag(hasArea);
        selectionStateChanged = true;
    }

    if (m_marqueeDragging)
    {
        const wxPoint docPt = m_owner ? m_owner->ScreenToWorld(event.GetPosition()) : wxPoint(0, 0);
        m_marqueeCurrentDoc = docPt;
        m_marqueeConstrainSquare = event.ShiftDown() || wxGetKeyState(WXK_SHIFT);

        const wxRect finalRect = GetMarqueeRect();
        const bool hasArea = finalRect.width > 0 && finalRect.height > 0;

        if (m_marqueeConstrainSquare)
        {
            m_marqueeStartDoc = finalRect.GetTopLeft();
            m_marqueeCurrentDoc = wxPoint(finalRect.GetRight() + 1, finalRect.GetBottom() + 1);
            m_marqueeConstrainSquare = false;
        }

        StopMarqueeDrag(hasArea);
        selectionStateChanged = true;
    }

    if (m_lassoDragging)
    {
        const wxPoint docPt = m_owner ? m_owner->ScreenToWorld(event.GetPosition()) : wxPoint(0, 0);

        if (m_lassoPoints.empty() || m_lassoPoints.back() != docPt)
            m_lassoPoints.push_back(docPt);

        StopLassoDrag(m_lassoPoints.size() >= 3);
        selectionStateChanged = true;
    }

    if (m_drawingStroke)
    {
        ContinueToolStroke(event.GetPosition());
        StopDrawingStroke();
        drawingStateChanged = true;
    }

    StopPanning();
    StopFreeTransformPivotDrag();
    StopFreeTransformScale();
    StopMovingLayer();
    RestoreDocumentToolCursor(this);

    if (selectionStateChanged || drawingStateChanged)
    {
        MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);

        if (mainFrame)
        {
            if (drawingStateChanged && !selectionStateChanged)
                mainFrame->RefreshAfterBrushStroke();
            else
                mainFrame->RefreshDocumentDependentUI();
        }
    }

    Refresh(false);
    event.Skip();
}

void DocumentCanvas::OnRightDown(wxMouseEvent& event)
{
    if (!m_owner)
    {
        event.Skip();
        return;
    }

    const ToolType tool = m_owner->GetActiveTool();

    if (tool == ToolType::PathSelection && !m_penPoints.empty() && HitTestPenPath(event.GetPosition()))
    {
        m_pathSelected = true;
        ShowPenContextMenu(event.GetPosition());
        RestoreDocumentToolCursor(this);
        Refresh(false);
        return;
    }

    if (tool == ToolType::DirectSelection && !m_penPoints.empty() && HitTestPenPath(event.GetPosition()))
    {
        ShowPenContextMenu(event.GetPosition());
        RestoreDocumentToolCursor(this);
        Refresh(false);
        return;
    }

    if (tool == ToolType::Pen && !m_penPoints.empty())
    {
        StopPenEdit();
        FinishPenPoint(true);
        ShowPenContextMenu(event.GetPosition());
        RestoreDocumentToolCursor(this);
        Refresh(false);
        return;
    }

    event.Skip();
}

void DocumentCanvas::OnMouseMove(wxMouseEvent& event)
{
    if (!m_owner)
    {
        StopPanning();
        StopFreeTransformPivotDrag();
        StopFreeTransformScale();
        StopMovingLayer();
        StopMarqueeDrag(false);
        StopLassoDrag(false);
        StopDrawingStroke();
        return;
    }

    const wxPoint oldMousePos = m_lastMousePos;
    const bool oldBrushVisible = ShouldDrawBrushCursor();

    m_mouseInside = true;
    m_lastMousePos = event.GetPosition();

    if (m_pathDragging)
    {
        UpdatePathSelectionDrag(event.GetPosition());
        return;
    }

    if (m_penEditing)
    {
        UpdatePenEdit(event.GetPosition());
        return;
    }

    if (m_penDraggingPoint)
    {
        UpdatePenDrag(event.GetPosition());
        return;
    }

    if (m_shapeDragging)
    {
        UpdateShapeDrag(event.GetPosition());
        return;
    }

    if (m_owner && m_owner->GetActiveTool() == ToolType::Pen && m_penPathActive && !m_penClosed)
    {
        m_penPreviewDoc = m_owner->ScreenToWorldPixel(event.GetPosition());
        Refresh(false);
        return;
    }

    if (m_gradientDragging)
    {
        m_gradientCurrentDoc = m_owner->ScreenToWorldPixel(event.GetPosition());
        Refresh(false);
        return;
    }

    if (m_owner->GetActiveTool() == ToolType::Dropper && event.LeftIsDown())
    {
        PickDropperColorAt(event.GetPosition());
        RestoreDocumentToolCursor(this);
        return;
    }

    if (m_panning)
    {
        const wxPoint now = event.GetPosition();
        const wxPoint delta = now - m_panStartScreen;

        m_owner->SetViewOffset(m_panStartViewOffset + delta);
        SetCursor(wxCursor(wxCURSOR_HAND));
        Refresh(false);
        return;
    }

    if (m_transformPivotDragging)
    {
        SetFreeTransformPivotDocPoint(m_owner->ScreenToWorldPixel(event.GetPosition()));
        SetCursor(wxCursor(wxCURSOR_SIZING));
        return;
    }

    if (m_transformScaling)
    {
        ApplyFreeTransformScale(event.GetPosition());
        return;
    }

    if (m_movingLayer)
    {
        const wxPoint now = event.GetPosition();
        const wxPoint deltaScreen = now - m_moveStartScreen;
        const double zoom = m_owner->GetZoom();

        const int deltaX = static_cast<int>(std::lround(static_cast<double>(deltaScreen.x) / zoom));
        const int deltaY = static_cast<int>(std::lround(static_cast<double>(deltaScreen.y) / zoom));

        int newOffsetX = m_moveStartLayerOffset.x + deltaX;
        int newOffsetY = m_moveStartLayerOffset.y + deltaY;

        if (m_owner->GetSnapToDocumentBounds())
        {
            const wxPoint snapped = m_owner->GetSelectedLayerSnappedOffset(newOffsetX, newOffsetY);
            newOffsetX = snapped.x;
            newOffsetY = snapped.y;
        }

        m_owner->SetSelectedLayerOffset(newOffsetX, newOffsetY);
        Refresh(false);
        return;
    }

    if (m_cropDragging)
    {
        SetCursor(GetCropHandleCursor(m_activeCropHandle));

        const wxPoint docPt = ClampDocPointToPage(m_owner->ScreenToWorldPixel(event.GetPosition()));

        if (m_activeCropHandle == CropHandle::Inside && !m_cropDragStartRect.IsEmpty())
        {
            const int pageW = m_owner->GetPageWidth();
            const int pageH = m_owner->GetPageHeight();

            const int cropW = m_cropDragStartRect.width;
            const int cropH = m_cropDragStartRect.height;

            int left = m_cropDragStartRect.x + (docPt.x - m_cropDragStartDoc.x);
            int top = m_cropDragStartRect.y + (docPt.y - m_cropDragStartDoc.y);

            left = std::max(0, std::min(pageW - cropW, left));
            top = std::max(0, std::min(pageH - cropH, top));

            SetCropRectFromEdges(left, top, left + cropW, top + cropH);
        }
        else if (!m_cropDragStartRect.IsEmpty())
        {
            int left = m_cropDragStartRect.x;
            int top = m_cropDragStartRect.y;
            int right = m_cropDragStartRect.x + m_cropDragStartRect.width;
            int bottom = m_cropDragStartRect.y + m_cropDragStartRect.height;

            switch (m_activeCropHandle)
            {
                case CropHandle::TopLeft:
                    left = docPt.x;
                    top = docPt.y;
                    break;

                case CropHandle::Top:
                    top = docPt.y;
                    break;

                case CropHandle::TopRight:
                    right = docPt.x;
                    top = docPt.y;
                    break;

                case CropHandle::Right:
                    right = docPt.x;
                    break;

                case CropHandle::BottomRight:
                    right = docPt.x;
                    bottom = docPt.y;
                    break;

                case CropHandle::Bottom:
                    bottom = docPt.y;
                    break;

                case CropHandle::BottomLeft:
                    left = docPt.x;
                    bottom = docPt.y;
                    break;

                case CropHandle::Left:
                    left = docPt.x;
                    break;

                default:
                    m_cropCurrentDoc = docPt;
                    Refresh(false);
                    return;
            }

            SetCropRectFromEdges(left, top, right, bottom);
        }
        else
        {
            m_cropCurrentDoc = docPt;
        }

        Refresh(false);
        return;
    }

    if (m_marqueeDragging)
    {
        m_marqueeCurrentDoc = m_owner->ScreenToWorld(event.GetPosition());
        m_marqueeConstrainSquare = event.ShiftDown() || wxGetKeyState(WXK_SHIFT);
        Refresh(false);
        return;
    }

    if (m_lassoDragging)
    {
        const wxPoint docPt = m_owner->ScreenToWorld(event.GetPosition());

        if (m_lassoPoints.empty())
        {
            m_lassoPoints.push_back(docPt);
        }
        else
        {
            const wxPoint last = m_lassoPoints.back();
            const int dx = docPt.x - last.x;
            const int dy = docPt.y - last.y;

            if ((dx * dx + dy * dy) >= 4)
                m_lassoPoints.push_back(docPt);
        }

        Refresh(false);
        return;
    }

    if (m_drawingStroke)
    {
        ContinueToolStroke(event.GetPosition());
        return;
    }

    if (m_owner->GetActiveTool() == ToolType::Crop && HasCropSelection())
    {
        const CropHandle cropHandle = HitTestCropHandle(event.GetPosition());

        if (cropHandle != CropHandle::None)
        {
            SetCursor(GetCropHandleCursor(cropHandle));

            if (oldBrushVisible)
                RefreshOldAndNewBrushCursor(oldMousePos, m_lastMousePos);

            return;
        }
    }

    if (m_freeTransformVisible)
    {
        if (HitTestFreeTransformPivot(event.GetPosition()))
        {
            SetCursor(wxCursor(wxCURSOR_SIZING));

            if (oldBrushVisible)
                RefreshOldAndNewBrushCursor(oldMousePos, m_lastMousePos);

            return;
        }

        const TransformHandle transformHandle = HitTestFreeTransformHandle(event.GetPosition());

        if (transformHandle != TransformHandle::None)
        {
            SetCursor(GetFreeTransformHandleCursor(transformHandle));

            if (oldBrushVisible)
                RefreshOldAndNewBrushCursor(oldMousePos, m_lastMousePos);

            return;
        }
    }

    RestoreDocumentToolCursor(this);

    const bool newBrushVisible = ShouldDrawBrushCursor();
    if (oldBrushVisible || newBrushVisible)
    {
        RefreshOldAndNewBrushCursor(oldMousePos, m_lastMousePos);
        return;
    }

    event.Skip();
}

void DocumentCanvas::OnMouseLeave(wxMouseEvent& event)
{
    const wxPoint oldMousePos = m_lastMousePos;
    const bool wasVisible = ShouldDrawBrushCursor();

    m_mouseInside = false;

    if (wasVisible)
        RefreshBrushCursorAt(oldMousePos);

    if (!m_panning)
        RestoreDocumentToolCursor(this);

    event.Skip();
}

void DocumentCanvas::OnMouseCaptureLost(wxMouseCaptureLostEvent& event)
{
    const bool wasDrawing = m_drawingStroke;
    const bool hadLasso = m_lassoDragging || m_lassoHasSelection;

    CommitInlineTextEdit();
    StopPanning();
    StopFreeTransformPivotDrag();
    StopFreeTransformScale();
    StopMovingLayer();
    StopMarqueeDrag(m_marqueeHasSelection);
    StopLassoDrag(hadLasso && m_lassoPoints.size() >= 3);
    StopCropDrag(m_cropHasSelection);
    StopDrawingStroke();
    StopGradientDrag(false);
    StopShapeDrag(false);
    StopPathSelectionDrag();
    StopPenEdit();
    FinishPenPoint(true);

    RestoreDocumentToolCursor(this);

    if (wasDrawing)
    {
        MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
        if (mainFrame)
            mainFrame->RefreshDocumentDependentUI();
    }

    event.Skip();
}

void DocumentCanvas::OnSelectionAnimTimer(wxTimerEvent& event)
{
    const bool hasMarqueeOrLasso =
        m_marqueeDragging ||
        m_marqueeHasSelection ||
        m_lassoDragging ||
        m_lassoHasSelection;

    const bool hasCrop =
        m_cropDragging ||
        m_cropHasSelection;

    if (!hasMarqueeOrLasso && !hasCrop)
    {
        if (m_selectionAnimTimer.IsRunning())
            m_selectionAnimTimer.Stop();

        event.Skip();
        return;
    }

    m_selectionAnimOffset = (m_selectionAnimOffset + 1) % 8;
    Refresh(false);
    event.Skip();
}

void DocumentCanvas::StopPanning()
{
    if (!m_panning)
        return;

    m_panning = false;

    if (HasCapture() && !m_movingLayer && !m_marqueeDragging && !m_lassoDragging)
        ReleaseMouse();

    RestoreDocumentToolCursor(this);
    Refresh(false);
}

void DocumentCanvas::StopMovingLayer()
{
    if (!m_movingLayer)
        return;

    m_movingLayer = false;

    if (m_owner)
    {
        m_owner->EndMoveHistoryGesture();
        m_owner->EndHistoryTransaction();
    }

    MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
    if (mainFrame)
        mainFrame->RefreshDocumentDependentUI();

    if (HasCapture() && !m_panning && !m_marqueeDragging && !m_cropDragging)
        ReleaseMouse();
}

void DocumentCanvas::StopMarqueeDrag(bool keepSelection)
{
    if (!m_marqueeDragging && !m_marqueeHasSelection)
        return;

    m_marqueeDragging = false;

    if (keepSelection)
    {
        m_marqueeHasSelection = true;
        m_lassoHasSelection = false;
        m_lassoDragging = false;
        m_lassoPoints.clear();
    }
    else
    {
        m_marqueeHasSelection = false;
    }

    if (HasCapture() && !m_panning && !m_movingLayer && !m_drawingStroke && !m_lassoDragging)
        ReleaseMouse();

    m_marqueeConstrainSquare = false;

    Refresh(false);
}

void DocumentCanvas::StopLassoDrag(bool keepSelection)
{
    if (!m_lassoDragging && !m_lassoHasSelection)
        return;

    m_lassoDragging = false;

    if (keepSelection && m_lassoPoints.size() >= 3)
    {
        m_lassoHasSelection = true;
        m_marqueeDragging = false;
        m_marqueeHasSelection = false;
    }
    else
    {
        m_lassoHasSelection = false;
        m_lassoPoints.clear();
    }

    if (HasCapture() && !m_panning && !m_movingLayer && !m_drawingStroke && !m_marqueeDragging)
        ReleaseMouse();

    Refresh(false);
}

void DocumentCanvas::SelectAllMarquee()
{
    if (!m_owner)
        return;

    m_lassoDragging = false;
    m_lassoHasSelection = false;
    m_lassoPoints.clear();

    m_marqueeDragging = false;
    m_marqueeHasSelection = true;
    m_marqueeStartDoc = wxPoint(0, 0);
    m_marqueeCurrentDoc = wxPoint(m_owner->GetPageWidth(), m_owner->GetPageHeight());
    m_selectionAnimOffset = 0;

    if (!m_selectionAnimTimer.IsRunning())
        m_selectionAnimTimer.Start(100);

    Refresh(false);
}

void DocumentCanvas::ClearMarqueeSelection()
{
    const bool hadSelection = m_marqueeDragging || m_marqueeHasSelection || m_lassoDragging || m_lassoHasSelection;

    m_marqueeDragging = false;
    m_marqueeHasSelection = false;
    m_marqueeStartDoc = wxPoint(0, 0);
    m_marqueeCurrentDoc = wxPoint(0, 0);
    m_marqueeConstrainSquare = false;
    m_marqueeElliptical = false;

    m_lassoDragging = false;
    m_lassoHasSelection = false;
    m_lassoPoints.clear();

    if (m_selectionAnimTimer.IsRunning())
        m_selectionAnimTimer.Stop();

    if (hadSelection)
        RefreshMainFrameDocumentUI(this);

    Refresh(false);
}

bool DocumentCanvas::HasMarqueeSelection() const
{
    const wxRect r = GetMarqueeRect();
    return (m_marqueeHasSelection || m_lassoHasSelection) && r.width > 0 && r.height > 0;
}

wxRect DocumentCanvas::GetMarqueeRect() const
{
    if (m_lassoHasSelection)
        return GetLassoBoundingRect();

    if (!m_marqueeHasSelection && !m_marqueeDragging)
        return wxRect();

    wxPoint end = m_marqueeCurrentDoc;

    if (m_marqueeConstrainSquare && m_marqueeDragging)
    {
        const int dx = m_marqueeCurrentDoc.x - m_marqueeStartDoc.x;
        const int dy = m_marqueeCurrentDoc.y - m_marqueeStartDoc.y;
        const int size = std::max(std::abs(dx), std::abs(dy));

        end.x = m_marqueeStartDoc.x + (dx < 0 ? -size : size);
        end.y = m_marqueeStartDoc.y + (dy < 0 ? -size : size);
    }

    const int x1 = std::min(m_marqueeStartDoc.x, end.x);
    const int y1 = std::min(m_marqueeStartDoc.y, end.y);
    const int x2 = std::max(m_marqueeStartDoc.x, end.x);
    const int y2 = std::max(m_marqueeStartDoc.y, end.y);

    return wxRect(x1, y1, x2 - x1, y2 - y1);
}

bool DocumentCanvas::GetSelectionMask(wxRect& outDocRect, std::vector<unsigned char>& outMask) const
{
    outDocRect = wxRect();
    outMask.clear();

    if (m_lassoHasSelection && m_lassoPoints.size() >= 3)
    {
        wxRect bounds = GetLassoBoundingRect();

        if (bounds.width <= 0 || bounds.height <= 0)
            return false;

        outDocRect = bounds;
        outMask.assign(static_cast<size_t>(bounds.width) * static_cast<size_t>(bounds.height), 0);

        for (int y = 0; y < bounds.height; ++y)
        {
            for (int x = 0; x < bounds.width; ++x)
            {
                const int docX = bounds.x + x;
                const int docY = bounds.y + y;

                if (PointInsidePolygon(m_lassoPoints, docX, docY))
                    outMask[static_cast<size_t>(y) * static_cast<size_t>(bounds.width) + static_cast<size_t>(x)] = 255;
            }
        }

        return true;
    }

    if (!m_marqueeHasSelection)
        return false;

    const wxRect rect = GetMarqueeRect();

    if (rect.width <= 0 || rect.height <= 0)
        return false;

    outDocRect = rect;

    if (!m_marqueeElliptical)
        return true;

    outMask.assign(static_cast<size_t>(rect.width) * static_cast<size_t>(rect.height), 0);

    for (int y = 0; y < rect.height; ++y)
    {
        for (int x = 0; x < rect.width; ++x)
        {
            if (PointInsideEllipseLocal(x, y, rect.width, rect.height))
                outMask[static_cast<size_t>(y) * static_cast<size_t>(rect.width) + static_cast<size_t>(x)] = 255;
        }
    }

    return true;
}

wxRect DocumentCanvas::GetLassoBoundingRect() const
{
    if (m_lassoPoints.empty())
        return wxRect();

    int minX = m_lassoPoints[0].x;
    int minY = m_lassoPoints[0].y;
    int maxX = m_lassoPoints[0].x;
    int maxY = m_lassoPoints[0].y;

    for (const wxPoint& p : m_lassoPoints)
    {
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }

    return wxRect(minX, minY, std::max(1, maxX - minX), std::max(1, maxY - minY));
}








