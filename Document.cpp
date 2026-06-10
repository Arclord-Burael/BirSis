#include "Document.h"
#include "DocumentCanvas.h"
#include "Ruler.h"
#include "StatusBar.h"

#include <algorithm>
#include <cstring>
#include <vector>
#include <cmath>
#include <fstream>
#include <cstdint>
#include <limits>
#include <wx/frame.h>
#include <wx/sizer.h>
#include <wx/image.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/bitmap.h>

namespace
{
    bool ImagesEqual(const wxImage& a, const wxImage& b)
    {
        if (!a.IsOk() && !b.IsOk())
            return true;

        if (a.IsOk() != b.IsOk())
            return false;

        if (a.GetWidth() != b.GetWidth() || a.GetHeight() != b.GetHeight())
            return false;

        const size_t rgbBytes = static_cast<size_t>(a.GetWidth()) * static_cast<size_t>(a.GetHeight()) * 3u;
        const unsigned char* aData = a.GetData();
        const unsigned char* bData = b.GetData();
        if ((aData == nullptr) != (bData == nullptr))
            return false;
        if (aData && std::memcmp(aData, bData, rgbBytes) != 0)
            return false;

        const bool aHasAlpha = a.HasAlpha();
        const bool bHasAlpha = b.HasAlpha();
        if (aHasAlpha != bHasAlpha)
            return false;

        if (aHasAlpha)
        {
            const size_t alphaBytes = static_cast<size_t>(a.GetWidth()) * static_cast<size_t>(a.GetHeight());
            const unsigned char* aAlpha = a.GetAlpha();
            const unsigned char* bAlpha = b.GetAlpha();
            if ((aAlpha == nullptr) != (bAlpha == nullptr))
                return false;
            if (aAlpha && std::memcmp(aAlpha, bAlpha, alphaBytes) != 0)
                return false;
        }

        return true;
    }

    void EnsureAlpha(wxImage& image)
    {
        if (!image.IsOk())
            return;

        if (!image.HasAlpha())
            image.InitAlpha();
    }

    bool ApplyPixel(wxImage& image, int x, int y, const wxColour& color, bool erase)
    {
        if (!image.IsOk())
            return false;

        const int w = image.GetWidth();
        const int h = image.GetHeight();
        if (x < 0 || y < 0 || x >= w || y >= h)
            return false;

        unsigned char* data = image.GetData();
        if (!data)
            return false;

        EnsureAlpha(image);
        unsigned char* alpha = image.GetAlpha();
        if (!alpha)
            return false;

        const int idx = (y * w + x);
        const int rgb = idx * 3;

        if (erase)
        {
            if (alpha[idx] == 0)
                return false;

            alpha[idx] = 0;
            return true;
        }

        const unsigned char r = static_cast<unsigned char>(color.Red());
        const unsigned char g = static_cast<unsigned char>(color.Green());
        const unsigned char b = static_cast<unsigned char>(color.Blue());

        const bool same = data[rgb + 0] == r && data[rgb + 1] == g && data[rgb + 2] == b && alpha[idx] == 255;

        if (same)
            return false;

        data[rgb + 0] = r;
        data[rgb + 1] = g;
        data[rgb + 2] = b;
        alpha[idx] = 255;
        return true;
    }

    constexpr uint32_t ASB_MAGIC = 0x21425341;
    constexpr uint32_t ASB_LAYER_MAGIC = 0x2152594C;
    constexpr uint16_t ASB_VERSION = 1;

    #pragma pack(push, 1)
    struct ASBHeader
    {
        uint32_t magic;
        uint16_t version;
        uint32_t width;
        uint32_t height;
        uint32_t layerCount;
        uint64_t reserved;
    };

    struct ASBLayerHeader
    {
        uint32_t magic;
        uint32_t width;
        uint32_t height;
        int32_t offsetX;
        int32_t offsetY;
        uint8_t visible;
        uint8_t locked;
        uint16_t nameLength;
        uint32_t rgbaByteCount;
        uint64_t reserved;
    };
    #pragma pack(pop)

    template <typename T>
    bool WriteBinary(std::ofstream& out, const T& value)
    {
        out.write(reinterpret_cast<const char*>(&value), sizeof(T));
        return out.good();
    }

    template <typename T>
    bool ReadBinary(std::ifstream& in, T& value)
    {
        in.read(reinterpret_cast<char*>(&value), sizeof(T));
        return in.good();
    }
}

wxBEGIN_EVENT_TABLE(Document, wxPanel)
    EVT_SIZE(Document::OnSize)
wxEND_EVENT_TABLE()

Document::Document(wxWindow* parent, int pageWidth, int pageHeight, int contentMode)
: wxPanel(parent, wxID_ANY)
, m_pageWidth(pageWidth)
, m_pageHeight(pageHeight)
, m_contentMode(contentMode)
, m_zoomSettleTimer(this, wxID_ANY)
{
    SetBackgroundColour(wxColour(64, 64, 64));
    BuildUI();

    Bind(wxEVT_TIMER, &Document::OnZoomSettleTimer, this, m_zoomSettleTimer.GetId());

    CallAfter([this]()
    {
        UpdateViewCenter();
        SyncRulers();
        UpdateZoomStatusText();
        RequestCanvasRefresh();
    });
}

void Document::BuildUI()
{
    m_cornerPanel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(20, 20));
    m_cornerPanel->SetBackgroundColour(wxColour(45, 45, 45));

    m_hRuler = new Ruler(this, RulerOrientation::Horizontal);
    m_vRuler = new Ruler(this, RulerOrientation::Vertical);
    m_canvas = new DocumentCanvas(this, this);
    m_canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_canvas->SetBackgroundColour(wxColour(64, 64, 64));

    wxFlexGridSizer* rootSizer = new wxFlexGridSizer(2, 2, 0, 0);
    rootSizer->Add(m_cornerPanel, 0, wxEXPAND);
    rootSizer->Add(m_hRuler, 1, wxEXPAND);
    rootSizer->Add(m_vRuler, 1, wxEXPAND);
    rootSizer->Add(m_canvas, 1, wxEXPAND);
    rootSizer->AddGrowableCol(1, 1);
    rootSizer->AddGrowableRow(1, 1);
    SetSizer(rootSizer);
}

void Document::ResetBlankDocument(const wxString& layerName)
{
    m_layers.clear();

    DocumentLayer layer;
    layer.name = layerName;
    layer.visible = true;
    layer.locked = false;
    layer.offsetX = 0;
    layer.offsetY = 0;
    layer.image = wxImage(m_pageWidth, m_pageHeight, true);
    layer.image.SetRGB(wxRect(0, 0, m_pageWidth, m_pageHeight), 255, 255, 255);

    if (!layer.image.HasAlpha())
        layer.image.InitAlpha();

    unsigned char* alpha = layer.image.GetAlpha();
    if (alpha)
    {
        const unsigned char alphaValue = (m_contentMode == 2) ? 0 : 255;
        std::fill(alpha, alpha + (m_pageWidth * m_pageHeight), alphaValue);
    }

    m_layers.push_back(layer);
    m_selectedLayer = 0;

    ClearHistory();
    MarkDisplayCacheDirty();
    RebuildOriginalBitmapCache();
    RebuildPreviewCache();
    MarkClean();
    NotifyDocumentChanged();
}

bool Document::LoadImageAsSingleLayer(const wxImage& image, const wxString& layerName)
{
    if (!image.IsOk())
        return false;

    m_layers.clear();

    DocumentLayer layer;
    layer.name = layerName;
    layer.visible = true;
    layer.locked = false;
    layer.offsetX = 0;
    layer.offsetY = 0;
    layer.image = image;

    m_layers.push_back(layer);
    m_selectedLayer = 0;

    ClearHistory();
    MarkDisplayCacheDirty();
    RebuildOriginalBitmapCache();
    RebuildPreviewCache();
    MarkClean();
    NotifyDocumentChanged();
    return true;
}

bool Document::DuplicateFromDocument(const Document& source)
{
    m_pageWidth = source.m_pageWidth;
    m_pageHeight = source.m_pageHeight;
    m_contentMode = source.m_contentMode;
    m_resolution = source.m_resolution;

    m_layers.clear();
    m_layers.reserve(source.m_layers.size());

    for (const DocumentLayer& srcLayer : source.m_layers)
    {
        DocumentLayer dstLayer;
        dstLayer.name = srcLayer.name;
        dstLayer.visible = srcLayer.visible;
        dstLayer.locked = srcLayer.locked;
        dstLayer.offsetX = srcLayer.offsetX;
        dstLayer.offsetY = srcLayer.offsetY;

        if (srcLayer.image.IsOk())
            dstLayer.image = srcLayer.image.Copy();

        m_layers.push_back(dstLayer);
    }

    m_selectedLayer = source.m_selectedLayer;

    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        m_selectedLayer = m_layers.empty() ? -1 : 0;

    ClearHistory();
    MarkDisplayCacheDirty();
    RebuildOriginalBitmapCache();
    RebuildPreviewCache();
    SyncRulers();
    MarkClean();
    NotifyDocumentChanged();

    return !m_layers.empty();
}

bool Document::SaveASB(const wxString& path) const
{
    if (path.IsEmpty() || m_pageWidth <= 0 || m_pageHeight <= 0)
        return false;

    std::ofstream out(path.ToStdString(), std::ios::binary);
    if (!out.is_open())
        return false;

    ASBHeader header{};
    header.magic = ASB_MAGIC;
    header.version = ASB_VERSION;
    header.width = static_cast<uint32_t>(m_pageWidth);
    header.height = static_cast<uint32_t>(m_pageHeight);
    header.layerCount = static_cast<uint32_t>(m_layers.size());
    header.reserved = static_cast<uint64_t>(std::max(1, static_cast<int>(std::lround(m_resolution))));

    if (!WriteBinary(out, header))
        return false;

    for (const DocumentLayer& layer : m_layers)
    {
        if (!layer.image.IsOk())
            return false;

        wxImage image = layer.image.Copy();

        if (!image.HasAlpha())
            image.InitAlpha();

        const int w = image.GetWidth();
        const int h = image.GetHeight();

        if (w <= 0 || h <= 0)
            return false;

        const uint64_t pixelCount64 = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
        const uint64_t rgbaBytes64 = pixelCount64 * 4ull;

        if (rgbaBytes64 > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
            return false;

        wxCharBuffer nameBuffer = layer.name.ToUTF8();
        const size_t nameLen = std::strlen(nameBuffer.data());

        if (nameLen > static_cast<size_t>(std::numeric_limits<uint16_t>::max()))
            return false;

        ASBLayerHeader layerHeader{};
        layerHeader.magic = ASB_LAYER_MAGIC;
        layerHeader.width = static_cast<uint32_t>(w);
        layerHeader.height = static_cast<uint32_t>(h);
        layerHeader.offsetX = static_cast<int32_t>(layer.offsetX);
        layerHeader.offsetY = static_cast<int32_t>(layer.offsetY);
        layerHeader.visible = layer.visible ? 1 : 0;
        layerHeader.locked = layer.locked ? 1 : 0;
        layerHeader.nameLength = static_cast<uint16_t>(nameLen);
        layerHeader.rgbaByteCount = static_cast<uint32_t>(rgbaBytes64);
        layerHeader.reserved = 0;

        if (!WriteBinary(out, layerHeader))
            return false;

        if (nameLen > 0)
        {
            out.write(nameBuffer.data(), static_cast<std::streamsize>(nameLen));
            if (!out.good())
                return false;
        }

        const unsigned char* rgb = image.GetData();
        const unsigned char* alpha = image.GetAlpha();

        if (!rgb || !alpha)
            return false;

        std::vector<unsigned char> rgba;
        rgba.resize(static_cast<size_t>(rgbaBytes64));

        for (uint64_t i = 0; i < pixelCount64; ++i)
        {
            const uint64_t rgbIndex = i * 3ull;
            const uint64_t rgbaIndex = i * 4ull;

            rgba[static_cast<size_t>(rgbaIndex + 0)] = rgb[static_cast<size_t>(rgbIndex + 0)];
            rgba[static_cast<size_t>(rgbaIndex + 1)] = rgb[static_cast<size_t>(rgbIndex + 1)];
            rgba[static_cast<size_t>(rgbaIndex + 2)] = rgb[static_cast<size_t>(rgbIndex + 2)];
            rgba[static_cast<size_t>(rgbaIndex + 3)] = alpha[static_cast<size_t>(i)];
        }

        out.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));

        if (!out.good())
            return false;
    }

    return true;
}

bool Document::LoadASB(const wxString& path)
{
    if (path.IsEmpty())
        return false;

    std::ifstream in(path.ToStdString(), std::ios::binary);
    if (!in.is_open())
        return false;

    ASBHeader header{};
    if (!ReadBinary(in, header))
        return false;

    if (header.magic != ASB_MAGIC || header.version != ASB_VERSION)
        return false;

    if (header.width == 0 || header.height == 0 || header.layerCount == 0)
        return false;

    if (header.width > 100000 || header.height > 100000 || header.layerCount > 10000)
        return false;

    std::vector<DocumentLayer> loadedLayers;
    loadedLayers.reserve(header.layerCount);

    for (uint32_t layerIndex = 0; layerIndex < header.layerCount; ++layerIndex)
    {
        ASBLayerHeader layerHeader{};
        if (!ReadBinary(in, layerHeader))
            return false;

        if (layerHeader.magic != ASB_LAYER_MAGIC)
            return false;

        if (layerHeader.width == 0 || layerHeader.height == 0)
            return false;

        const uint64_t pixelCount64 = static_cast<uint64_t>(layerHeader.width) * static_cast<uint64_t>(layerHeader.height);
        const uint64_t expectedRgbaBytes64 = pixelCount64 * 4ull;

        if (layerHeader.rgbaByteCount != expectedRgbaBytes64)
            return false;

        std::string utf8Name;
        if (layerHeader.nameLength > 0)
        {
            utf8Name.resize(layerHeader.nameLength);
            in.read(&utf8Name[0], static_cast<std::streamsize>(utf8Name.size()));

            if (!in.good())
                return false;
        }

        std::vector<unsigned char> rgba;
        rgba.resize(static_cast<size_t>(expectedRgbaBytes64));

        in.read(reinterpret_cast<char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
        if (!in.good())
            return false;

        unsigned char* rgb = new unsigned char[static_cast<size_t>(pixelCount64) * 3u];
        unsigned char* alpha = new unsigned char[static_cast<size_t>(pixelCount64)];

        for (uint64_t i = 0; i < pixelCount64; ++i)
        {
            const uint64_t rgbIndex = i * 3ull;
            const uint64_t rgbaIndex = i * 4ull;

            rgb[static_cast<size_t>(rgbIndex + 0)] = rgba[static_cast<size_t>(rgbaIndex + 0)];
            rgb[static_cast<size_t>(rgbIndex + 1)] = rgba[static_cast<size_t>(rgbaIndex + 1)];
            rgb[static_cast<size_t>(rgbIndex + 2)] = rgba[static_cast<size_t>(rgbaIndex + 2)];
            alpha[static_cast<size_t>(i)] = rgba[static_cast<size_t>(rgbaIndex + 3)];
        }

        wxImage image(static_cast<int>(layerHeader.width), static_cast<int>(layerHeader.height));
        image.SetData(rgb);
        image.SetAlpha(alpha);

        DocumentLayer layer;
        layer.name = utf8Name.empty() ? wxString::Format("Layer %u", layerIndex + 1) : wxString::FromUTF8(utf8Name.c_str());
        layer.image = image;
        layer.visible = layerHeader.visible != 0;
        layer.locked = layerHeader.locked != 0;
        layer.offsetX = static_cast<int>(layerHeader.offsetX);
        layer.offsetY = static_cast<int>(layerHeader.offsetY);

        loadedLayers.push_back(layer);
    }

    m_pageWidth = static_cast<int>(header.width);
    m_pageHeight = static_cast<int>(header.height);
    m_resolution = header.reserved > 0 ? static_cast<double>(header.reserved) : 72.0;
    m_layers = loadedLayers;
    m_selectedLayer = m_layers.empty() ? -1 : 0;

    const int resolutionInt = std::max(1, static_cast<int>(std::lround(m_resolution)));

    for (DocumentLayer& layer : m_layers)
    {
        if (!layer.image.IsOk())
            continue;

        layer.image.SetOption(wxIMAGE_OPTION_RESOLUTION, resolutionInt);
        layer.image.SetOption(wxIMAGE_OPTION_RESOLUTIONX, resolutionInt);
        layer.image.SetOption(wxIMAGE_OPTION_RESOLUTIONY, resolutionInt);
        layer.image.SetOption(wxIMAGE_OPTION_RESOLUTIONUNIT, wxIMAGE_RESOLUTION_INCHES);
    }

    ClearHistory();
    MarkDisplayCacheDirty();
    UpdateViewCenter();
    SyncRulers();
    MarkClean();
    NotifyDocumentChanged();

    return true;
}

const DocumentLayer* Document::GetLayer(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return nullptr;

    return &m_layers[index];
}

const std::vector<DocumentLayer>& Document::GetLayers() const { return m_layers; }

int Document::GetLayerCount() const
{
    return static_cast<int>(m_layers.size());
}

int Document::GetSelectedLayer() const { return m_selectedLayer; }

void Document::SetSelectedLayer(int index)
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return;

    m_selectedLayer = index;
    RequestCanvasRefresh();
}

int Document::GetLayerOpacity(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return 100;

    return m_layers[index].opacity;
}

void Document::SetLayerOpacity(int index, int opacity)
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return;

    opacity = std::clamp(opacity, 0, 100);

    if (m_layers[index].opacity == opacity)
        return;

    BeginHistoryTransaction("Layer Opacity");

    m_layers[index].opacity = opacity;

    MarkDisplayCacheDirty();
    RequestCanvasRefresh();

    EndHistoryTransaction();
}

void Document::SetLayerOpacityInternal(int index, int opacity)
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return;

    opacity = std::clamp(opacity, 0, 100);

    if (m_layers[index].opacity == opacity)
        return;

    m_layers[index].opacity = opacity;

    MarkDisplayCacheDirty();
    RequestCanvasRefresh();
}

