#pragma once

#include <wx/wx.h>
#include <wx/timer.h>
#include <vector>
#include "ToolType.h"
#include "dialog/GradientData.h"
#include "dialog/CanvasSizeDialog.h"

class Ruler;
class DocumentCanvas;

struct DocumentLayer
{
    wxString name;
    wxImage image;
    bool visible = true;
    bool locked = false;
    int opacity = 100;
    int offsetX = 0;
    int offsetY = 0;
};

struct BrushDabPixel
{
    int ox = 0;
    int oy = 0;
    double strength = 0.0;
};

class Document : public wxPanel
{
public:
    explicit Document(wxWindow* parent, int pageWidth = 1000, int pageHeight = 1000, int contentMode = 0);
    ~Document() override = default;

    void ResetBlankDocument(const wxString& layerName);
    bool LoadImageAsSingleLayer(const wxImage& image, const wxString& layerName);
    bool DuplicateFromDocument(const Document& source);

    bool SaveASB(const wxString& path) const;
    bool LoadASB(const wxString& path);

    const DocumentLayer* GetLayer(int index) const;
    const std::vector<DocumentLayer>& GetLayers() const;
    int GetLayerCount() const;
    int GetSelectedLayer() const;
    void SetSelectedLayer(int index);
    int GetLayerOpacity(int index) const;
    void SetLayerOpacity(int index, int opacity);
    void SetLayerOpacityInternal(int index, int opacity);

    int GetPageWidth() const;
    int GetPageHeight() const;
    int GetContentMode() const;

    double GetZoom() const;
    void SetZoom(double zoom);
    void SetZoomAroundPoint(double zoom, const wxPoint& focusClientPoint);
    void ZoomIn();
    void ZoomOut();
    void FitInWindow();
    void Zoom100();

    wxPoint GetPageOriginInVirtualPixels() const;
    wxSize GetScaledPageSize() const;

    const wxBitmap* GetCachedLayerBitmap(int index) const;
    const wxBitmap* GetPreviewLayerBitmap(int index) const;
    const wxBitmap* GetOriginalLayerBitmap(int index) const;

    void EnsureDisplayCache();
    void NotifyCanvasScrolled();
    void RequestCanvasRefresh();
    void RequestCanvasRefreshRect(const wxRect& docRect);
    void MarkDisplayCacheDirty();
    DocumentCanvas* GetCanvas() const;

    wxPoint GetViewOffset() const;
    void SetViewOffset(const wxPoint& viewOffset);
    wxPoint ScreenToWorld(const wxPoint& screenPoint) const;
    wxPoint ScreenToWorldPixel(const wxPoint& screenPoint) const;
    wxPoint WorldToCanvasVirtual(const wxPoint& worldPoint) const;
    void UpdateViewCenter();

    bool IsLayerVisible(int index) const;
    bool IsLayerLocked(int index) const;

    void SetLayerVisible(int index, bool visible);
    void SetLayerLocked(int index, bool locked);

    void SetAllLayersVisible(bool visible);
    void SetAllLayersLocked(bool locked);
    bool AreAllLayersVisible() const;
    bool AreAllLayersLocked() const;

    bool IsCompositeChannelVisible() const;
    bool IsRedChannelVisible() const;
    bool IsGreenChannelVisible() const;
    bool IsBlueChannelVisible() const;

    void SetCompositeChannelVisible(bool visible);
    void SetRedChannelVisible(bool visible);
    void SetGreenChannelVisible(bool visible);
    void SetBlueChannelVisible(bool visible);

    void ApplyChannelVisibilityToImage(wxImage& image) const;

    void AddLayer();
    bool SelectedLayerHasAnyPixel() const;
    bool DeleteSelectedLayer();
    void DuplicateSelectedLayer();
    wxString GetSelectedLayerName() const;
    void SetSelectedLayerName(const wxString& name);

    ToolType GetActiveTool() const;
    void SetActiveTool(ToolType tool);

    bool CanMoveSelectedLayer() const;
    wxPoint GetSelectedLayerOffset() const;
    void SetSelectedLayerOffset(int offsetX, int offsetY);
    bool TransformSelectedLayerToRect(const wxImage& sourceImage, const wxRect& targetRect);