int Document::GetPageWidth() const { return m_pageWidth; }
int Document::GetPageHeight() const { return m_pageHeight; }
int Document::GetContentMode() const { return m_contentMode; }
double Document::GetZoom() const { return m_zoom; }

void Document::SetZoom(double zoom)
{
    if (!m_canvas)
        return;

    const wxSize clientSize = m_canvas->GetClientSize();
    SetZoomAroundPoint(zoom, wxPoint(clientSize.x / 2, clientSize.y / 2));
}

void Document::SetZoomAroundPoint(double zoom, const wxPoint& focusClientPoint)
{
    const double newZoom = std::clamp(zoom, k_zoomMin, k_zoomMax);
    if (std::fabs(newZoom - m_zoom) < 1e-9)
    {
        UpdateZoomStatusText();
        return;
    }

    const double worldX = (static_cast<double>(focusClientPoint.x) - static_cast<double>(m_viewOffset.x)) / m_zoom;
    const double worldY = (static_cast<double>(focusClientPoint.y) - static_cast<double>(m_viewOffset.y)) / m_zoom;

    m_zoom = newZoom;
    InvalidateDisplayCache();

    m_viewOffset = wxPoint(
        static_cast<int>(std::lround(static_cast<double>(focusClientPoint.x) - worldX * m_zoom)),
        static_cast<int>(std::lround(static_cast<double>(focusClientPoint.y) - worldY * m_zoom))
    );

    SyncRulers();
    UpdateZoomStatusText();
    RequestCanvasRefresh();
}

void Document::ZoomIn()
{
    // Use the same 1.25× step as the mouse wheel so keyboard/button zoom
    // feels equally snappy and reaches target levels in fewer presses.
    SetZoom(m_zoom * 1.25);
}

void Document::ZoomOut()
{
    SetZoom(m_zoom / 1.25);
}

void Document::FitInWindow()
{

}

void Document::Zoom100()
{
    SetZoom(1.0);
}

wxPoint Document::GetPageOriginInVirtualPixels() const
{
    return m_viewOffset;
}

wxSize Document::GetScaledPageSize() const
{
    const int scaledW = std::max(1, static_cast<int>(std::lround(static_cast<double>(m_pageWidth) * m_zoom)));
    const int scaledH = std::max(1, static_cast<int>(std::lround(static_cast<double>(m_pageHeight) * m_zoom)));
    return wxSize(scaledW, scaledH);
}

const wxBitmap* Document::GetCachedLayerBitmap(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_cachedLayerBitmaps.size()))
        return nullptr;

    return &m_cachedLayerBitmaps[index];
}

const wxBitmap* Document::GetPreviewLayerBitmap(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_previewLayerBitmaps.size()))
        return nullptr;

    return &m_previewLayerBitmaps[index];
}

const wxBitmap* Document::GetOriginalLayerBitmap(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_originalLayerBitmaps.size()))
        return nullptr;

    return &m_originalLayerBitmaps[index];
}

void Document::EnsureDisplayCache()
{
    // During interactive zoom, OnPaint uses StretchBlit from the preview
    // bitmap, so doing a full wxImage::Scale here on every wheel event is
    // pure wasted work.  The zoom-settle timer triggers one rebuild when
    // the user stops scrolling.
    if (m_isZooming)
        return;

    if (!m_cacheDirty && std::fabs(m_cachedZoom - m_zoom) < 1e-9)
        return;

    RebuildDisplayCache();
}

void Document::NotifyCanvasScrolled()
{
    SyncRulers();
}

void Document::RequestCanvasRefresh()
{
    if (m_canvas)
    {
        if (m_selectedLayer >= 0)
            m_canvas->MarkGLLayerDirty(m_selectedLayer);

        m_canvas->Refresh(false);
    }
}

void Document::RequestCanvasRefreshRect(const wxRect& docRect)
{
    if (!m_canvas || docRect.IsEmpty())
        return;

    if (m_selectedLayer >= 0 && m_selectedLayer < static_cast<int>(m_layers.size()))
    {
        const DocumentLayer& layer = m_layers[m_selectedLayer];

        wxRect localRect(docRect.x - layer.offsetX, docRect.y - layer.offsetY, docRect.width, docRect.height);

        localRect.Inflate(2, 2);
        m_canvas->MarkGLLayerDirtyRect(m_selectedLayer, localRect);
    }

    const double zoom = m_zoom;
    const wxPoint pageOrigin = GetPageOriginInVirtualPixels();

    const int left = static_cast<int>(std::floor(static_cast<double>(pageOrigin.x) + static_cast<double>(docRect.x) * zoom));
    const int top = static_cast<int>(std::floor(static_cast<double>(pageOrigin.y) + static_cast<double>(docRect.y) * zoom));
    const int right = static_cast<int>(std::ceil(static_cast<double>(pageOrigin.x) + static_cast<double>(docRect.x + docRect.width) * zoom));
    const int bottom = static_cast<int>(std::ceil(static_cast<double>(pageOrigin.y) + static_cast<double>(docRect.y + docRect.height) * zoom));

    wxRect screenRect(left, top, right - left, bottom - top);
    screenRect.Inflate(4, 4);

    m_canvas->RefreshRect(screenRect, false);
}

DocumentCanvas* Document::GetCanvas() const
{
    return m_canvas;
}

const std::vector<wxString>& Document::GetHistoryLabels() const
{
    return m_historyLabels;
}

wxPoint Document::GetViewOffset() const
{
    return m_viewOffset;
}

void Document::SetViewOffset(const wxPoint& viewOffset)
{
    m_viewOffset = viewOffset;
    SyncRulers();
    RequestCanvasRefresh();
}

wxPoint Document::ScreenToWorld(const wxPoint& screenPoint) const
{
    return wxPoint(
        static_cast<int>(std::lround((static_cast<double>(screenPoint.x) - static_cast<double>(m_viewOffset.x)) / m_zoom)),
        static_cast<int>(std::lround((static_cast<double>(screenPoint.y) - static_cast<double>(m_viewOffset.y)) / m_zoom))
    );
}

wxPoint Document::ScreenToWorldPixel(const wxPoint& screenPoint) const
{
    if (m_zoom <= 0.0)
        return wxPoint(0, 0);

    return wxPoint(
        static_cast<int>(std::floor((static_cast<double>(screenPoint.x) - static_cast<double>(m_viewOffset.x)) / m_zoom)),
        static_cast<int>(std::floor((static_cast<double>(screenPoint.y) - static_cast<double>(m_viewOffset.y)) / m_zoom))
    );
}

wxPoint Document::WorldToCanvasVirtual(const wxPoint& worldPoint) const
{
    return wxPoint(
        static_cast<int>(std::lround(static_cast<double>(worldPoint.x) * m_zoom + static_cast<double>(m_viewOffset.x))),
        static_cast<int>(std::lround(static_cast<double>(worldPoint.y) * m_zoom + static_cast<double>(m_viewOffset.y)))
    );
}

void Document::UpdateViewCenter()
{
    if (!m_canvas)
        return;

    const wxSize clientSize = m_canvas->GetClientSize();
    const wxSize scaledPageSize = GetScaledPageSize();

    m_viewOffset = wxPoint((clientSize.x - scaledPageSize.x) / 2, (clientSize.y - scaledPageSize.y) / 2);
}

void Document::SyncRulers()
{
    if (!m_hRuler || !m_vRuler)
        return;

    m_hRuler->SetZoom(static_cast<float>(m_zoom));
    m_vRuler->SetZoom(static_cast<float>(m_zoom));
    m_hRuler->SetOffset(-m_viewOffset.x);
    m_vRuler->SetOffset(-m_viewOffset.y);
}

void Document::MarkDisplayCacheDirty()
{
    InvalidateDisplayCache();
    RebuildOriginalBitmapCache();
    RebuildPreviewCache();

    if (m_canvas)
        m_canvas->MarkAllGLLayersDirty();
}

void Document::InvalidateDisplayCache()
{
    m_cachedLayerBitmaps.clear();
    m_cachedZoom = -1.0;
    m_cacheDirty = true;
}

void Document::RebuildOriginalBitmapCache()
{
    m_originalLayerBitmaps.clear();
    m_originalLayerBitmaps.reserve(m_layers.size());

    for (const DocumentLayer& layer : m_layers)
    {
        if (!layer.image.IsOk())
        {
            m_originalLayerBitmaps.push_back(wxBitmap());
            continue;
        }

        m_originalLayerBitmaps.push_back(wxBitmap(layer.image));
    }
}

void Document::RebuildPreviewCache()
{
    m_previewLayerBitmaps.clear();
    m_previewLayerBitmaps.reserve(m_layers.size());

    for (const DocumentLayer& layer : m_layers)
    {
        if (!layer.image.IsOk())
        {
            m_previewLayerBitmaps.push_back(wxBitmap());
            continue;
        }

        const int srcW = layer.image.GetWidth();
        const int srcH = layer.image.GetHeight();

        if (srcW <= k_previewMaxDim && srcH <= k_previewMaxDim)
        {
            m_previewLayerBitmaps.push_back(wxBitmap(layer.image));
            continue;
        }

        const double scale = std::min(static_cast<double>(k_previewMaxDim) / static_cast<double>(srcW), static_cast<double>(k_previewMaxDim) / static_cast<double>(srcH));

        const int pw = std::max(1, static_cast<int>(srcW * scale + 0.5));
        const int ph = std::max(1, static_cast<int>(srcH * scale + 0.5));

        wxImage thumb = layer.image.Scale(pw, ph, wxIMAGE_QUALITY_NEAREST);
        m_previewLayerBitmaps.push_back(thumb.IsOk() ? wxBitmap(thumb) : wxBitmap());
    }
}

void Document::BeginInteractiveZoom()
{
    m_isZooming = true;
    // Give the user 250 ms of inactivity before we do the (now lightweight)
    // cache rebuild; this keeps the fast StretchBlit render path active during
    // rapid scroll-wheel gestures without any visible delay on settle.
    m_zoomSettleTimer.StartOnce(250);
}

void Document::EndInteractiveZoom()
{
    m_zoomSettleTimer.Stop();
    m_isZooming = false;
}

double Document::GetResolution() const
{
    return m_resolution;
}

bool Document::ResizeImage(int newWidth, int newHeight, double newResolution, int resampleQuality)
{
    newWidth = std::max(1, newWidth);
    newHeight = std::max(1, newHeight);
    newResolution = std::max(1.0, newResolution);

    const bool sameSize = (newWidth == m_pageWidth && newHeight == m_pageHeight);
    const bool sameResolution = (std::fabs(newResolution - m_resolution) < 1e-9);

    if (sameSize && sameResolution)
        return false;

    PushHistorySnapshot("Resize Image");

    const int oldPageWidth = std::max(1, m_pageWidth);
    const int oldPageHeight = std::max(1, m_pageHeight);

    wxImageResizeQuality quality = wxIMAGE_QUALITY_BICUBIC;
    if (resampleQuality == 0)
        quality = wxIMAGE_QUALITY_NEAREST;
    else if (resampleQuality == 1)
        quality = wxIMAGE_QUALITY_BILINEAR;
    else
        quality = wxIMAGE_QUALITY_BICUBIC;

    const double scaleX = static_cast<double>(newWidth) / static_cast<double>(oldPageWidth);
    const double scaleY = static_cast<double>(newHeight) / static_cast<double>(oldPageHeight);

    for (DocumentLayer& layer : m_layers)
    {
        if (!layer.image.IsOk())
            continue;

        if (layer.image.GetWidth() == oldPageWidth && layer.image.GetHeight() == oldPageHeight)
        {
            wxImage scaled = layer.image.Scale(newWidth, newHeight, quality);
            if (scaled.IsOk())
                layer.image = scaled;
        }
        else
        {
            const int scaledLayerW = std::max(1, static_cast<int>(std::lround(static_cast<double>(layer.image.GetWidth()) * scaleX)));
            const int scaledLayerH = std::max(1, static_cast<int>(std::lround(static_cast<double>(layer.image.GetHeight()) * scaleY)));

            wxImage scaled = layer.image.Scale(scaledLayerW, scaledLayerH, quality);
            if (scaled.IsOk())
                layer.image = scaled;
        }

        layer.offsetX = static_cast<int>(std::lround(static_cast<double>(layer.offsetX) * scaleX));
        layer.offsetY = static_cast<int>(std::lround(static_cast<double>(layer.offsetY) * scaleY));
    }

    m_pageWidth = newWidth;
    m_pageHeight = newHeight;
    m_resolution = newResolution;

    MarkDisplayCacheDirty();
    UpdateViewCenter();
    SyncRulers();
    NotifyDocumentChanged();
    return true;
}

bool Document::ResizeCanvas(int newWidth, int newHeight, AnchorGridPanel::AnchorPosition anchor, const wxColour& fillColor)
{
    newWidth = std::max(1, newWidth);
    newHeight = std::max(1, newHeight);

    const int oldWidth = std::max(1, m_pageWidth);
    const int oldHeight = std::max(1, m_pageHeight);

    if (newWidth == oldWidth && newHeight == oldHeight)
        return false;

    int dstX = 0;
    int dstY = 0;

    switch (anchor)
    {
        case AnchorGridPanel::TOP_LEFT:
            dstX = newWidth - oldWidth;
            dstY = newHeight - oldHeight;
            break;

        case AnchorGridPanel::TOP_CENTER:
            dstX = (newWidth - oldWidth) / 2;
            dstY = newHeight - oldHeight;
            break;

        case AnchorGridPanel::TOP_RIGHT:
            dstX = 0;
            dstY = newHeight - oldHeight;
            break;

        case AnchorGridPanel::MIDDLE_LEFT:
            dstX = newWidth - oldWidth;
            dstY = (newHeight - oldHeight) / 2;
            break;

        case AnchorGridPanel::CENTER:
            dstX = (newWidth - oldWidth) / 2;
            dstY = (newHeight - oldHeight) / 2;
            break;

        case AnchorGridPanel::MIDDLE_RIGHT:
            dstX = 0;
            dstY = (newHeight - oldHeight) / 2;
            break;

        case AnchorGridPanel::BOTTOM_LEFT:
            dstX = newWidth - oldWidth;
            dstY = 0;
            break;

        case AnchorGridPanel::BOTTOM_CENTER:
            dstX = (newWidth - oldWidth) / 2;
            dstY = 0;
            break;

        case AnchorGridPanel::BOTTOM_RIGHT:
            dstX = 0;
            dstY = 0;
            break;

        default:
            dstX = (newWidth - oldWidth) / 2;
            dstY = (newHeight - oldHeight) / 2;
            break;
    }

    PushHistorySnapshot("Canvas Size");

    for (DocumentLayer& layer : m_layers)
    {
        if (!layer.image.IsOk())
            continue;

        const bool fullCanvasLayer =
            layer.offsetX == 0 &&
            layer.offsetY == 0 &&
            layer.image.GetWidth() == oldWidth &&
            layer.image.GetHeight() == oldHeight;

        if (fullCanvasLayer)
        {
            wxImage newImage(newWidth, newHeight, true);

            if (!newImage.IsOk())
                continue;

            newImage.SetRGB(wxRect(0, 0, newWidth, newHeight), fillColor.Red(), fillColor.Green(), fillColor.Blue());

            if (!newImage.HasAlpha())
                newImage.InitAlpha();

            unsigned char* dstAlpha = newImage.GetAlpha();

            if (dstAlpha)
                std::fill(dstAlpha, dstAlpha + static_cast<size_t>(newWidth) * static_cast<size_t>(newHeight), 255);

            const wxImage& oldImage = layer.image;

            const unsigned char* srcData = oldImage.GetData();
            const unsigned char* srcAlpha = oldImage.HasAlpha() ? oldImage.GetAlpha() : nullptr;
            unsigned char* dstData = newImage.GetData();

            if (!srcData || !dstData)
                continue;

            int srcX = 0;
            int srcY = 0;
            int copyX = dstX;
            int copyY = dstY;

            if (copyX < 0)
            {
                srcX = -copyX;
                copyX = 0;
            }

            if (copyY < 0)
            {
                srcY = -copyY;
                copyY = 0;
            }

            const int copyW = std::min(oldWidth - srcX, newWidth - copyX);
            const int copyH = std::min(oldHeight - srcY, newHeight - copyY);

            if (copyW > 0 && copyH > 0)
            {
                for (int y = 0; y < copyH; ++y)
                {
                    for (int x = 0; x < copyW; ++x)
                    {
                        const int si = (srcY + y) * oldWidth + (srcX + x);
                        const int di = (copyY + y) * newWidth + (copyX + x);

                        dstData[di * 3 + 0] = srcData[si * 3 + 0];
                        dstData[di * 3 + 1] = srcData[si * 3 + 1];
                        dstData[di * 3 + 2] = srcData[si * 3 + 2];

                        if (dstAlpha)
                            dstAlpha[di] = srcAlpha ? srcAlpha[si] : 255;
                    }
                }
            }

            layer.image = newImage;
            layer.offsetX = 0;
            layer.offsetY = 0;
        }
        else
        {
            layer.offsetX += dstX;
            layer.offsetY += dstY;
        }
    }

    m_pageWidth = newWidth;
    m_pageHeight = newHeight;

    MarkDisplayCacheDirty();
    UpdateViewCenter();
    SyncRulers();
    NotifyDocumentChanged();
    RequestCanvasRefresh();

    return true;
}

bool Document::RotateCanvas180()
{
    if (m_layers.empty())
        return false;

    PushHistorySnapshot("Rotate 180");

    bool changed = false;

    for (DocumentLayer& layer : m_layers)
    {
        if (!layer.image.IsOk())
            continue;

        wxImage rotated = layer.image.Rotate180();

        if (!rotated.IsOk())
            continue;

        layer.image = rotated;

        layer.offsetX = m_pageWidth  - layer.offsetX - layer.image.GetWidth();
        layer.offsetY = m_pageHeight - layer.offsetY - layer.image.GetHeight();

        changed = true;
    }

    if (!changed)
        return false;

    MarkDisplayCacheDirty();
    SyncRulers();
    NotifyDocumentChanged();
    RequestCanvasRefresh();

    return true;
}

bool Document::RotateCanvas90CW()
{
    if (m_layers.empty())
        return false;

    PushHistorySnapshot("Rotate 90 CW");

    const int oldW = m_pageWidth;
    const int oldH = m_pageHeight;

    // swap canvas size
    m_pageWidth = oldH;
    m_pageHeight = oldW;

    bool changed = false;

    for (DocumentLayer& layer : m_layers)
    {
        if (!layer.image.IsOk())
            continue;

        wxImage rotated = layer.image.Rotate90(true); // CW

        if (!rotated.IsOk())
            continue;

        const int oldOffsetX = layer.offsetX;
        const int oldOffsetY = layer.offsetY;

        const int oldImgH = layer.image.GetHeight();

        layer.image = rotated;

        // rotate position
        layer.offsetX = oldH - (oldOffsetY + oldImgH);
        layer.offsetY = oldOffsetX;

        changed = true;
    }

    if (!changed)
        return false;

    MarkDisplayCacheDirty();
    SyncRulers();
    NotifyDocumentChanged();
    RequestCanvasRefresh();

    return true;
}

bool Document::RotateCanvas90CCW()
{
    if (m_layers.empty())
        return false;

    PushHistorySnapshot("Rotate 90 CCW");

    const int oldW = m_pageWidth;
    const int oldH = m_pageHeight;

    m_pageWidth = oldH;
    m_pageHeight = oldW;

    bool changed = false;

    for (DocumentLayer& layer : m_layers)
    {
        if (!layer.image.IsOk())
            continue;

        wxImage rotated = layer.image.Rotate90(false); // CCW
        if (!rotated.IsOk())
            continue;

        const int oldOffsetX = layer.offsetX;
        const int oldOffsetY = layer.offsetY;
        const int oldImgW = layer.image.GetWidth();

        layer.image = rotated;

        layer.offsetX = oldOffsetY;
        layer.offsetY = oldW - (oldOffsetX + oldImgW);

        changed = true;
    }

    if (!changed)
        return false;

    MarkDisplayCacheDirty();
    SyncRulers();
    NotifyDocumentChanged();
    RequestCanvasRefresh();

    return true;
}

bool Document::RotateCanvasArbitrary(double degreesClockwise)
{
    if (m_layers.empty())
        return false;

    const double pi = 3.14159265358979323846;
    const double radians = -degreesClockwise * pi / 180.0;
    const int oldW = m_pageWidth;
    const int oldH = m_pageHeight;
    const wxPoint centre(oldW / 2, oldH / 2);

    PushHistorySnapshot("Rotate Canvas");

    bool changed = false;
    int newW = oldW;
    int newH = oldH;

    for (DocumentLayer& layer : m_layers)
    {
        if (!layer.image.IsOk())
            continue;

        wxImage canvas(oldW, oldH, true);
        canvas.SetRGB(wxRect(0, 0, oldW, oldH), 255, 255, 255);
        canvas.InitAlpha();
        std::fill(canvas.GetAlpha(), canvas.GetAlpha() + oldW * oldH, 0);

        const unsigned char* srcData = layer.image.GetData();
        const unsigned char* srcAlpha = layer.image.HasAlpha() ? layer.image.GetAlpha() : nullptr;
        unsigned char* dstData = canvas.GetData();
        unsigned char* dstAlpha = canvas.GetAlpha();

        for (int y = 0; y < layer.image.GetHeight(); ++y)
        {
            for (int x = 0; x < layer.image.GetWidth(); ++x)
            {
                const int dx = layer.offsetX + x;
                const int dy = layer.offsetY + y;
                if (dx < 0 || dy < 0 || dx >= oldW || dy >= oldH)
                    continue;

                const int si = y * layer.image.GetWidth() + x;
                const int di = dy * oldW + dx;
                dstData[di * 3 + 0] = srcData[si * 3 + 0];
                dstData[di * 3 + 1] = srcData[si * 3 + 1];
                dstData[di * 3 + 2] = srcData[si * 3 + 2];
                dstAlpha[di] = srcAlpha ? srcAlpha[si] : 255;
            }
        }

        wxPoint offset;
        wxImage rotated = canvas.Rotate(radians, centre, true, &offset);
        if (!rotated.IsOk())
            continue;

        layer.image = rotated;
        layer.offsetX = 0;
        layer.offsetY = 0;
        newW = rotated.GetWidth();
        newH = rotated.GetHeight();
        changed = true;
    }

    if (!changed)
        return false;

    m_pageWidth = newW;
    m_pageHeight = newH;
    MarkDisplayCacheDirty();
    UpdateViewCenter();
    SyncRulers();
    NotifyDocumentChanged();
    RequestCanvasRefresh();
    return true;
}

bool Document::FlipCanvasHorizontal()
{
    if (m_layers.empty())
        return false;

    PushHistorySnapshot("Flip Horizontal");

    bool changed = false;

    for (DocumentLayer& layer : m_layers)
    {
        if (!layer.image.IsOk())
            continue;

        wxImage flipped = layer.image.Mirror(true); // horizontal
        if (!flipped.IsOk())
            continue;

        const int oldImgW = layer.image.GetWidth();

        layer.image = flipped;
        layer.offsetX = m_pageWidth - layer.offsetX - oldImgW;

        changed = true;
    }

    if (!changed)
        return false;

    MarkDisplayCacheDirty();
    SyncRulers();
    NotifyDocumentChanged();
    RequestCanvasRefresh();

    return true;
}

bool Document::FlipCanvasVertical()
{
    if (m_layers.empty())
        return false;

    PushHistorySnapshot("Flip Vertical");

    bool changed = false;

    for (DocumentLayer& layer : m_layers)
    {
        if (!layer.image.IsOk())
            continue;

        wxImage flipped = layer.image.Mirror(false); // vertical
        if (!flipped.IsOk())
            continue;

        const int oldImgH = layer.image.GetHeight();

        layer.image = flipped;
        layer.offsetY = m_pageHeight - layer.offsetY - oldImgH;

        changed = true;
    }

    if (!changed)
        return false;

    MarkDisplayCacheDirty();
    SyncRulers();
    NotifyDocumentChanged();
    RequestCanvasRefresh();

    return true;
}

bool Document::PickVisibleColorAt(const wxPoint& docPoint, wxColour& outColor) const
{
    if (docPoint.x < 0 || docPoint.y < 0 || docPoint.x >= m_pageWidth || docPoint.y >= m_pageHeight)
        return false;

    double outR = 0.0;
    double outG = 0.0;
    double outB = 0.0;
    double outA = 0.0;

    for (const DocumentLayer& layer : m_layers)
    {
        if (!layer.visible || !layer.image.IsOk())
            continue;

        const int lx = docPoint.x - layer.offsetX;
        const int ly = docPoint.y - layer.offsetY;

        if (lx < 0 || ly < 0 || lx >= layer.image.GetWidth() || ly >= layer.image.GetHeight())
            continue;

        const unsigned char* data = layer.image.GetData();

        if (!data)
            continue;

        const int pixelIndex = ly * layer.image.GetWidth() + lx;
        const int rgbIndex = pixelIndex * 3;

        double srcA = 1.0;

        if (layer.image.HasAlpha())
        {
            const unsigned char* alpha = layer.image.GetAlpha();

            if (!alpha)
                continue;

            srcA = static_cast<double>(alpha[pixelIndex]) / 255.0;
        }

        if (srcA <= 0.0)
            continue;

        const double srcR = static_cast<double>(data[rgbIndex + 0]) / 255.0;
        const double srcG = static_cast<double>(data[rgbIndex + 1]) / 255.0;
        const double srcB = static_cast<double>(data[rgbIndex + 2]) / 255.0;

        const double newA = srcA + outA * (1.0 - srcA);

        if (newA <= 0.0)
            continue;

        outR = (srcR * srcA + outR * outA * (1.0 - srcA)) / newA;
        outG = (srcG * srcA + outG * outA * (1.0 - srcA)) / newA;
        outB = (srcB * srcA + outB * outA * (1.0 - srcA)) / newA;
        outA = newA;
    }

    if (outA <= 0.0)
        return false;

    outColor = wxColour(
        static_cast<unsigned char>(std::clamp(outR, 0.0, 1.0) * 255.0 + 0.5),
        static_cast<unsigned char>(std::clamp(outG, 0.0, 1.0) * 255.0 + 0.5),
        static_cast<unsigned char>(std::clamp(outB, 0.0, 1.0) * 255.0 + 0.5)
    );

    return true;
}

wxImage Document::CopySelectedLayerImage() const
{
    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return wxImage();

    const wxImage& image = m_layers[m_selectedLayer].image;
    return image.IsOk() ? image.Copy() : wxImage();
}

bool Document::ReplaceSelectedLayerImage(const wxImage& image, bool addHistory, const wxString& historyLabel)
{
    if (!image.IsOk())
        return false;

    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return false;

    DocumentLayer& layer = m_layers[m_selectedLayer];

    if (!layer.image.IsOk())
        return false;

    if (image.GetWidth() != layer.image.GetWidth() || image.GetHeight() != layer.image.GetHeight())
        return false;

    if (addHistory)
        BeginHistoryTransaction(historyLabel);

    layer.image = image.Copy();

    MarkDisplayCacheDirty();

    if (m_canvas)
        m_canvas->MarkGLLayerDirty(m_selectedLayer);

    RequestCanvasRefresh();
    NotifyDocumentChanged();

    if (addHistory)
        EndHistoryTransaction();

    return true;
}

bool Document::PreviewSelectedLayerImage(const wxImage& image)
{
    if (!image.IsOk())
        return false;

    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return false;

    DocumentLayer& layer = m_layers[m_selectedLayer];

    if (!layer.image.IsOk())
        return false;

    if (image.GetWidth() != layer.image.GetWidth() || image.GetHeight() != layer.image.GetHeight())
        return false;

    layer.image = image.Copy();

    MarkDisplayCacheDirty();

    if (m_canvas)
        m_canvas->MarkGLLayerDirty(m_selectedLayer);

    RequestCanvasRefresh();

    return true;
}

wxImage Document::CopyVisibleMergedImage(bool applyChannelVisibility) const
{
    if (m_pageWidth <= 0 || m_pageHeight <= 0)
        return wxImage();

    wxImage merged(m_pageWidth, m_pageHeight, true);
    merged.SetRGB(wxRect(0, 0, m_pageWidth, m_pageHeight), 255, 255, 255);
    merged.InitAlpha();

    unsigned char* dstData = merged.GetData();
    unsigned char* dstAlpha = merged.GetAlpha();

    if (!dstData || !dstAlpha)
        return wxImage();

    std::fill(dstAlpha, dstAlpha + (m_pageWidth * m_pageHeight), 0);

    for (const DocumentLayer& layer : m_layers)
    {
        if (!layer.visible || !layer.image.IsOk())
            continue;

        const unsigned char* srcData = layer.image.GetData();
        const unsigned char* srcAlpha = layer.image.HasAlpha() ? layer.image.GetAlpha() : nullptr;

        if (!srcData)
            continue;

        const int lw = layer.image.GetWidth();
        const int lh = layer.image.GetHeight();

        for (int y = 0; y < lh; ++y)
        {
            const int dy = layer.offsetY + y;

            if (dy < 0 || dy >= m_pageHeight)
                continue;

            for (int x = 0; x < lw; ++x)
            {
                const int dx = layer.offsetX + x;

                if (dx < 0 || dx >= m_pageWidth)
                    continue;

                const int srcIndex = y * lw + x;
                const int srcRgb = srcIndex * 3;

                const int dstIndex = dy * m_pageWidth + dx;
                const int dstRgb = dstIndex * 3;

                double sa = srcAlpha ? static_cast<double>(srcAlpha[srcIndex]) / 255.0 : 1.0;
                sa *= static_cast<double>(layer.opacity) / 100.0;

                if (sa <= 0.0)
                    continue;

                const double sr = static_cast<double>(srcData[srcRgb + 0]) / 255.0;
                const double sg = static_cast<double>(srcData[srcRgb + 1]) / 255.0;
                const double sb = static_cast<double>(srcData[srcRgb + 2]) / 255.0;

                const double da = static_cast<double>(dstAlpha[dstIndex]) / 255.0;
                const double dr = static_cast<double>(dstData[dstRgb + 0]) / 255.0;
                const double dg = static_cast<double>(dstData[dstRgb + 1]) / 255.0;
                const double db = static_cast<double>(dstData[dstRgb + 2]) / 255.0;

                const double outA = sa + da * (1.0 - sa);

                if (outA <= 0.0)
                    continue;

                const double outR = (sr * sa + dr * da * (1.0 - sa)) / outA;
                const double outG = (sg * sa + dg * da * (1.0 - sa)) / outA;
                const double outB = (sb * sa + db * da * (1.0 - sa)) / outA;

                dstData[dstRgb + 0] = static_cast<unsigned char>(std::clamp(outR, 0.0, 1.0) * 255.0 + 0.5);
                dstData[dstRgb + 1] = static_cast<unsigned char>(std::clamp(outG, 0.0, 1.0) * 255.0 + 0.5);
                dstData[dstRgb + 2] = static_cast<unsigned char>(std::clamp(outB, 0.0, 1.0) * 255.0 + 0.5);
                dstAlpha[dstIndex] = static_cast<unsigned char>(std::clamp(outA, 0.0, 1.0) * 255.0 + 0.5);
            }
        }
    }

    if (applyChannelVisibility)
        ApplyChannelVisibilityToImage(merged);

    return merged;
}

void Document::OnZoomSettleTimer(wxTimerEvent&)
{
    m_isZooming = false;
    RequestCanvasRefresh();
}

void Document::RebuildDisplayCache()
{
    m_cachedLayerBitmaps.clear();
    m_cachedLayerBitmaps.reserve(m_layers.size());

    // Keep the cached-bitmap path only for images that are cheap to pre-scale
    // (≤ ~2 000×2 000 at the current zoom).  For larger results we skip the
    // cache and let OnPaint use StretchBlit from the original, which is GPU-
    // accelerated and doesn't block the UI thread for hundreds of milliseconds.
    const int maxDimension = 16384;
    const long long maxPixels = 4000000LL;

    for (const DocumentLayer& layer : m_layers)
    {
        if (!layer.image.IsOk())
        {
            m_cachedLayerBitmaps.push_back(wxBitmap());
            continue;
        }

        const int drawW = std::max(1, static_cast<int>(std::lround(static_cast<double>(layer.image.GetWidth()) * m_zoom)));
        const int drawH = std::max(1, static_cast<int>(std::lround(static_cast<double>(layer.image.GetHeight()) * m_zoom)));

        const long long pixelCount = static_cast<long long>(drawW) * static_cast<long long>(drawH);

        if (drawW > maxDimension || drawH > maxDimension || pixelCount > maxPixels)
        {
            m_cachedLayerBitmaps.push_back(wxBitmap());
            continue;
        }

        wxImage scaled = layer.image.Scale(drawW, drawH, wxIMAGE_QUALITY_NEAREST);
        if (!scaled.IsOk())
        {
            m_cachedLayerBitmaps.push_back(wxBitmap());
            continue;
        }

        wxBitmap bmp(scaled);
        if (!bmp.IsOk())
        {
            m_cachedLayerBitmaps.push_back(wxBitmap());
            continue;
        }

        m_cachedLayerBitmaps.push_back(bmp);
    }

    m_cachedZoom = m_zoom;
    m_cacheDirty = false;
}

void Document::UpdateZoomStatusText()
{
    wxFrame* frame = wxDynamicCast(wxGetTopLevelParent(this), wxFrame);
    if (!frame)
        return;

    StatusBar* statusBar = dynamic_cast<StatusBar*>(frame->GetStatusBar());
    if (!statusBar)
        return;

    statusBar->SetZoomText(wxString::Format("Zoom: %.1f%%", m_zoom * 100.0));
}

bool Document::IsLayerVisible(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;

    return m_layers[index].visible;
}

bool Document::IsLayerLocked(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;

    return m_layers[index].locked;
}

void Document::SetLayerVisible(int index, bool visible)
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return;

    if (m_layers[index].visible == visible)
        return;

    PushHistorySnapshot("Layer Visibility");
    m_layers[index].visible = visible;
    MarkDisplayCacheDirty();
    NotifyDocumentChanged();
}

void Document::SetLayerLocked(int index, bool locked)
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return;

    if (m_layers[index].locked == locked)
        return;

    PushHistorySnapshot("Layer Lock");
    m_layers[index].locked = locked;
    NotifyDocumentChanged();
}

void Document::OnSize(wxSizeEvent& event)
{
    SyncRulers();
    RequestCanvasRefresh();
    event.Skip();
}

void Document::SetAllLayersVisible(bool visible)
{
    bool changed = false;
    for (size_t i = 0; i < m_layers.size(); ++i)
    {
        if (m_layers[i].visible != visible)
        {
            changed = true;
            break;
        }
    }

    if (!changed)
        return;

    PushHistorySnapshot("All Layers Visibility");

    for (size_t i = 0; i < m_layers.size(); ++i)
        m_layers[i].visible = visible;

    MarkDisplayCacheDirty();
    NotifyDocumentChanged();
}

void Document::SetAllLayersLocked(bool locked)
{
    bool changed = false;
    for (size_t i = 0; i < m_layers.size(); ++i)
    {
        if (m_layers[i].locked != locked)
        {
            changed = true;
            break;
        }
    }

    if (!changed)
        return;

    PushHistorySnapshot("All Layers Lock");

    for (size_t i = 0; i < m_layers.size(); ++i)
        m_layers[i].locked = locked;

    NotifyDocumentChanged();
}

bool Document::AreAllLayersVisible() const
{
    if (m_layers.empty())
        return false;

    for (size_t i = 0; i < m_layers.size(); ++i)
    {
        if (!m_layers[i].visible)
            return false;
    }

    return true;
}