    bool CanDrawOnSelectedLayer() const;
    bool DrawOnSelectedLayerLine(const wxPoint& startDocPoint, const wxPoint& endDocPoint, int size, const wxColour& color, bool erase);
    bool DrawPencilOnSelectedLayerLine(const wxPoint& startDocPoint, const wxPoint& endDocPoint, int size, const wxColour& color);
    bool DrawBrushOnSelectedLayerLine(const wxPoint& startDocPoint, const wxPoint& endDocPoint, int size, int hardness, int opacity, int flow, const wxColour& color, const wxImage* strokeBaseImage = nullptr, std::vector<double>* strokeMask = nullptr, bool erase = false, const std::vector<BrushDabPixel>* cachedDab = nullptr);
    bool DrawCloneStampOnSelectedLayerLine(const wxPoint& startDocPoint, const wxPoint& endDocPoint, const wxPoint& sourceStartDocPoint, const wxPoint& sourceEndDocPoint, int size, int hardness, int opacity, int flow, const wxImage* sourceImage, std::vector<double>* strokeMask = nullptr, const std::vector<BrushDabPixel>* cachedDab = nullptr);

    bool GetSnapToDocumentBounds() const;
    void SetSnapToDocumentBounds(bool enabled);

    wxPoint GetSnappedLayerOffset(int layerIndex, int offsetX, int offsetY) const;
    wxPoint GetSelectedLayerSnappedOffset(int offsetX, int offsetY) const;

    bool CopySelectionFromSelectedLayer(const wxRect& docRect, wxImage& outImage, const std::vector<unsigned char>* selectionMask = nullptr) const;
    bool CutSelectionFromSelectedLayer(const wxRect& docRect, wxImage& outImage, const std::vector<unsigned char>* selectionMask = nullptr);
    bool DeleteSelectionFromSelectedLayer(const wxRect& docRect, const std::vector<unsigned char>* selectionMask = nullptr);
    bool FillSelectionOnSelectedLayer(const wxRect& docRect, const wxColour& color, const std::vector<unsigned char>* selectionMask = nullptr);
    bool BlendSelectionOnSelectedLayer(const wxRect& docRect, const wxColour& color, int opacity, const std::vector<unsigned char>* selectionMask = nullptr);
    bool BucketFillSelectedLayerAt(const wxPoint& docPoint, const wxColour& color, int tolerance = 32, bool antiAlias = true, bool contiguous = true, bool sampleAllLayers = false, const wxRect* limitDocRect = nullptr, const std::vector<unsigned char>* selectionMask = nullptr);
    bool ApplyLinearGradientToSelectedLayer(const wxPoint& startDocPoint, const wxPoint& endDocPoint, const GradientData& gradient, const wxRect* limitDocRect = nullptr, const std::vector<unsigned char>* selectionMask = nullptr);
    bool InsertLayerAboveCurrent(const wxImage& image, const wxString& layerName);
    bool InsertTextLayerAt(const wxPoint& docPoint, const wxString& text, const wxFont& font, const wxColour& color);
    bool PasteOnSelectedEmptyLayer(const wxImage& image);
    bool CropToRect(const wxRect& docRect);

    bool CanUndo() const;
    bool IsModified() const;
    void MarkClean();
    const std::vector<wxString>& GetHistoryLabels() const;
    void BeginHistoryTransaction(const wxString& label = "Action");
    void EndHistoryTransaction();

    void BeginLayerPatchHistoryTransaction(const wxString& label = "Action");
    void ExpandLayerPatchHistoryRect(const wxRect& docRect);
    void EndLayerPatchHistoryTransaction(const wxImage* beforeImage = nullptr);

    bool CanRedo() const;
    bool Undo();
    bool Redo();
    size_t GetUndoCount() const;
    size_t GetRedoCount() const;

    void BeginMoveHistoryGesture();
    void EndMoveHistoryGesture();

    bool IsZooming() const { return m_isZooming; }
    void BeginInteractiveZoom();
    void EndInteractiveZoom();

    double GetResolution() const;
    bool ResizeImage(int newWidth, int newHeight, double newResolution, int resampleQuality);
    bool ResizeCanvas(int newWidth, int newHeight, AnchorGridPanel::AnchorPosition anchor, const wxColour& fillColor);
    bool RotateCanvas180();
    bool RotateCanvas90CW();
    bool RotateCanvas90CCW();
    bool RotateCanvasArbitrary(double degreesClockwise);
    bool FlipCanvasHorizontal();
    bool FlipCanvasVertical();