bool Document::AreAllLayersLocked() const
{
    if (m_layers.empty())
        return false;

    for (size_t i = 0; i < m_layers.size(); ++i)
    {
        if (!m_layers[i].locked)
            return false;
    }

    return true;
}

bool Document::IsCompositeChannelVisible() const
{
    return m_redChannelVisible && m_greenChannelVisible && m_blueChannelVisible;
}

bool Document::IsRedChannelVisible() const
{
    return m_redChannelVisible;
}

bool Document::IsGreenChannelVisible() const
{
    return m_greenChannelVisible;
}

bool Document::IsBlueChannelVisible() const
{
    return m_blueChannelVisible;
}

void Document::SetCompositeChannelVisible(bool visible)
{
    if (m_redChannelVisible == visible && m_greenChannelVisible == visible && m_blueChannelVisible == visible)
        return;

    m_redChannelVisible = visible;
    m_greenChannelVisible = visible;
    m_blueChannelVisible = visible;

    if (m_canvas)
        m_canvas->MarkAllGLLayersDirty();

    MarkDisplayCacheDirty();
    RequestCanvasRefresh();
    NotifyDocumentChanged();
}

void Document::SetRedChannelVisible(bool visible)
{
    if (m_redChannelVisible == visible)
        return;

    m_redChannelVisible = visible;

    if (m_canvas)
        m_canvas->MarkAllGLLayersDirty();

    MarkDisplayCacheDirty();
    RequestCanvasRefresh();
    NotifyDocumentChanged();
}

void Document::SetGreenChannelVisible(bool visible)
{
    if (m_greenChannelVisible == visible)
        return;

    m_greenChannelVisible = visible;

    if (m_canvas)
        m_canvas->MarkAllGLLayersDirty();

    MarkDisplayCacheDirty();
    RequestCanvasRefresh();
    NotifyDocumentChanged();
}

void Document::SetBlueChannelVisible(bool visible)
{
    if (m_blueChannelVisible == visible)
        return;

    m_blueChannelVisible = visible;

    if (m_canvas)
        m_canvas->MarkAllGLLayersDirty();

    MarkDisplayCacheDirty();
    RequestCanvasRefresh();
    NotifyDocumentChanged();
}

void Document::ApplyChannelVisibilityToImage(wxImage& image) const
{
    if (!image.IsOk())
        return;

    unsigned char* data = image.GetData();

    if (!data)
        return;

    if (!m_redChannelVisible && !m_greenChannelVisible && !m_blueChannelVisible)
    {
        if (!image.HasAlpha())
            image.InitAlpha();

        unsigned char* alpha = image.GetAlpha();

        if (alpha)
            std::fill(alpha, alpha + (image.GetWidth() * image.GetHeight()), 0);

        return;
    }

    const bool singleChannelVisible =
        (m_redChannelVisible && !m_greenChannelVisible && !m_blueChannelVisible) ||
        (!m_redChannelVisible && m_greenChannelVisible && !m_blueChannelVisible) ||
        (!m_redChannelVisible && !m_greenChannelVisible && m_blueChannelVisible);

    const int w = image.GetWidth();
    const int h = image.GetHeight();
    const int count = w * h;

    for (int i = 0; i < count; ++i)
    {
        const int rgb = i * 3;

        if (singleChannelVisible)
        {
            unsigned char value = 0;

            if (m_redChannelVisible)
                value = data[rgb + 0];
            else if (m_greenChannelVisible)
                value = data[rgb + 1];
            else if (m_blueChannelVisible)
                value = data[rgb + 2];

            data[rgb + 0] = value;
            data[rgb + 1] = value;
            data[rgb + 2] = value;
            continue;
        }

        if (!m_redChannelVisible)
            data[rgb + 0] = 0;

        if (!m_greenChannelVisible)
            data[rgb + 1] = 0;

        if (!m_blueChannelVisible)
            data[rgb + 2] = 0;
    }
}

void Document::AddLayer()
{
    BeginHistoryTransaction("Add Layer");

    DocumentLayer layer;
    layer.name = wxString::Format("Layer %d", static_cast<int>(m_layers.size()) + 1);
    layer.visible = true;
    layer.locked = false;
    layer.offsetX = 0;
    layer.offsetY = 0;

    layer.image = wxImage(m_pageWidth, m_pageHeight, true);
    layer.image.SetRGB(wxRect(0, 0, m_pageWidth, m_pageHeight), 255, 255, 255);

    if (!layer.image.HasAlpha())
        layer.image.InitAlpha();

    unsigned char* alpha = layer.image.GetAlpha();
    if (alpha)
        std::fill(alpha, alpha + (m_pageWidth * m_pageHeight), 0);

    int insertIndex = static_cast<int>(m_layers.size());
    if (m_selectedLayer >= 0 && m_selectedLayer < static_cast<int>(m_layers.size()))
        insertIndex = m_selectedLayer + 1;

    m_layers.insert(m_layers.begin() + insertIndex, layer);
    m_selectedLayer = insertIndex;

    MarkDisplayCacheDirty();
    NotifyDocumentChanged();

    EndHistoryTransaction();
}

bool Document::SelectedLayerHasAnyPixel() const
{
    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return false;

    const DocumentLayer& layer = m_layers[m_selectedLayer];
    if (!layer.image.IsOk())
        return false;

    const int width = layer.image.GetWidth();
    const int height = layer.image.GetHeight();
    if (width <= 0 || height <= 0)
        return false;

    if (layer.image.HasAlpha())
    {
        const unsigned char* alpha = layer.image.GetAlpha();
        if (alpha)
        {
            const int pixelCount = width * height;
            for (int i = 0; i < pixelCount; ++i)
            {
                if (alpha[i] != 0)
                    return true;
            }
            return false;
        }
    }

    return true;
}

bool Document::DeleteSelectedLayer()
{
    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return false;

    if (m_layers.size() <= 1)
        return false;

    BeginHistoryTransaction("Delete Layer");

    m_layers.erase(m_layers.begin() + m_selectedLayer);

    if (m_selectedLayer >= static_cast<int>(m_layers.size()))
        m_selectedLayer = static_cast<int>(m_layers.size()) - 1;

    MarkDisplayCacheDirty();
    NotifyDocumentChanged();

    EndHistoryTransaction();
    return true;
}

void Document::DuplicateSelectedLayer()
{
    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return;

    BeginHistoryTransaction("Duplicate Layer");

    DocumentLayer duplicated = m_layers[m_selectedLayer];
    duplicated.name += " Copy";

    if (duplicated.image.IsOk())
        duplicated.image = duplicated.image.Copy();

    const int insertIndex = m_selectedLayer + 1;
    m_layers.insert(m_layers.begin() + insertIndex, duplicated);
    m_selectedLayer = insertIndex;

    MarkDisplayCacheDirty();
    NotifyDocumentChanged();

    EndHistoryTransaction();
}

wxString Document::GetSelectedLayerName() const
{
    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return wxString();

    return m_layers[m_selectedLayer].name;
}

void Document::SetSelectedLayerName(const wxString& name)
{
    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return;

    wxString trimmed = name;
    trimmed.Trim(true).Trim(false);

    if (trimmed.IsEmpty())
        return;

    if (m_layers[m_selectedLayer].name == trimmed)
        return;

    BeginHistoryTransaction("Rename Layer");

    m_layers[m_selectedLayer].name = trimmed;
    NotifyDocumentChanged();

    EndHistoryTransaction();
}

ToolType Document::GetActiveTool() const
{
    return m_activeTool;
}

void Document::SetActiveTool(ToolType tool)
{
    m_activeTool = tool;
    RequestCanvasRefresh();
}

bool Document::CanMoveSelectedLayer() const
{
    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return false;

    const DocumentLayer& layer = m_layers[m_selectedLayer];
    return layer.visible && !layer.locked && layer.image.IsOk();
}

wxPoint Document::GetSelectedLayerOffset() const
{
    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return wxPoint(0, 0);

    return wxPoint(m_layers[m_selectedLayer].offsetX, m_layers[m_selectedLayer].offsetY);
}

void Document::SetSelectedLayerOffset(int offsetX, int offsetY)
{
    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return;

    DocumentLayer& layer = m_layers[m_selectedLayer];
    if (layer.offsetX == offsetX && layer.offsetY == offsetY)
        return;

    layer.offsetX = offsetX;
    layer.offsetY = offsetY;
    RequestCanvasRefresh();
}

bool Document::TransformSelectedLayerToRect(const wxImage& sourceImage, const wxRect& targetRect)
{
    if (!sourceImage.IsOk())
        return false;

    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return false;

    const int newW = std::max(1, targetRect.width);
    const int newH = std::max(1, targetRect.height);

    wxImage scaled = sourceImage.Scale(newW, newH, wxIMAGE_QUALITY_HIGH);

    if (!scaled.IsOk())
        return false;

    if (!scaled.HasAlpha())
        scaled.InitAlpha();

    const int resolutionInt = std::max(1, static_cast<int>(std::lround(m_resolution)));

    scaled.SetOption(wxIMAGE_OPTION_RESOLUTION, resolutionInt);
    scaled.SetOption(wxIMAGE_OPTION_RESOLUTIONX, resolutionInt);
    scaled.SetOption(wxIMAGE_OPTION_RESOLUTIONY, resolutionInt);
    scaled.SetOption(wxIMAGE_OPTION_RESOLUTIONUNIT, wxIMAGE_RESOLUTION_INCHES);

    DocumentLayer& layer = m_layers[m_selectedLayer];
    layer.image = scaled;
    layer.offsetX = targetRect.x;
    layer.offsetY = targetRect.y;

    MarkDisplayCacheDirty();

    if (m_canvas)
        m_canvas->MarkGLLayerDirty(m_selectedLayer);

    RequestCanvasRefresh();
    NotifyDocumentChanged();

    return true;
}

bool Document::CanDrawOnSelectedLayer() const
{
    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return false;

    const DocumentLayer& layer = m_layers[m_selectedLayer];
    return layer.visible && !layer.locked && layer.image.IsOk();
}

bool Document::DrawOnSelectedLayerLine(const wxPoint& startDocPoint, const wxPoint& endDocPoint, int size, const wxColour& color, bool erase)
{
    if (!CanDrawOnSelectedLayer())
        return false;

    DocumentLayer& layer = m_layers[m_selectedLayer];
    wxImage& image = layer.image;

    if (!image.IsOk())
        return false;

    EnsureAlpha(image);

    const int brushSize = std::max(1, size);
    const int half = brushSize / 2;

    const wxPoint startLocal(startDocPoint.x - layer.offsetX, startDocPoint.y - layer.offsetY);
    const wxPoint endLocal(endDocPoint.x - layer.offsetX, endDocPoint.y - layer.offsetY);

    auto isPixelInRoundShape = [](int bx, int by, int sizeValue) -> bool
    {
        const double cx = static_cast<double>(sizeValue) * 0.5;
        const double cy = static_cast<double>(sizeValue) * 0.5;
        const double px = static_cast<double>(bx) + 0.5;
        const double py = static_cast<double>(by) + 0.5;
        const double dx = px - cx;
        const double dy = py - cy;
        const double r = static_cast<double>(sizeValue) * 0.5;
        return (dx * dx + dy * dy) <= (r * r);
    };

    auto drawDab = [&](int cx, int cy) -> bool
    {
        bool changed = false;

        for (int by = 0; by < brushSize; ++by)
        {
            for (int bx = 0; bx < brushSize; ++bx)
            {
                if (!isPixelInRoundShape(bx, by, brushSize))
                    continue;

                const int x = cx - half + bx;
                const int y = cy - half + by;

                if (ApplyPixel(image, x, y, color, erase))
                    changed = true;
            }
        }

        return changed;
    };

    bool anyChanged = false;

    int x0 = startLocal.x;
    int y0 = startLocal.y;
    const int x1 = endLocal.x;
    const int y1 = endLocal.y;

    const int dx = std::abs(x1 - x0);
    const int sx = (x0 < x1) ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    for (;;)
    {
        if (drawDab(x0, y0))
            anyChanged = true;

        if (x0 == x1 && y0 == y1)
            break;

        const int e2 = err * 2;

        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }

        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }

    if (!anyChanged)
        return false;

    RebuildDisplayCache();
    RequestCanvasRefresh();
    return true;
}

bool Document::DrawPencilOnSelectedLayerLine(const wxPoint& startDocPoint, const wxPoint& endDocPoint, int size, const wxColour& color)
{
    if (!CanDrawOnSelectedLayer())
        return false;

    DocumentLayer& layer = m_layers[m_selectedLayer];
    wxImage& image = layer.image;

    if (!image.IsOk())
        return false;

    EnsureAlpha(image);

    unsigned char* data = image.GetData();
    unsigned char* alpha = image.GetAlpha();

    if (!data || !alpha)
        return false;

    const int w = image.GetWidth();
    const int h = image.GetHeight();

    const unsigned char r = static_cast<unsigned char>(color.Red());
    const unsigned char g = static_cast<unsigned char>(color.Green());
    const unsigned char b = static_cast<unsigned char>(color.Blue());

    const int pencilSize = std::max(1, size);
    const int half = pencilSize / 2;

    const wxPoint startLocal(startDocPoint.x - layer.offsetX, startDocPoint.y - layer.offsetY);
    const wxPoint endLocal(endDocPoint.x - layer.offsetX, endDocPoint.y - layer.offsetY);

    auto isPixelInPencilShape = [pencilSize](int bx, int by) -> bool
    {
        const double cx = static_cast<double>(pencilSize) * 0.5;
        const double cy = static_cast<double>(pencilSize) * 0.5;
        const double px = static_cast<double>(bx) + 0.5;
        const double py = static_cast<double>(by) + 0.5;
        const double dx = px - cx;
        const double dy = py - cy;
        const double radius = static_cast<double>(pencilSize) * 0.5;

        return (dx * dx + dy * dy) <= (radius * radius);
    };

    wxRect dirtyLocalRect;
    bool hasDirtyRect = false;

    auto addDirtyLocalRect = [&](const wxRect& rect)
    {
        if (!hasDirtyRect)
        {
            dirtyLocalRect = rect;
            hasDirtyRect = true;
        }
        else
        {
            dirtyLocalRect.Union(rect);
        }
    };

    auto drawPencilDab = [&](int cx, int cy) -> bool
    {
        bool changed = false;

        const int dabLeft = cx - half;
        const int dabTop = cy - half;

        for (int by = 0; by < pencilSize; ++by)
        {
            for (int bx = 0; bx < pencilSize; ++bx)
            {
                if (!isPixelInPencilShape(bx, by))
                    continue;

                const int x = dabLeft + bx;
                const int y = dabTop + by;

                if (x < 0 || y < 0 || x >= w || y >= h)
                    continue;

                const int idx = y * w + x;
                const int rgb = idx * 3;

                if (data[rgb + 0] == r && data[rgb + 1] == g && data[rgb + 2] == b && alpha[idx] == 255)
                    continue;

                data[rgb + 0] = r;
                data[rgb + 1] = g;
                data[rgb + 2] = b;
                alpha[idx] = 255;
                changed = true;
            }
        }

        if (changed)
        {
            wxRect dabRect(std::max(0, dabLeft), std::max(0, dabTop), std::min(w, dabLeft + pencilSize) - std::max(0, dabLeft), std::min(h, dabTop + pencilSize) - std::max(0, dabTop));

            if (!dabRect.IsEmpty())
                addDirtyLocalRect(dabRect);
        }

        return changed;
    };

    bool anyChanged = false;

    int x0 = startLocal.x;
    int y0 = startLocal.y;
    const int x1 = endLocal.x;
    const int y1 = endLocal.y;

    const int dx = std::abs(x1 - x0);
    const int sx = (x0 < x1) ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    for (;;)
    {
        if (drawPencilDab(x0, y0))
            anyChanged = true;

        if (x0 == x1 && y0 == y1)
            break;

        const int e2 = err * 2;

        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }

        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }

    if (!anyChanged)
        return false;

    InvalidateDisplayCache();

    if (hasDirtyRect)
    {
        wxRect dirtyDocRect(dirtyLocalRect.x + layer.offsetX, dirtyLocalRect.y + layer.offsetY, dirtyLocalRect.width, dirtyLocalRect.height);

        RequestCanvasRefreshRect(dirtyDocRect);
    }
    else
    {
        RequestCanvasRefresh();
    }

    return true;
}

bool Document::DrawBrushOnSelectedLayerLine(const wxPoint& startDocPoint, const wxPoint& endDocPoint, int size, int hardness, int opacity, int flow, const wxColour& color, const wxImage* strokeBaseImage, std::vector<double>* strokeMask, bool erase, const std::vector<BrushDabPixel>* cachedDab)
{
    if (!CanDrawOnSelectedLayer())
        return false;

    DocumentLayer& layer = m_layers[m_selectedLayer];
    wxImage& image = layer.image;

    if (!image.IsOk())
        return false;

    EnsureAlpha(image);

    unsigned char* data = image.GetData();
    unsigned char* alpha = image.GetAlpha();

    if (!data || !alpha)
        return false;

    const int w = image.GetWidth();
    const int h = image.GetHeight();

    const wxImage* baseImage = (strokeBaseImage && strokeBaseImage->IsOk()) ? strokeBaseImage : &image;
    const unsigned char* baseData = baseImage->GetData();
    const unsigned char* baseAlpha = baseImage->HasAlpha() ? baseImage->GetAlpha() : nullptr;

    if (!baseData || baseImage->GetWidth() != w || baseImage->GetHeight() != h)
    {
        baseImage = &image;
        baseData = data;
        baseAlpha = alpha;
    }

    const bool hasStrokeMask = strokeMask && strokeMask->size() == static_cast<size_t>(w) * static_cast<size_t>(h);

    const int brushSizeInt = std::max(1, size);
    const int half = brushSizeInt / 2;

    const double brushSize = static_cast<double>(brushSizeInt);
    const double radius = brushSize * 0.5;
    const double radiusSq = radius * radius;

    const int hardnessClamped = std::max(0, std::min(100, hardness));

    const double hard01 = static_cast<double>(hardnessClamped) / 100.0;
    const double hardRadius = radius * (0.03 + hard01 * 0.45);
    const double softRange = std::max(1.0, radius - hardRadius);

    const double opacity01 = std::max(0.0, std::min(1.0, static_cast<double>(opacity) / 100.0));
    const double flow01 = std::max(0.0, std::min(1.0, static_cast<double>(flow) / 100.0));

    if (opacity01 <= 0.0 || flow01 <= 0.0)
        return false;

    const double srcR = color.Red() / 255.0;
    const double srcG = color.Green() / 255.0;
    const double srcB = color.Blue() / 255.0;

    const wxPoint startLocal(startDocPoint.x - layer.offsetX, startDocPoint.y - layer.offsetY);
    const wxPoint endLocal(endDocPoint.x - layer.offsetX, endDocPoint.y - layer.offsetY);

    wxRect dirtyLocalRect;
    bool hasDirtyRect = false;

    auto addDirtyLocalRect = [&](const wxRect& rect)
    {
        if (!hasDirtyRect)
        {
            dirtyLocalRect = rect;
            hasDirtyRect = true;
        }
        else
        {
            dirtyLocalRect.Union(rect);
        }
    };

    std::vector<BrushDabPixel> localDabPixels;
    const std::vector<BrushDabPixel>* dabPixels = cachedDab;

    if (!dabPixels || dabPixels->empty())
    {
        localDabPixels.reserve(static_cast<size_t>(brushSizeInt) * static_cast<size_t>(brushSizeInt));

        const double center = brushSize * 0.5;

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
                localDabPixels.push_back(p);
            }
        }

        dabPixels = &localDabPixels;
    }

    if (!dabPixels || dabPixels->empty())
        return false;

    auto drawBrushDab = [&](int cx, int cy, double taper) -> bool
    {
        bool changed = false;

        const int dabLeft = cx - half;
        const int dabTop = cy - half;

        const int rectLeft = std::max(0, dabLeft);
        const int rectTop = std::max(0, dabTop);
        const int rectRight = std::min(w, dabLeft + brushSizeInt);
        const int rectBottom = std::min(h, dabTop + brushSizeInt);

        wxRect dabRect(rectLeft, rectTop, rectRight - rectLeft, rectBottom - rectTop);

        if (dabRect.IsEmpty())
            return false;

        for (const BrushDabPixel& p : *dabPixels)
        {
            double strength = p.strength * taper;

            if (strength <= 0.0)
                continue;

            const int x = cx + p.ox;
            const int y = cy + p.oy;

            if (x < 0 || y < 0 || x >= w || y >= h)
                continue;

            const int idx = y * w + x;
            const int rgb = idx * 3;

            double maskValue = strength;

            if (hasStrokeMask)
            {
                double& storedMask = (*strokeMask)[static_cast<size_t>(idx)];

                const double addMask = strength * flow01;
                storedMask = storedMask + addMask * (1.0 - storedMask);
                storedMask = std::max(0.0, std::min(1.0, storedMask));

                maskValue = storedMask;
            }

            const double finalStrength = std::max(0.0, std::min(1.0, maskValue * opacity01));

            if (finalStrength <= 0.0)
                continue;

            const double baseA = baseAlpha ? baseAlpha[idx] / 255.0 : 1.0;
            const double baseR = baseData[rgb + 0] / 255.0;
            const double baseG = baseData[rgb + 1] / 255.0;
            const double baseB = baseData[rgb + 2] / 255.0;

            double targetA = baseA;
            double targetR = baseR;
            double targetG = baseG;
            double targetB = baseB;

            if (erase)
            {
                if (baseA <= 0.0)
                    continue;

                targetA = baseA * (1.0 - finalStrength);
            }
            else
            {
                targetA = baseA + finalStrength * (1.0 - baseA);

                if (targetA <= 0.0)
                    continue;

                targetR = (srcR * finalStrength + baseR * baseA * (1.0 - finalStrength)) / targetA;
                targetG = (srcG * finalStrength + baseG * baseA * (1.0 - finalStrength)) / targetA;
                targetB = (srcB * finalStrength + baseB * baseA * (1.0 - finalStrength)) / targetA;
            }

            const unsigned char nr = static_cast<unsigned char>(std::max(0.0, std::min(255.0, targetR * 255.0)));
            const unsigned char ng = static_cast<unsigned char>(std::max(0.0, std::min(255.0, targetG * 255.0)));
            const unsigned char nb = static_cast<unsigned char>(std::max(0.0, std::min(255.0, targetB * 255.0)));
            const unsigned char na = static_cast<unsigned char>(std::max(0.0, std::min(255.0, targetA * 255.0)));

            if (data[rgb + 0] == nr && data[rgb + 1] == ng && data[rgb + 2] == nb && alpha[idx] == na)
                continue;

            data[rgb + 0] = nr;
            data[rgb + 1] = ng;
            data[rgb + 2] = nb;
            alpha[idx] = na;

            changed = true;
        }

        if (changed)
            addDirtyLocalRect(dabRect);

        return changed;
    };

    bool anyChanged = false;

    const double lineDx = static_cast<double>(endLocal.x - startLocal.x);
    const double lineDy = static_cast<double>(endLocal.y - startLocal.y);
    const double distance = std::sqrt(lineDx * lineDx + lineDy * lineDy);

    const double spacing = std::max(1.0, brushSize * 0.12);
    const int steps = std::max(1, static_cast<int>(std::ceil(distance / spacing)));

    const double taperRange = 0.08;

    for (int i = 0; i <= steps; ++i)
    {
        const double t = static_cast<double>(i) / static_cast<double>(steps);

        double taper = 1.0;

        if (steps > 1)
        {
            if (t < taperRange)
            {
                taper = t / taperRange;
                taper = taper * taper * (3.0 - 2.0 * taper);
            }
            else if (t > 1.0 - taperRange)
            {
                taper = (1.0 - t) / taperRange;
                taper = taper * taper * (3.0 - 2.0 * taper);
            }
        }

        const int x = static_cast<int>(std::lround(startLocal.x + lineDx * t));
        const int y = static_cast<int>(std::lround(startLocal.y + lineDy * t));

        if (drawBrushDab(x, y, taper))
            anyChanged = true;
    }

    if (!anyChanged)
        return false;

    InvalidateDisplayCache();

    if (hasDirtyRect)
    {
        wxRect dirtyDocRect(dirtyLocalRect.x + layer.offsetX, dirtyLocalRect.y + layer.offsetY, dirtyLocalRect.width, dirtyLocalRect.height);
        RequestCanvasRefreshRect(dirtyDocRect);
    }
    else
    {
        RequestCanvasRefresh();
    }

    return true;
}

bool Document::DrawCloneStampOnSelectedLayerLine(const wxPoint& startDocPoint, const wxPoint& endDocPoint, const wxPoint& sourceStartDocPoint, const wxPoint& sourceEndDocPoint, int size, int hardness, int opacity, int flow, const wxImage* sourceImage, std::vector<double>* strokeMask, const std::vector<BrushDabPixel>* cachedDab)
{
    if (!CanDrawOnSelectedLayer())
        return false;

    if (!sourceImage || !sourceImage->IsOk())
        return false;

    DocumentLayer& layer = m_layers[m_selectedLayer];
    wxImage& image = layer.image;

    if (!image.IsOk())
        return false;

    EnsureAlpha(image);

    const int w = image.GetWidth();
    const int h = image.GetHeight();

    const int sw = sourceImage->GetWidth();
    const int sh = sourceImage->GetHeight();

    if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0)
        return false;

    unsigned char* dstData = image.GetData();
    unsigned char* dstAlpha = image.GetAlpha();

    const unsigned char* srcData = sourceImage->GetData();
    const unsigned char* srcAlpha = sourceImage->HasAlpha() ? sourceImage->GetAlpha() : nullptr;

    if (!dstData || !dstAlpha || !srcData)
        return false;

    const int brushSizeInt = std::max(1, size);
    const int half = brushSizeInt / 2;

    const double brushSize = static_cast<double>(brushSizeInt);
    const double radius = brushSize * 0.5;
    const double radiusSq = radius * radius;

    const int hardnessClamped = std::max(0, std::min(100, hardness));
    const int opacityClamped = std::max(0, std::min(100, opacity));
    const int flowClamped = std::max(0, std::min(100, flow));

    const double hard01 = static_cast<double>(hardnessClamped) / 100.0;
    const double hardRadius = radius * (0.03 + hard01 * 0.45);
    const double softRange = std::max(1.0, radius - hardRadius);

    const double opacity01 = static_cast<double>(opacityClamped) / 100.0;
    const double flow01 = static_cast<double>(flowClamped) / 100.0;

    if (opacity01 <= 0.0 || flow01 <= 0.0)
        return false;

    const bool hasStrokeMask = strokeMask && strokeMask->size() == static_cast<size_t>(w) * static_cast<size_t>(h);

    const wxPoint startLocal(startDocPoint.x - layer.offsetX, startDocPoint.y - layer.offsetY);
    const wxPoint endLocal(endDocPoint.x - layer.offsetX, endDocPoint.y - layer.offsetY);

    const wxPoint sourceStartLocal = sourceStartDocPoint;
    const wxPoint sourceEndLocal = sourceEndDocPoint;

    std::vector<BrushDabPixel> localDabPixels;
    const std::vector<BrushDabPixel>* dabPixels = cachedDab;

    if (!dabPixels || dabPixels->empty())
    {
        localDabPixels.reserve(static_cast<size_t>(brushSizeInt) * static_cast<size_t>(brushSizeInt));

        const double center = brushSize * 0.5;

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
                localDabPixels.push_back(p);
            }
        }

        dabPixels = &localDabPixels;
    }

    if (!dabPixels || dabPixels->empty())
        return false;

    bool hasDirtyRect = false;
    wxRect dirtyLocalRect;

    auto addDirtyLocalRect = [&](const wxRect& rect)
    {
        if (rect.IsEmpty())
            return;

        if (!hasDirtyRect)
        {
            dirtyLocalRect = rect;
            hasDirtyRect = true;
        }
        else
        {
            dirtyLocalRect.Union(rect);
        }
    };

    auto drawDab = [&](int dstCx, int dstCy, int srcCx, int srcCy) -> bool
    {
        bool changed = false;

        int dirtyLeft = w;
        int dirtyTop = h;
        int dirtyRight = -1;
        int dirtyBottom = -1;

        for (const BrushDabPixel& p : *dabPixels)
        {
            const int dx = dstCx + p.ox;
            const int dy = dstCy + p.oy;
            const int sx = srcCx + p.ox;
            const int sy = srcCy + p.oy;

            if (dx < 0 || dy < 0 || dx >= w || dy >= h)
                continue;

            if (sx < 0 || sy < 0 || sx >= sw || sy >= sh)
                continue;

            const int dstIndex = dy * w + dx;
            const int dstRgb = dstIndex * 3;

            const int srcIndex = sy * sw + sx;
            const int srcRgb = srcIndex * 3;

            const double srcA = srcAlpha ? static_cast<double>(srcAlpha[srcIndex]) / 255.0 : 1.0;

            if (srcA <= 0.0)
                continue;

            const double stampAmount = p.strength * opacity01 * flow01;

            if (stampAmount <= 0.0)
                continue;

            double blendAmount = stampAmount;

            if (hasStrokeMask)
            {
                double& used = (*strokeMask)[static_cast<size_t>(dstIndex)];
                const double target = std::min(opacity01, used + (1.0 - used) * stampAmount);
                blendAmount = std::max(0.0, target - used);
                used = target;
            }

            if (blendAmount <= 0.0)
                continue;

            const double dstR = static_cast<double>(dstData[dstRgb + 0]) / 255.0;
            const double dstG = static_cast<double>(dstData[dstRgb + 1]) / 255.0;
            const double dstB = static_cast<double>(dstData[dstRgb + 2]) / 255.0;
            const double dstA = static_cast<double>(dstAlpha[dstIndex]) / 255.0;

            const double srcR = static_cast<double>(srcData[srcRgb + 0]) / 255.0;
            const double srcG = static_cast<double>(srcData[srcRgb + 1]) / 255.0;
            const double srcB = static_cast<double>(srcData[srcRgb + 2]) / 255.0;

            const double effectiveSrcA = srcA * blendAmount;
            const double outA = effectiveSrcA + dstA * (1.0 - effectiveSrcA);

            double outR = srcR;
            double outG = srcG;
            double outB = srcB;

            if (outA > 0.0)
            {
                outR = (srcR * effectiveSrcA + dstR * dstA * (1.0 - effectiveSrcA)) / outA;
                outG = (srcG * effectiveSrcA + dstG * dstA * (1.0 - effectiveSrcA)) / outA;
                outB = (srcB * effectiveSrcA + dstB * dstA * (1.0 - effectiveSrcA)) / outA;
            }

            dstData[dstRgb + 0] = static_cast<unsigned char>(std::clamp(outR, 0.0, 1.0) * 255.0 + 0.5);
            dstData[dstRgb + 1] = static_cast<unsigned char>(std::clamp(outG, 0.0, 1.0) * 255.0 + 0.5);
            dstData[dstRgb + 2] = static_cast<unsigned char>(std::clamp(outB, 0.0, 1.0) * 255.0 + 0.5);
            dstAlpha[dstIndex] = static_cast<unsigned char>(std::clamp(outA, 0.0, 1.0) * 255.0 + 0.5);

            dirtyLeft = std::min(dirtyLeft, dx);
            dirtyTop = std::min(dirtyTop, dy);
            dirtyRight = std::max(dirtyRight, dx);
            dirtyBottom = std::max(dirtyBottom, dy);

            changed = true;
        }

        if (changed && dirtyRight >= dirtyLeft && dirtyBottom >= dirtyTop)
            addDirtyLocalRect(wxRect(dirtyLeft, dirtyTop, dirtyRight - dirtyLeft + 1, dirtyBottom - dirtyTop + 1));

        return changed;
    };

    bool anyChanged = false;

    const double lineDx = static_cast<double>(endLocal.x - startLocal.x);
    const double lineDy = static_cast<double>(endLocal.y - startLocal.y);
    const double distance = std::sqrt(lineDx * lineDx + lineDy * lineDy);

    const double spacing = std::max(1.0, brushSize * 0.12);
    const int steps = std::max(1, static_cast<int>(std::ceil(distance / spacing)));

    for (int i = 0; i <= steps; ++i)
    {
        const double t = static_cast<double>(i) / static_cast<double>(steps);

        const int dstX = static_cast<int>(std::lround(static_cast<double>(startLocal.x) + lineDx * t));
        const int dstY = static_cast<int>(std::lround(static_cast<double>(startLocal.y) + lineDy * t));

        const int srcX = static_cast<int>(std::lround(static_cast<double>(sourceStartLocal.x) + static_cast<double>(sourceEndLocal.x - sourceStartLocal.x) * t));
        const int srcY = static_cast<int>(std::lround(static_cast<double>(sourceStartLocal.y) + static_cast<double>(sourceEndLocal.y - sourceStartLocal.y) * t));

        if (drawDab(dstX, dstY, srcX, srcY))
            anyChanged = true;
    }

    if (!anyChanged)
        return false;

    InvalidateDisplayCache();

    if (hasDirtyRect)
    {
        wxRect dirtyDocRect(dirtyLocalRect.x + layer.offsetX, dirtyLocalRect.y + layer.offsetY, dirtyLocalRect.width, dirtyLocalRect.height);

        ExpandLayerPatchHistoryRect(dirtyDocRect);
        RequestCanvasRefreshRect(dirtyDocRect);
    }
    else
    {
        RequestCanvasRefresh();
    }

    return true;
}

bool Document::GetSnapToDocumentBounds() const
{
    return m_snapToDocumentBounds;
}

void Document::SetSnapToDocumentBounds(bool enabled)
{
    m_snapToDocumentBounds = enabled;
}

wxPoint Document::GetSnappedLayerOffset(int layerIndex, int offsetX, int offsetY) const
{
    if (layerIndex < 0 || layerIndex >= static_cast<int>(m_layers.size()))
        return wxPoint(offsetX, offsetY);

    if (!m_snapToDocumentBounds)
        return wxPoint(offsetX, offsetY);

    const DocumentLayer& layer = m_layers[layerIndex];
    if (!layer.image.IsOk())
        return wxPoint(offsetX, offsetY);

    const int imageW = layer.image.GetWidth();
    const int imageH = layer.image.GetHeight();

    const int snapDistance = 7;

    int snappedX = offsetX;
    int snappedY = offsetY;

    if (std::abs(offsetX) <= snapDistance)
        snappedX = 0;

    if (std::abs(offsetY) <= snapDistance)
        snappedY = 0;

    const int rightEdgeOffset = m_pageWidth - imageW;
    if (std::abs(offsetX - rightEdgeOffset) <= snapDistance)
        snappedX = rightEdgeOffset;

    const int bottomEdgeOffset = m_pageHeight - imageH;
    if (std::abs(offsetY - bottomEdgeOffset) <= snapDistance)
        snappedY = bottomEdgeOffset;

    return wxPoint(snappedX, snappedY);
}

wxPoint Document::GetSelectedLayerSnappedOffset(int offsetX, int offsetY) const
{
    return GetSnappedLayerOffset(m_selectedLayer, offsetX, offsetY);
}