    wxImage CopySelectedLayerImage() const;
    bool ReplaceSelectedLayerImage(const wxImage& image, bool addHistory = true, const wxString& historyLabel = "Color Balance");
    bool PreviewSelectedLayerImage(const wxImage& image);
    wxImage CopyVisibleMergedImage(bool applyChannelVisibility = true) const;
    bool PickVisibleColorAt(const wxPoint& docPoint, wxColour& outColor) const;

private:
    struct HistoryState
    {
        std::vector<DocumentLayer> layers;
        int selectedLayer = -1;
        int pageWidth = 0;
        int pageHeight = 0;
        double resolution = 72.0;
    };

    struct LayerPatchHistoryState
    {
        bool valid = false;
        int layerIndex = -1;
        wxRect localRect;
        wxImage beforeImage;
        wxImage afterImage;
    };

    void BuildUI();
    void SyncRulers();
    void InvalidateDisplayCache();
    void RebuildDisplayCache();
    void RebuildPreviewCache();
    void RebuildOriginalBitmapCache();
    void UpdateZoomStatusText();
    void OnSize(wxSizeEvent& event);
    void OnZoomSettleTimer(wxTimerEvent& event);

    HistoryState CaptureHistoryState() const;
    void RestoreHistoryState(const HistoryState& state);
    bool HistoryStatesEqual(const HistoryState& a, const HistoryState& b) const;
    bool CaptureLayerPatchBefore();
    bool CaptureLayerPatchAfter();
    void ApplyLayerPatch(const LayerPatchHistoryState& patch, bool useAfterImage);
    bool LayerPatchImagesEqual(const wxImage& a, const wxImage& b) const;
    void PushHistorySnapshot(const wxString& label = "Action");
    void ClearHistory();
    void NotifyDocumentChanged();

private:
    int m_pageWidth = 1000;
    int m_pageHeight = 1000;
    int m_contentMode = 0;

    std::vector<DocumentLayer> m_layers;
    int m_selectedLayer = -1;

    wxPanel* m_cornerPanel = nullptr;
    Ruler* m_hRuler = nullptr;
    Ruler* m_vRuler = nullptr;
    DocumentCanvas* m_canvas = nullptr;

    wxPoint m_viewOffset{0, 0};
    double m_zoom = 1.0;
    ToolType m_activeTool = ToolType::Move;

    static constexpr double k_zoomMin = 0.125;
    static constexpr double k_zoomMax = 16.0;
    static constexpr size_t k_maxHistoryStates = 50;
    static constexpr int k_previewMaxDim = 1024;

    bool m_cacheDirty = true;
    double m_cachedZoom = -1.0;
    std::vector<wxBitmap> m_cachedLayerBitmaps;

    bool m_hasCleanState = false;
    HistoryState m_cleanState;

    std::vector<wxBitmap> m_originalLayerBitmaps;
    std::vector<wxBitmap> m_previewLayerBitmaps;

    wxTimer m_zoomSettleTimer;
    bool m_isZooming = false;

    bool m_snapToDocumentBounds = true;
    bool m_redChannelVisible = true;
    bool m_greenChannelVisible = true;
    bool m_blueChannelVisible = true;

    std::vector<HistoryState> m_undoStack;
    std::vector<HistoryState> m_redoStack;

    std::vector<LayerPatchHistoryState> m_undoPatchStack;
    std::vector<LayerPatchHistoryState> m_redoPatchStack;
    std::vector<bool> m_undoEntryIsPatch;
    std::vector<bool> m_redoEntryIsPatch;

    bool m_layerPatchTransactionActive = false;
    LayerPatchHistoryState m_layerPatchTransaction;
    wxString m_layerPatchTransactionLabel;

    bool m_inUndoRedo = false;
    bool m_moveHistoryActive = false;
    HistoryState m_moveHistoryStartState;
    bool m_historyTransactionActive = false;
    HistoryState m_historyTransactionStartState;
    std::vector<wxString> m_historyLabels;
    std::vector<wxString> m_redoHistoryLabels;
    wxString m_historyTransactionLabel;

    double m_resolution = 72.0;

private:
    wxDECLARE_EVENT_TABLE();
};