bool Document::CopySelectionFromSelectedLayer(const wxRect& docRect, wxImage& outImage, const std::vector<unsigned char>* selectionMask) const
{
    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return false;

    const DocumentLayer& layer = m_layers[m_selectedLayer];

    if (!layer.image.IsOk())
        return false;

    if (docRect.width <= 0 || docRect.height <= 0)
        return false;

    if (selectionMask && selectionMask->size() != static_cast<size_t>(docRect.width) * static_cast<size_t>(docRect.height))
        return false;

    outImage = wxImage(docRect.width, docRect.height, true);

    if (!outImage.IsOk())
        return false;

    if (!outImage.HasAlpha())
        outImage.InitAlpha();

    unsigned char* outData = outImage.GetData();
    unsigned char* outAlpha = outImage.GetAlpha();

    if (!outData || !outAlpha)
        return false;

    std::fill(outData, outData + static_cast<size_t>(docRect.width) * static_cast<size_t>(docRect.height) * 3u, 0);
    std::fill(outAlpha, outAlpha + static_cast<size_t>(docRect.width) * static_cast<size_t>(docRect.height), 0);

    const unsigned char* srcData = layer.image.GetData();
    const unsigned char* srcAlpha = layer.image.HasAlpha() ? layer.image.GetAlpha() : nullptr;

    if (!srcData)
        return false;

    const int srcW = layer.image.GetWidth();
    const int srcH = layer.image.GetHeight();

    bool copiedAny = false;

    for (int y = 0; y < docRect.height; ++y)
    {
        for (int x = 0; x < docRect.width; ++x)
        {
            const size_t maskIndex = static_cast<size_t>(y) * static_cast<size_t>(docRect.width) + static_cast<size_t>(x);

            if (selectionMask && !(*selectionMask)[maskIndex])
                continue;

            const int docX = docRect.x + x;
            const int docY = docRect.y + y;

            const int srcX = docX - layer.offsetX;
            const int srcY = docY - layer.offsetY;

            if (srcX < 0 || srcY < 0 || srcX >= srcW || srcY >= srcH)
                continue;

            const int srcIndex = srcY * srcW + srcX;
            const int dstIndex = y * docRect.width + x;

            outData[dstIndex * 3 + 0] = srcData[srcIndex * 3 + 0];
            outData[dstIndex * 3 + 1] = srcData[srcIndex * 3 + 1];
            outData[dstIndex * 3 + 2] = srcData[srcIndex * 3 + 2];
            outAlpha[dstIndex] = srcAlpha ? srcAlpha[srcIndex] : 255;

            copiedAny = true;
        }
    }

    return copiedAny;
}

bool Document::DeleteSelectionFromSelectedLayer(const wxRect& docRect, const std::vector<unsigned char>* selectionMask)
{
    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return false;

    DocumentLayer& layer = m_layers[m_selectedLayer];

    if (!layer.image.IsOk())
        return false;

    if (docRect.width <= 0 || docRect.height <= 0)
        return false;

    if (selectionMask && selectionMask->size() != static_cast<size_t>(docRect.width) * static_cast<size_t>(docRect.height))
        return false;

    if (!layer.image.HasAlpha())
        layer.image.InitAlpha();

    unsigned char* alpha = layer.image.GetAlpha();

    if (!alpha)
        return false;

    const int imageW = layer.image.GetWidth();
    const int imageH = layer.image.GetHeight();

    bool changed = false;

    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    bool hasDirty = false;

    PushHistorySnapshot("Delete Selection");

    for (int y = 0; y < docRect.height; ++y)
    {
        for (int x = 0; x < docRect.width; ++x)
        {
            const size_t maskIndex = static_cast<size_t>(y) * static_cast<size_t>(docRect.width) + static_cast<size_t>(x);

            if (selectionMask && !(*selectionMask)[maskIndex])
                continue;

            const int docX = docRect.x + x;
            const int docY = docRect.y + y;

            const int localX = docX - layer.offsetX;
            const int localY = docY - layer.offsetY;

            if (localX < 0 || localY < 0 || localX >= imageW || localY >= imageH)
                continue;

            const int pixelIndex = localY * imageW + localX;

            if (alpha[pixelIndex] == 0)
                continue;

            alpha[pixelIndex] = 0;
            changed = true;

            if (!hasDirty)
            {
                minX = maxX = docX;
                minY = maxY = docY;
                hasDirty = true;
            }
            else
            {
                minX = std::min(minX, docX);
                minY = std::min(minY, docY);
                maxX = std::max(maxX, docX);
                maxY = std::max(maxY, docY);
            }
        }
    }

    if (!changed)
        return false;

    MarkDisplayCacheDirty();
    NotifyDocumentChanged();

    if (hasDirty)
        RequestCanvasRefreshRect(wxRect(minX, minY, maxX - minX + 1, maxY - minY + 1));
    else
        RequestCanvasRefresh();

    return true;
}

bool Document::FillSelectionOnSelectedLayer(const wxRect& docRect, const wxColour& color, const std::vector<unsigned char>* selectionMask)
{
    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return false;

    DocumentLayer& layer = m_layers[m_selectedLayer];

    if (!layer.image.IsOk())
        return false;

    if (docRect.width <= 0 || docRect.height <= 0)
        return false;

    if (selectionMask && selectionMask->size() != static_cast<size_t>(docRect.width) * static_cast<size_t>(docRect.height))
        return false;

    if (!layer.image.HasAlpha())
        layer.image.InitAlpha();

    unsigned char* data = layer.image.GetData();
    unsigned char* alpha = layer.image.GetAlpha();

    if (!data || !alpha)
        return false;

    const int imageW = layer.image.GetWidth();
    const int imageH = layer.image.GetHeight();

    const unsigned char r = static_cast<unsigned char>(color.Red());
    const unsigned char g = static_cast<unsigned char>(color.Green());
    const unsigned char b = static_cast<unsigned char>(color.Blue());
    const unsigned char a = 255;

    bool changed = false;

    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    bool hasDirty = false;

    PushHistorySnapshot("Fill Selection");

    for (int y = 0; y < docRect.height; ++y)
    {
        for (int x = 0; x < docRect.width; ++x)
        {
            const size_t maskIndex = static_cast<size_t>(y) * static_cast<size_t>(docRect.width) + static_cast<size_t>(x);

            if (selectionMask && !(*selectionMask)[maskIndex])
                continue;

            const int docX = docRect.x + x;
            const int docY = docRect.y + y;

            const int localX = docX - layer.offsetX;
            const int localY = docY - layer.offsetY;

            if (localX < 0 || localY < 0 || localX >= imageW || localY >= imageH)
                continue;

            const int pixelIndex = localY * imageW + localX;
            const int rgbIndex = pixelIndex * 3;

            if (data[rgbIndex + 0] == r && data[rgbIndex + 1] == g && data[rgbIndex + 2] == b && alpha[pixelIndex] == a)
                continue;

            data[rgbIndex + 0] = r;
            data[rgbIndex + 1] = g;
            data[rgbIndex + 2] = b;
            alpha[pixelIndex] = a;

            changed = true;

            if (!hasDirty)
            {
                minX = maxX = docX;
                minY = maxY = docY;
                hasDirty = true;
            }
            else
            {
                minX = std::min(minX, docX);
                minY = std::min(minY, docY);
                maxX = std::max(maxX, docX);
                maxY = std::max(maxY, docY);
            }
        }
    }

    if (!changed)
        return false;

    MarkDisplayCacheDirty();
    NotifyDocumentChanged();

    if (hasDirty)
        RequestCanvasRefreshRect(wxRect(minX, minY, maxX - minX + 1, maxY - minY + 1));
    else
        RequestCanvasRefresh();

    return true;
}

bool Document::BlendSelectionOnSelectedLayer(const wxRect& docRect, const wxColour& color, int opacity, const std::vector<unsigned char>* selectionMask)
{
    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return false;

    DocumentLayer& layer = m_layers[m_selectedLayer];

    if (!layer.image.IsOk())
        return false;

    if (docRect.width <= 0 || docRect.height <= 0)
        return false;

    if (selectionMask && selectionMask->size() != static_cast<size_t>(docRect.width) * static_cast<size_t>(docRect.height))
        return false;

    const int clampedOpacity = std::max(0, std::min(100, opacity));

    if (clampedOpacity <= 0)
        return false;

    if (clampedOpacity >= 100)
        return FillSelectionOnSelectedLayer(docRect, color, selectionMask);

    if (!layer.image.HasAlpha())
        layer.image.InitAlpha();

    unsigned char* data = layer.image.GetData();
    unsigned char* alpha = layer.image.GetAlpha();

    if (!data || !alpha)
        return false;

    const int imageW = layer.image.GetWidth();
    const int imageH = layer.image.GetHeight();

    const double srcOpacity = static_cast<double>(clampedOpacity) / 100.0;
    const double srcR = static_cast<double>(color.Red());
    const double srcG = static_cast<double>(color.Green());
    const double srcB = static_cast<double>(color.Blue());

    bool changed = false;

    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    bool hasDirty = false;

    PushHistorySnapshot("Stroke Selection");

    for (int y = 0; y < docRect.height; ++y)
    {
        for (int x = 0; x < docRect.width; ++x)
        {
            const size_t maskIndex = static_cast<size_t>(y) * static_cast<size_t>(docRect.width) + static_cast<size_t>(x);

            if (selectionMask && !(*selectionMask)[maskIndex])
                continue;

            const int docX = docRect.x + x;
            const int docY = docRect.y + y;

            const int localX = docX - layer.offsetX;
            const int localY = docY - layer.offsetY;

            if (localX < 0 || localY < 0 || localX >= imageW || localY >= imageH)
                continue;

            const int pixelIndex = localY * imageW + localX;
            const int rgbIndex = pixelIndex * 3;

            const double dstA = static_cast<double>(alpha[pixelIndex]) / 255.0;
            const double outA = srcOpacity + dstA * (1.0 - srcOpacity);

            if (outA <= 0.0)
                continue;

            const double dstR = static_cast<double>(data[rgbIndex + 0]);
            const double dstG = static_cast<double>(data[rgbIndex + 1]);
            const double dstB = static_cast<double>(data[rgbIndex + 2]);

            const unsigned char outR = static_cast<unsigned char>(std::clamp((srcR * srcOpacity + dstR * dstA * (1.0 - srcOpacity)) / outA, 0.0, 255.0) + 0.5);
            const unsigned char outG = static_cast<unsigned char>(std::clamp((srcG * srcOpacity + dstG * dstA * (1.0 - srcOpacity)) / outA, 0.0, 255.0) + 0.5);
            const unsigned char outB = static_cast<unsigned char>(std::clamp((srcB * srcOpacity + dstB * dstA * (1.0 - srcOpacity)) / outA, 0.0, 255.0) + 0.5);
            const unsigned char outAlpha = static_cast<unsigned char>(std::clamp(outA * 255.0, 0.0, 255.0) + 0.5);

            if (data[rgbIndex + 0] == outR && data[rgbIndex + 1] == outG && data[rgbIndex + 2] == outB && alpha[pixelIndex] == outAlpha)
                continue;

            data[rgbIndex + 0] = outR;
            data[rgbIndex + 1] = outG;
            data[rgbIndex + 2] = outB;
            alpha[pixelIndex] = outAlpha;

            changed = true;

            if (!hasDirty)
            {
                minX = maxX = docX;
                minY = maxY = docY;
                hasDirty = true;
            }
            else
            {
                minX = std::min(minX, docX);
                minY = std::min(minY, docY);
                maxX = std::max(maxX, docX);
                maxY = std::max(maxY, docY);
            }
        }
    }

    if (!changed)
        return false;

    MarkDisplayCacheDirty();
    NotifyDocumentChanged();

    if (hasDirty)
        RequestCanvasRefreshRect(wxRect(minX, minY, maxX - minX + 1, maxY - minY + 1));
    else
        RequestCanvasRefresh();

    return true;
}

bool Document::BucketFillSelectedLayerAt(const wxPoint& docPoint, const wxColour& color, int tolerance, bool antiAlias, bool contiguous, bool sampleAllLayers, const wxRect* limitDocRect, const std::vector<unsigned char>* selectionMask)
{
    if (!CanDrawOnSelectedLayer())
        return false;

    DocumentLayer& layer = m_layers[m_selectedLayer];

    if (!layer.image.IsOk())
        return false;

    if (!layer.image.HasAlpha())
        layer.image.InitAlpha();

    unsigned char* data = layer.image.GetData();
    unsigned char* alpha = layer.image.GetAlpha();

    if (!data || !alpha)
        return false;

    const int w = layer.image.GetWidth();
    const int h = layer.image.GetHeight();

    wxRect fillLimitDoc(0, 0, m_pageWidth, m_pageHeight);

    if (limitDocRect && !limitDocRect->IsEmpty())
        fillLimitDoc = *limitDocRect;

    if (selectionMask && selectionMask->size() != static_cast<size_t>(fillLimitDoc.width) * static_cast<size_t>(fillLimitDoc.height))
        return false;

    wxRect layerDocRect(layer.offsetX, layer.offsetY, w, h);
    wxRect clippedDocRect = fillLimitDoc.Intersect(layerDocRect);

    if (clippedDocRect.IsEmpty())
        return false;

    if (!clippedDocRect.Contains(docPoint))
        return false;

    const int startX = docPoint.x - layer.offsetX;
    const int startY = docPoint.y - layer.offsetY;

    if (startX < 0 || startY < 0 || startX >= w || startY >= h)
        return false;

    const int clampedTolerance = std::max(0, std::min(255, tolerance));

    wxImage sampleImage = sampleAllLayers ? CopyVisibleMergedImage() : layer.image;
    if (!sampleImage.IsOk())
        return false;

    if (!sampleImage.HasAlpha())
        sampleImage.InitAlpha();

    const unsigned char* sampleData = sampleImage.GetData();
    const unsigned char* sampleAlpha = sampleImage.GetAlpha();

    if (!sampleData || !sampleAlpha)
        return false;

    auto getSamplePixel = [&](int docX, int docY, int& r, int& g, int& b, int& a) -> bool
    {
        int sx = docX;
        int sy = docY;

        if (!sampleAllLayers)
        {
            sx = docX - layer.offsetX;
            sy = docY - layer.offsetY;
        }

        if (sx < 0 || sy < 0 || sx >= sampleImage.GetWidth() || sy >= sampleImage.GetHeight())
            return false;

        const int pi = sy * sampleImage.GetWidth() + sx;
        const int ri = pi * 3;

        r = sampleData[ri + 0];
        g = sampleData[ri + 1];
        b = sampleData[ri + 2];
        a = sampleAlpha[pi];

        return true;
    };

    int targetR = 0;
    int targetG = 0;
    int targetB = 0;
    int targetA = 0;

    if (!getSamplePixel(docPoint.x, docPoint.y, targetR, targetG, targetB, targetA))
        return false;

    auto inSelection = [&](int docX, int docY) -> bool
    {
        if (!fillLimitDoc.Contains(wxPoint(docX, docY)))
            return false;

        if (!selectionMask)
            return true;

        const int mx = docX - fillLimitDoc.x;
        const int my = docY - fillLimitDoc.y;

        if (mx < 0 || my < 0 || mx >= fillLimitDoc.width || my >= fillLimitDoc.height)
            return false;

        const size_t index = static_cast<size_t>(my) * static_cast<size_t>(fillLimitDoc.width) + static_cast<size_t>(mx);
        return index < selectionMask->size() && (*selectionMask)[index] > 0;
    };

    auto colorMatches = [&](int docX, int docY) -> bool
    {
        int r = 0;
        int g = 0;
        int b = 0;
        int a = 0;

        if (!getSamplePixel(docX, docY, r, g, b, a))
            return false;

        const int dr = r - targetR;
        const int dg = g - targetG;
        const int db = b - targetB;
        const int da = a - targetA;

        const int distanceSq = dr * dr + dg * dg + db * db + da * da;
        const int toleranceSq = clampedTolerance * clampedTolerance;

        return distanceSq <= toleranceSq;
    };

    std::vector<unsigned char> fillMask(
        static_cast<size_t>(fillLimitDoc.width) * static_cast<size_t>(fillLimitDoc.height),
        0
    );

    if (contiguous)
    {
        std::vector<unsigned char> visited(
            static_cast<size_t>(fillLimitDoc.width) * static_cast<size_t>(fillLimitDoc.height),
            0
        );

        auto localIndex = [&](int docX, int docY) -> size_t
        {
            const int lx = docX - fillLimitDoc.x;
            const int ly = docY - fillLimitDoc.y;
            return static_cast<size_t>(ly) * static_cast<size_t>(fillLimitDoc.width) + static_cast<size_t>(lx);
        };

        if (!inSelection(docPoint.x, docPoint.y) || !colorMatches(docPoint.x, docPoint.y))
            return false;

        std::vector<wxPoint> stack;
        stack.push_back(docPoint);

        while (!stack.empty())
        {
            const wxPoint p = stack.back();
            stack.pop_back();

            if (!inSelection(p.x, p.y))
                continue;

            const size_t idx = localIndex(p.x, p.y);

            if (visited[idx])
                continue;

            visited[idx] = 1;

            if (!colorMatches(p.x, p.y))
                continue;

            fillMask[idx] = 255;

            stack.push_back(wxPoint(p.x + 1, p.y));
            stack.push_back(wxPoint(p.x - 1, p.y));
            stack.push_back(wxPoint(p.x, p.y + 1));
            stack.push_back(wxPoint(p.x, p.y - 1));
        }
    }
    else
    {
        for (int yDoc = fillLimitDoc.y; yDoc < fillLimitDoc.y + fillLimitDoc.height; ++yDoc)
        {
            for (int xDoc = fillLimitDoc.x; xDoc < fillLimitDoc.x + fillLimitDoc.width; ++xDoc)
            {
                if (!inSelection(xDoc, yDoc))
                    continue;

                if (!colorMatches(xDoc, yDoc))
                    continue;

                const int lx = xDoc - fillLimitDoc.x;
                const int ly = yDoc - fillLimitDoc.y;

                fillMask[static_cast<size_t>(ly) * static_cast<size_t>(fillLimitDoc.width) + static_cast<size_t>(lx)] = 255;
            }
        }
    }

    if (antiAlias)
    {
        std::vector<unsigned char> aaMask = fillMask;

        for (int y = 0; y < fillLimitDoc.height; ++y)
        {
            for (int x = 0; x < fillLimitDoc.width; ++x)
            {
                const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(fillLimitDoc.width) + static_cast<size_t>(x);

                if (fillMask[idx])
                    continue;

                int neighborCount = 0;

                for (int yy = -1; yy <= 1; ++yy)
                {
                    for (int xx = -1; xx <= 1; ++xx)
                    {
                        if (xx == 0 && yy == 0)
                            continue;

                        const int nx = x + xx;
                        const int ny = y + yy;

                        if (nx < 0 || ny < 0 || nx >= fillLimitDoc.width || ny >= fillLimitDoc.height)
                            continue;

                        const size_t nidx = static_cast<size_t>(ny) * static_cast<size_t>(fillLimitDoc.width) + static_cast<size_t>(nx);

                        if (fillMask[nidx])
                            ++neighborCount;
                    }
                }

                if (neighborCount > 0 && inSelection(fillLimitDoc.x + x, fillLimitDoc.y + y) && colorMatches(fillLimitDoc.x + x, fillLimitDoc.y + y))
                    aaMask[idx] = static_cast<unsigned char>(std::min(255, neighborCount * 32));
            }
        }

        fillMask.swap(aaMask);
    }

    bool hasFill = false;

    for (unsigned char v : fillMask)
    {
        if (v)
        {
            hasFill = true;
            break;
        }
    }

    if (!hasFill)
        return false;

    PushHistorySnapshot("Bucket Fill");

    bool changed = false;

    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    bool hasDirty = false;

    const unsigned char r = color.Red();
    const unsigned char g = color.Green();
    const unsigned char b = color.Blue();

    for (int y = 0; y < fillLimitDoc.height; ++y)
    {
        for (int x = 0; x < fillLimitDoc.width; ++x)
        {
            const size_t maskIndex = static_cast<size_t>(y) * static_cast<size_t>(fillLimitDoc.width) + static_cast<size_t>(x);
            const unsigned char maskValue = fillMask[maskIndex];

            if (!maskValue)
                continue;

            const int docX = fillLimitDoc.x + x;
            const int docY = fillLimitDoc.y + y;

            const int localX = docX - layer.offsetX;
            const int localY = docY - layer.offsetY;

            if (localX < 0 || localY < 0 || localX >= w || localY >= h)
                continue;

            const int pixelIndex = localY * w + localX;
            const int rgbIndex = pixelIndex * 3;

            const double amount = static_cast<double>(maskValue) / 255.0;

            const unsigned char outR = static_cast<unsigned char>(std::clamp(static_cast<double>(data[rgbIndex + 0]) + (static_cast<double>(r) - static_cast<double>(data[rgbIndex + 0])) * amount, 0.0, 255.0) + 0.5);
            const unsigned char outG = static_cast<unsigned char>(std::clamp(static_cast<double>(data[rgbIndex + 1]) + (static_cast<double>(g) - static_cast<double>(data[rgbIndex + 1])) * amount, 0.0, 255.0) + 0.5);
            const unsigned char outB = static_cast<unsigned char>(std::clamp(static_cast<double>(data[rgbIndex + 2]) + (static_cast<double>(b) - static_cast<double>(data[rgbIndex + 2])) * amount, 0.0, 255.0) + 0.5);
            const unsigned char outA = static_cast<unsigned char>(std::clamp(static_cast<double>(alpha[pixelIndex]) + (255.0 - static_cast<double>(alpha[pixelIndex])) * amount, 0.0, 255.0) + 0.5);

            if (data[rgbIndex + 0] == outR && data[rgbIndex + 1] == outG && data[rgbIndex + 2] == outB && alpha[pixelIndex] == outA)
                continue;

            data[rgbIndex + 0] = outR;
            data[rgbIndex + 1] = outG;
            data[rgbIndex + 2] = outB;
            alpha[pixelIndex] = outA;

            changed = true;

            if (!hasDirty)
            {
                minX = maxX = docX;
                minY = maxY = docY;
                hasDirty = true;
            }
            else
            {
                minX = std::min(minX, docX);
                minY = std::min(minY, docY);
                maxX = std::max(maxX, docX);
                maxY = std::max(maxY, docY);
            }
        }
    }

    if (!changed)
        return false;

    MarkDisplayCacheDirty();
    NotifyDocumentChanged();

    if (hasDirty)
        RequestCanvasRefreshRect(wxRect(minX, minY, maxX - minX + 1, maxY - minY + 1));
    else
        RequestCanvasRefresh();

    return true;
}

bool Document::ApplyLinearGradientToSelectedLayer(const wxPoint& startDocPoint, const wxPoint& endDocPoint, const GradientData& gradient, const wxRect* limitDocRect, const std::vector<unsigned char>* selectionMask)
{
    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return false;

    DocumentLayer& layer = m_layers[m_selectedLayer];

    if (layer.locked || !layer.image.IsOk())
        return false;

    const double dx = static_cast<double>(endDocPoint.x - startDocPoint.x);
    const double dy = static_cast<double>(endDocPoint.y - startDocPoint.y);
    const double lenSq = dx * dx + dy * dy;

    if (lenSq <= 0.0001)
        return false;

    wxRect workRect;

    if (limitDocRect && !limitDocRect->IsEmpty())
        workRect = *limitDocRect;
    else
        workRect = wxRect(0, 0, m_pageWidth, m_pageHeight);

    workRect = workRect.Intersect(wxRect(0, 0, m_pageWidth, m_pageHeight));

    if (workRect.IsEmpty())
        return false;

    const wxRect layerDocRect(layer.offsetX, layer.offsetY, layer.image.GetWidth(), layer.image.GetHeight());
    workRect = workRect.Intersect(layerDocRect);

    if (workRect.IsEmpty())
        return false;

    unsigned char* data = layer.image.GetData();

    if (!data)
        return false;

    EnsureAlpha(layer.image);

    unsigned char* alpha = layer.image.GetAlpha();

    if (!alpha)
        return false;

    PushHistorySnapshot("Gradient");

    bool changed = false;

    for (int docY = workRect.y; docY < workRect.y + workRect.height; ++docY)
    {
        for (int docX = workRect.x; docX < workRect.x + workRect.width; ++docX)
        {
            if (selectionMask && limitDocRect)
            {
                const int maskX = docX - limitDocRect->x;
                const int maskY = docY - limitDocRect->y;

                if (maskX < 0 || maskY < 0 || maskX >= limitDocRect->width || maskY >= limitDocRect->height)
                    continue;

                const size_t maskIndex = static_cast<size_t>(maskY) * static_cast<size_t>(limitDocRect->width) + static_cast<size_t>(maskX);

                if (maskIndex >= selectionMask->size() || !(*selectionMask)[maskIndex])
                    continue;
            }

            const int lx = docX - layer.offsetX;
            const int ly = docY - layer.offsetY;

            if (lx < 0 || ly < 0 || lx >= layer.image.GetWidth() || ly >= layer.image.GetHeight())
                continue;

            const double px = static_cast<double>(docX) + 0.5;
            const double py = static_cast<double>(docY) + 0.5;

            double t = ((px - static_cast<double>(startDocPoint.x)) * dx + (py - static_cast<double>(startDocPoint.y)) * dy) / lenSq;
            t = std::clamp(t, 0.0, 1.0);

            const wxColour color = gradient.Evaluate(t);

            const unsigned char r = static_cast<unsigned char>(color.Red());
            const unsigned char g = static_cast<unsigned char>(color.Green());
            const unsigned char b = static_cast<unsigned char>(color.Blue());

            const int pixelIndex = ly * layer.image.GetWidth() + lx;
            const int rgbIndex = pixelIndex * 3;

            if (data[rgbIndex + 0] == r && data[rgbIndex + 1] == g && data[rgbIndex + 2] == b && alpha[pixelIndex] == 255)
                continue;

            data[rgbIndex + 0] = r;
            data[rgbIndex + 1] = g;
            data[rgbIndex + 2] = b;
            alpha[pixelIndex] = 255;

            changed = true;
        }
    }

    if (!changed)
        return false;

    MarkDisplayCacheDirty();
    NotifyDocumentChanged();
    RequestCanvasRefresh();

    return true;
}

bool Document::CutSelectionFromSelectedLayer(const wxRect& docRect, wxImage& outImage, const std::vector<unsigned char>* selectionMask)
{
    BeginHistoryTransaction("Cut Selection");

    if (!CopySelectionFromSelectedLayer(docRect, outImage, selectionMask))
    {
        EndHistoryTransaction();
        return false;
    }

    const bool deleted = DeleteSelectionFromSelectedLayer(docRect, selectionMask);

    EndHistoryTransaction();
    return deleted;
}

bool Document::InsertLayerAboveCurrent(const wxImage& image, const wxString& layerName)
{
    if (!image.IsOk())
        return false;

    PushHistorySnapshot("Insert Layer");

    DocumentLayer layer;
    layer.name = layerName;
    layer.visible = true;
    layer.locked = false;
    layer.offsetX = 0;
    layer.offsetY = 0;
    layer.image = image;

    int insertIndex = static_cast<int>(m_layers.size());
    if (m_selectedLayer >= 0 && m_selectedLayer < static_cast<int>(m_layers.size()))
        insertIndex = m_selectedLayer + 1;

    m_layers.insert(m_layers.begin() + insertIndex, layer);
    m_selectedLayer = insertIndex;

    MarkDisplayCacheDirty();
    NotifyDocumentChanged();
    return true;
}

bool Document::InsertTextLayerAt(const wxPoint& docPoint, const wxString& text, const wxFont& font, const wxColour& color)
{
    wxString cleanText = text;
    cleanText.Trim(true);
    cleanText.Trim(false);

    if (cleanText.IsEmpty())
        return false;

    wxBitmap measureBitmap(1, 1, 32);
    wxMemoryDC measureDC(measureBitmap);
    measureDC.SetFont(font);

    wxCoord textW = 0;
    wxCoord textH = 0;
    wxCoord descent = 0;
    wxCoord externalLeading = 0;

    measureDC.GetTextExtent(cleanText, &textW, &textH, &descent, &externalLeading);
    measureDC.SelectObject(wxNullBitmap);

    if (textW <= 0 || textH <= 0)
        return false;

    const int padding = 8;
    const int layerW = std::max(1, static_cast<int>(textW) + padding * 2);
    const int layerH = std::max(1, static_cast<int>(textH) + padding * 2);

    wxBitmap textBitmap(layerW, layerH, 32);
    wxMemoryDC dc(textBitmap);

    dc.SetBackground(wxBrush(wxColour(255, 255, 255)));
    dc.Clear();

    dc.SetFont(font);
    dc.SetTextForeground(color);
    dc.DrawText(cleanText, padding, padding);
    dc.SelectObject(wxNullBitmap);

    wxBitmap maskBitmap(layerW, layerH, 32);
    wxMemoryDC maskDC(maskBitmap);

    maskDC.SetBackground(wxBrush(wxColour(255, 255, 255)));
    maskDC.Clear();

    maskDC.SetFont(font);
    maskDC.SetTextForeground(wxColour(0, 0, 0));
    maskDC.DrawText(cleanText, padding, padding);
    maskDC.SelectObject(wxNullBitmap);

    wxImage textImage = textBitmap.ConvertToImage();
    wxImage maskImage = maskBitmap.ConvertToImage();

    if (!textImage.IsOk() || !maskImage.IsOk())
        return false;

    if (!textImage.HasAlpha())
        textImage.InitAlpha();

    unsigned char* textData = textImage.GetData();
    unsigned char* maskData = maskImage.GetData();
    unsigned char* alpha = textImage.GetAlpha();

    if (!textData || !maskData || !alpha)
        return false;

    const unsigned char r = static_cast<unsigned char>(color.Red());
    const unsigned char g = static_cast<unsigned char>(color.Green());
    const unsigned char b = static_cast<unsigned char>(color.Blue());

    for (int y = 0; y < layerH; ++y)
    {
        for (int x = 0; x < layerW; ++x)
        {
            const int index = y * layerW + x;
            const int rgb = index * 3;

            const int maskGray = (static_cast<int>(maskData[rgb + 0]) + static_cast<int>(maskData[rgb + 1]) + static_cast<int>(maskData[rgb + 2])) / 3;
            const int a = 255 - maskGray;

            alpha[index] = static_cast<unsigned char>(std::max(0, std::min(255, a)));

            if (alpha[index] == 0)
            {
                textData[rgb + 0] = 0;
                textData[rgb + 1] = 0;
                textData[rgb + 2] = 0;
            }
            else
            {
                textData[rgb + 0] = r;
                textData[rgb + 1] = g;
                textData[rgb + 2] = b;
            }
        }
    }

    BeginHistoryTransaction("Text");

    const bool inserted = InsertLayerAboveCurrent(textImage, "Text");

    if (inserted)
        SetSelectedLayerOffset(docPoint.x - padding, docPoint.y - padding);

    EndHistoryTransaction();

    if (!inserted)
        return false;

    MarkDisplayCacheDirty();
    NotifyDocumentChanged();
    RequestCanvasRefresh();

    return true;
}

bool Document::PasteOnSelectedEmptyLayer(const wxImage& image)
{
    if (!image.IsOk())
        return false;

    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return false;

    PushHistorySnapshot("Paste");

    DocumentLayer& layer = m_layers[m_selectedLayer];
    layer.image = image.Copy();
    layer.offsetX = 0;
    layer.offsetY = 0;
    layer.visible = true;

    MarkDisplayCacheDirty();
    NotifyDocumentChanged();
    return true;
}

bool Document::CropToRect(const wxRect& docRect)
{
    if (docRect.width <= 0 || docRect.height <= 0)
        return false;

    wxRect cropRect = docRect;
    cropRect.Intersect(wxRect(0, 0, m_pageWidth, m_pageHeight));

    if (cropRect.width <= 0 || cropRect.height <= 0)
        return false;

    BeginHistoryTransaction();

    for (DocumentLayer& layer : m_layers)
    {
        if (!layer.image.IsOk())
            continue;

        const wxRect layerBounds(layer.offsetX, layer.offsetY, layer.image.GetWidth(), layer.image.GetHeight());

        wxRect overlap = cropRect;
        overlap.Intersect(layerBounds);

        if (overlap.width <= 0 || overlap.height <= 0)
        {
            wxImage emptyImg(cropRect.width, cropRect.height, true);
            emptyImg.SetRGB(wxRect(0, 0, cropRect.width, cropRect.height), 255, 255, 255);

            if (!emptyImg.HasAlpha())
                emptyImg.InitAlpha();

            if (unsigned char* alpha = emptyImg.GetAlpha())
                std::fill(alpha, alpha + (cropRect.width * cropRect.height), 0);

            layer.image = emptyImg;
            layer.offsetX = 0;
            layer.offsetY = 0;
            continue;
        }

        const int srcX = overlap.x - layer.offsetX;
        const int srcY = overlap.y - layer.offsetY;
        const int srcW = overlap.width;
        const int srcH = overlap.height;

        wxImage croppedPart = layer.image.GetSubImage(wxRect(srcX, srcY, srcW, srcH));

        if (overlap.x == cropRect.x && overlap.y == cropRect.y &&
            overlap.width == cropRect.width && overlap.height == cropRect.height)
        {
            layer.image = croppedPart;
            layer.offsetX = 0;
            layer.offsetY = 0;
            continue;
        }

        wxImage newLayerImage(cropRect.width, cropRect.height, true);
        newLayerImage.SetRGB(wxRect(0, 0, cropRect.width, cropRect.height), 255, 255, 255);

        if (!newLayerImage.HasAlpha())
            newLayerImage.InitAlpha();

        unsigned char* dstData = newLayerImage.GetData();
        unsigned char* dstAlpha = newLayerImage.GetAlpha();

        if (dstAlpha)
            std::fill(dstAlpha, dstAlpha + (cropRect.width * cropRect.height), 0);

        const unsigned char* srcData = croppedPart.GetData();
        const unsigned char* srcAlpha = croppedPart.HasAlpha() ? croppedPart.GetAlpha() : nullptr;

        const int destX = overlap.x - cropRect.x;
        const int destY = overlap.y - cropRect.y;

        for (int y = 0; y < srcH; ++y)
        {
            for (int x = 0; x < srcW; ++x)
            {
                const int srcIndex = y * srcW + x;
                const int dstIndex = (destY + y) * cropRect.width + (destX + x);

                dstData[dstIndex * 3 + 0] = srcData[srcIndex * 3 + 0];
                dstData[dstIndex * 3 + 1] = srcData[srcIndex * 3 + 1];
                dstData[dstIndex * 3 + 2] = srcData[srcIndex * 3 + 2];

                if (dstAlpha)
                    dstAlpha[dstIndex] = srcAlpha ? srcAlpha[srcIndex] : 255;
            }
        }

        layer.image = newLayerImage;
        layer.offsetX = 0;
        layer.offsetY = 0;
    }

    m_pageWidth = cropRect.width;
    m_pageHeight = cropRect.height;

    MarkDisplayCacheDirty();
    UpdateViewCenter();
    SyncRulers();
    EndHistoryTransaction();
    return true;
}

bool Document::CanUndo() const
{
    return !m_undoEntryIsPatch.empty();
}

bool Document::IsModified() const
{
    if (!m_hasCleanState)
        return true;

    return !HistoryStatesEqual(m_cleanState, CaptureHistoryState());
}

void Document::MarkClean()
{
    m_cleanState = CaptureHistoryState();
    m_hasCleanState = true;
}

void Document::BeginHistoryTransaction(const wxString& label)
{
    if (m_inUndoRedo || m_historyTransactionActive)
        return;

    m_historyTransactionStartState = CaptureHistoryState();
    m_historyTransactionLabel = label;
    m_historyTransactionActive = true;
}

void Document::EndHistoryTransaction()
{
    if (!m_historyTransactionActive)
        return;

    m_historyTransactionActive = false;

    const HistoryState current = CaptureHistoryState();

    if (HistoryStatesEqual(m_historyTransactionStartState, current))
        return;

    m_undoStack.push_back(m_historyTransactionStartState);
    m_undoEntryIsPatch.push_back(false);

    m_historyLabels.push_back(
        m_historyTransactionLabel.IsEmpty() ? "Action" : m_historyTransactionLabel
    );

    while (m_undoEntryIsPatch.size() > k_maxHistoryStates)
    {
        const bool oldWasPatch = m_undoEntryIsPatch.front();
        m_undoEntryIsPatch.erase(m_undoEntryIsPatch.begin());

        if (oldWasPatch)
        {
            if (!m_undoPatchStack.empty())
                m_undoPatchStack.erase(m_undoPatchStack.begin());
        }
        else
        {
            if (!m_undoStack.empty())
                m_undoStack.erase(m_undoStack.begin());
        }

        if (!m_historyLabels.empty())
            m_historyLabels.erase(m_historyLabels.begin());
    }

    m_redoStack.clear();
    m_redoPatchStack.clear();
    m_redoEntryIsPatch.clear();
    m_redoHistoryLabels.clear();

    m_historyTransactionLabel.clear();

    NotifyDocumentChanged();
}

void Document::BeginLayerPatchHistoryTransaction(const wxString& label)
{
    if (m_inUndoRedo || m_layerPatchTransactionActive)
        return;

    if (m_selectedLayer < 0 || m_selectedLayer >= static_cast<int>(m_layers.size()))
        return;

    m_layerPatchTransaction = LayerPatchHistoryState();
    m_layerPatchTransaction.valid = true;
    m_layerPatchTransaction.layerIndex = m_selectedLayer;
    m_layerPatchTransaction.localRect = wxRect();
    m_layerPatchTransactionLabel = label;
    m_layerPatchTransactionActive = true;
}

void Document::ExpandLayerPatchHistoryRect(const wxRect& docRect)
{
    if (!m_layerPatchTransactionActive)
        return;

    if (!m_layerPatchTransaction.valid)
        return;

    if (docRect.IsEmpty())
        return;

    const int layerIndex = m_layerPatchTransaction.layerIndex;

    if (layerIndex < 0 || layerIndex >= static_cast<int>(m_layers.size()))
        return;

    const DocumentLayer& layer = m_layers[layerIndex];

    wxRect localRect(
        docRect.x - layer.offsetX,
        docRect.y - layer.offsetY,
        docRect.width,
        docRect.height
    );

    localRect.Inflate(2, 2);

    if (m_layerPatchTransaction.localRect.IsEmpty())
        m_layerPatchTransaction.localRect = localRect;
    else
        m_layerPatchTransaction.localRect.Union(localRect);
}

void Document::EndLayerPatchHistoryTransaction(const wxImage* beforeImage)
{
    if (!m_layerPatchTransactionActive)
        return;

    m_layerPatchTransactionActive = false;

    if (!m_layerPatchTransaction.valid)
        return;

    if (m_layerPatchTransaction.localRect.IsEmpty())
        return;

    if (beforeImage && beforeImage->IsOk())
    {
        const int layerIndex = m_layerPatchTransaction.layerIndex;

        if (layerIndex >= 0 && layerIndex < static_cast<int>(m_layers.size()))
        {
            wxRect localRect = m_layerPatchTransaction.localRect;
            const DocumentLayer& layer = m_layers[layerIndex];

            localRect.Intersect(wxRect(0, 0, layer.image.GetWidth(), layer.image.GetHeight()));

            if (!localRect.IsEmpty() &&
                beforeImage->GetWidth() == layer.image.GetWidth() &&
                beforeImage->GetHeight() == layer.image.GetHeight())
            {
                m_layerPatchTransaction.localRect = localRect;
                m_layerPatchTransaction.beforeImage = beforeImage->GetSubImage(localRect);
            }
        }
    }
    else
    {
        CaptureLayerPatchBefore();
    }

    if (!CaptureLayerPatchAfter())
        return;

    if (!m_layerPatchTransaction.beforeImage.IsOk())
        return;

    if (LayerPatchImagesEqual(m_layerPatchTransaction.beforeImage, m_layerPatchTransaction.afterImage))
        return;

    m_undoPatchStack.push_back(m_layerPatchTransaction);
    m_undoEntryIsPatch.push_back(true);

    m_historyLabels.push_back(
        m_layerPatchTransactionLabel.IsEmpty() ? "Action" : m_layerPatchTransactionLabel
    );

    while (m_undoEntryIsPatch.size() > k_maxHistoryStates)
    {
        const bool oldWasPatch = m_undoEntryIsPatch.front();
        m_undoEntryIsPatch.erase(m_undoEntryIsPatch.begin());

        if (oldWasPatch)
        {
            if (!m_undoPatchStack.empty())
                m_undoPatchStack.erase(m_undoPatchStack.begin());
        }
        else
        {
            if (!m_undoStack.empty())
                m_undoStack.erase(m_undoStack.begin());
        }

        if (!m_historyLabels.empty())
            m_historyLabels.erase(m_historyLabels.begin());
    }

    m_redoStack.clear();
    m_redoPatchStack.clear();
    m_redoEntryIsPatch.clear();
    m_redoHistoryLabels.clear();

    m_layerPatchTransaction = LayerPatchHistoryState();
    m_layerPatchTransactionLabel.clear();

    NotifyDocumentChanged();
}

bool Document::CanRedo() const
{
    return !m_redoEntryIsPatch.empty();
}

size_t Document::GetUndoCount() const
{
    return m_undoEntryIsPatch.size();
}

size_t Document::GetRedoCount() const
{
    return m_redoEntryIsPatch.size();
}

Document::HistoryState Document::CaptureHistoryState() const
{
    HistoryState state;
    state.selectedLayer = m_selectedLayer;
    state.pageWidth = m_pageWidth;
    state.pageHeight = m_pageHeight;
    state.resolution = m_resolution;

    state.layers.clear();
    state.layers.reserve(m_layers.size());

    for (const DocumentLayer& srcLayer : m_layers)
    {
        DocumentLayer dstLayer;
        dstLayer.name = srcLayer.name;
        dstLayer.visible = srcLayer.visible;
        dstLayer.locked = srcLayer.locked;
        dstLayer.opacity = srcLayer.opacity;
        dstLayer.offsetX = srcLayer.offsetX;
        dstLayer.offsetY = srcLayer.offsetY;

        if (srcLayer.image.IsOk())
            dstLayer.image = srcLayer.image.Copy();

        state.layers.push_back(dstLayer);
    }

    return state;
}

void Document::RestoreHistoryState(const HistoryState& state)
{
    const wxPoint oldViewOffset = m_viewOffset;

    m_layers = state.layers;
    m_selectedLayer = state.selectedLayer;
    m_pageWidth = std::max(1, state.pageWidth);
    m_pageHeight = std::max(1, state.pageHeight);
    m_resolution = std::max(1.0, state.resolution);

    m_viewOffset = oldViewOffset;

    MarkDisplayCacheDirty();
    SyncRulers();
    NotifyDocumentChanged();
}

bool Document::HistoryStatesEqual(const HistoryState& a, const HistoryState& b) const
{
    if (a.selectedLayer != b.selectedLayer)
        return false;

    if (a.pageWidth != b.pageWidth || a.pageHeight != b.pageHeight)
        return false;

    if (std::fabs(a.resolution - b.resolution) > 1e-9)
        return false;

    if (a.layers.size() != b.layers.size())
        return false;

    for (size_t i = 0; i < a.layers.size(); ++i)
    {
        const DocumentLayer& la = a.layers[i];
        const DocumentLayer& lb = b.layers[i];

        if (la.name != lb.name || la.visible != lb.visible || la.locked != lb.locked || la.opacity != lb.opacity || la.offsetX != lb.offsetX || la.offsetY != lb.offsetY)
            return false;

        if (!ImagesEqual(la.image, lb.image))
            return false;
    }

    return true;
}

bool Document::LayerPatchImagesEqual(const wxImage& a, const wxImage& b) const
{
    return ImagesEqual(a, b);
}

bool Document::CaptureLayerPatchBefore()
{
    if (!m_layerPatchTransaction.valid)
        return false;

    const int layerIndex = m_layerPatchTransaction.layerIndex;

    if (layerIndex < 0 || layerIndex >= static_cast<int>(m_layers.size()))
        return false;

    const DocumentLayer& layer = m_layers[layerIndex];

    if (!layer.image.IsOk())
        return false;

    wxRect localRect = m_layerPatchTransaction.localRect;
    localRect.Intersect(wxRect(0, 0, layer.image.GetWidth(), layer.image.GetHeight()));

    if (localRect.IsEmpty())
        return false;

    m_layerPatchTransaction.localRect = localRect;
    m_layerPatchTransaction.beforeImage = layer.image.GetSubImage(localRect);

    return m_layerPatchTransaction.beforeImage.IsOk();
}

bool Document::CaptureLayerPatchAfter()
{
    if (!m_layerPatchTransaction.valid)
        return false;

    const int layerIndex = m_layerPatchTransaction.layerIndex;

    if (layerIndex < 0 || layerIndex >= static_cast<int>(m_layers.size()))
        return false;

    const DocumentLayer& layer = m_layers[layerIndex];

    if (!layer.image.IsOk())
        return false;

    wxRect localRect = m_layerPatchTransaction.localRect;
    localRect.Intersect(wxRect(0, 0, layer.image.GetWidth(), layer.image.GetHeight()));

    if (localRect.IsEmpty())
        return false;

    m_layerPatchTransaction.localRect = localRect;
    m_layerPatchTransaction.afterImage = layer.image.GetSubImage(localRect);

    return m_layerPatchTransaction.afterImage.IsOk();
}

void Document::ApplyLayerPatch(const LayerPatchHistoryState& patch, bool useAfterImage)
{
    if (!patch.valid)
        return;

    if (patch.layerIndex < 0 || patch.layerIndex >= static_cast<int>(m_layers.size()))
        return;

    DocumentLayer& layer = m_layers[patch.layerIndex];

    if (!layer.image.IsOk())
        return;

    const wxImage& srcImage = useAfterImage ? patch.afterImage : patch.beforeImage;

    if (!srcImage.IsOk())
        return;

    wxRect localRect = patch.localRect;
    localRect.Intersect(wxRect(0, 0, layer.image.GetWidth(), layer.image.GetHeight()));

    if (localRect.IsEmpty())
        return;

    if (srcImage.GetWidth() != localRect.width || srcImage.GetHeight() != localRect.height)
        return;

    EnsureAlpha(layer.image);

    unsigned char* dstData = layer.image.GetData();
    unsigned char* dstAlpha = layer.image.GetAlpha();

    const unsigned char* srcData = srcImage.GetData();
    const unsigned char* srcAlpha = srcImage.HasAlpha() ? srcImage.GetAlpha() : nullptr;

    if (!dstData || !dstAlpha || !srcData)
        return;

    const int dstW = layer.image.GetWidth();
    const int srcW = srcImage.GetWidth();

    for (int y = 0; y < localRect.height; ++y)
    {
        const int dstY = localRect.y + y;

        for (int x = 0; x < localRect.width; ++x)
        {
            const int dstX = localRect.x + x;

            const int dstIndex = dstY * dstW + dstX;
            const int srcIndex = y * srcW + x;

            dstData[dstIndex * 3 + 0] = srcData[srcIndex * 3 + 0];
            dstData[dstIndex * 3 + 1] = srcData[srcIndex * 3 + 1];
            dstData[dstIndex * 3 + 2] = srcData[srcIndex * 3 + 2];

            dstAlpha[dstIndex] = srcAlpha ? srcAlpha[srcIndex] : 255;
        }
    }

    InvalidateDisplayCache();

    wxRect docRect(
        localRect.x + layer.offsetX,
        localRect.y + layer.offsetY,
        localRect.width,
        localRect.height
    );

    RequestCanvasRefreshRect(docRect);
}

void Document::PushHistorySnapshot(const wxString& label)
{
    if (m_inUndoRedo)
        return;

    const HistoryState state = CaptureHistoryState();

    if (!m_undoStack.empty() && HistoryStatesEqual(m_undoStack.back(), state))
        return;

    m_undoStack.push_back(state);
    m_undoEntryIsPatch.push_back(false);

    m_historyLabels.push_back(label.IsEmpty() ? "Action" : label);

    while (m_undoEntryIsPatch.size() > k_maxHistoryStates)
    {
        const bool oldWasPatch = m_undoEntryIsPatch.front();
        m_undoEntryIsPatch.erase(m_undoEntryIsPatch.begin());

        if (oldWasPatch)
        {
            if (!m_undoPatchStack.empty())
                m_undoPatchStack.erase(m_undoPatchStack.begin());
        }
        else
        {
            if (!m_undoStack.empty())
                m_undoStack.erase(m_undoStack.begin());
        }

        if (!m_historyLabels.empty())
            m_historyLabels.erase(m_historyLabels.begin());
    }

    m_redoStack.clear();
    m_redoPatchStack.clear();
    m_redoEntryIsPatch.clear();
    m_redoHistoryLabels.clear();

    NotifyDocumentChanged();
}

void Document::ClearHistory()
{
    m_undoStack.clear();
    m_redoStack.clear();

    m_undoPatchStack.clear();
    m_redoPatchStack.clear();

    m_undoEntryIsPatch.clear();
    m_redoEntryIsPatch.clear();

    m_moveHistoryActive = false;
    m_historyTransactionActive = false;
    m_layerPatchTransactionActive = false;

    m_historyTransactionStartState = HistoryState();
    m_moveHistoryStartState = HistoryState();
    m_layerPatchTransaction = LayerPatchHistoryState();

    m_historyLabels.clear();
    m_redoHistoryLabels.clear();

    m_historyTransactionLabel.clear();
    m_layerPatchTransactionLabel.clear();
}

bool Document::Undo()
{
    if (m_undoEntryIsPatch.empty())
        return false;

    const bool isPatch = m_undoEntryIsPatch.back();
    m_undoEntryIsPatch.pop_back();

    wxString label;
    if (!m_historyLabels.empty())
    {
        label = m_historyLabels.back();
        m_historyLabels.pop_back();
    }

    m_inUndoRedo = true;

    if (isPatch)
    {
        if (m_undoPatchStack.empty())
        {
            m_inUndoRedo = false;
            return false;
        }

        LayerPatchHistoryState patch = m_undoPatchStack.back();
        m_undoPatchStack.pop_back();

        ApplyLayerPatch(patch, false);

        m_redoPatchStack.push_back(patch);
        m_redoEntryIsPatch.push_back(true);
    }
    else
    {
        if (m_undoStack.empty())
        {
            m_inUndoRedo = false;
            return false;
        }

        const HistoryState current = CaptureHistoryState();
        const HistoryState previous = m_undoStack.back();
        m_undoStack.pop_back();

        m_redoStack.push_back(current);
        m_redoEntryIsPatch.push_back(false);

        RestoreHistoryState(previous);
    }

    if (!label.IsEmpty())
        m_redoHistoryLabels.push_back(label);

    m_inUndoRedo = false;
    NotifyDocumentChanged();
    return true;
}

bool Document::Redo()
{
    if (m_redoEntryIsPatch.empty())
        return false;

    const bool isPatch = m_redoEntryIsPatch.back();
    m_redoEntryIsPatch.pop_back();

    wxString label;
    if (!m_redoHistoryLabels.empty())
    {
        label = m_redoHistoryLabels.back();
        m_redoHistoryLabels.pop_back();
    }

    m_inUndoRedo = true;

    if (isPatch)
    {
        if (m_redoPatchStack.empty())
        {
            m_inUndoRedo = false;
            return false;
        }

        LayerPatchHistoryState patch = m_redoPatchStack.back();
        m_redoPatchStack.pop_back();

        ApplyLayerPatch(patch, true);

        m_undoPatchStack.push_back(patch);
        m_undoEntryIsPatch.push_back(true);
    }
    else
    {
        if (m_redoStack.empty())
        {
            m_inUndoRedo = false;
            return false;
        }

        const HistoryState current = CaptureHistoryState();
        const HistoryState next = m_redoStack.back();
        m_redoStack.pop_back();

        m_undoStack.push_back(current);
        m_undoEntryIsPatch.push_back(false);

        RestoreHistoryState(next);
    }

    if (!label.IsEmpty())
        m_historyLabels.push_back(label);

    if (m_undoEntryIsPatch.size() > k_maxHistoryStates)
    {
        const bool oldWasPatch = m_undoEntryIsPatch.front();
        m_undoEntryIsPatch.erase(m_undoEntryIsPatch.begin());

        if (oldWasPatch)
        {
            if (!m_undoPatchStack.empty())
                m_undoPatchStack.erase(m_undoPatchStack.begin());
        }
        else
        {
            if (!m_undoStack.empty())
                m_undoStack.erase(m_undoStack.begin());
        }

        if (!m_historyLabels.empty())
            m_historyLabels.erase(m_historyLabels.begin());
    }

    m_inUndoRedo = false;
    NotifyDocumentChanged();
    return true;
}

void Document::BeginMoveHistoryGesture()
{
    if (m_moveHistoryActive)
        return;

    m_moveHistoryStartState = CaptureHistoryState();
    m_moveHistoryActive = true;
}

void Document::EndMoveHistoryGesture()
{
    if (!m_moveHistoryActive)
        return;

    m_moveHistoryActive = false;

    const HistoryState current = CaptureHistoryState();
    if (HistoryStatesEqual(m_moveHistoryStartState, current))
        return;

    if (!m_undoStack.empty() && HistoryStatesEqual(m_undoStack.back(), m_moveHistoryStartState))
    {
        m_redoStack.clear();
        NotifyDocumentChanged();
        return;
    }

    m_undoStack.push_back(m_moveHistoryStartState);
    m_undoEntryIsPatch.push_back(false);
    m_historyLabels.push_back("Move Layer");

    while (m_undoEntryIsPatch.size() > k_maxHistoryStates)
    {
        const bool oldWasPatch = m_undoEntryIsPatch.front();
        m_undoEntryIsPatch.erase(m_undoEntryIsPatch.begin());

        if (oldWasPatch)
        {
            if (!m_undoPatchStack.empty())
                m_undoPatchStack.erase(m_undoPatchStack.begin());
        }
        else
        {
            if (!m_undoStack.empty())
                m_undoStack.erase(m_undoStack.begin());
        }

        if (!m_historyLabels.empty())
            m_historyLabels.erase(m_historyLabels.begin());
    }

    m_redoStack.clear();
    m_redoPatchStack.clear();
    m_redoEntryIsPatch.clear();
    m_redoHistoryLabels.clear();

    NotifyDocumentChanged();
}

void Document::NotifyDocumentChanged()
{
    RequestCanvasRefresh();
}



















