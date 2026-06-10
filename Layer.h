#pragma once

#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <wx/choice.h>
#include <wx/popupwin.h>
#include <wx/slider.h>
#include <wx/statbmp.h>
#include <vector>

#include "IconButton.h"

class Document;
class CompactSpinCtrl;

class Layer : public wxPanel
{
public:
    explicit Layer(wxWindow* parent);

    void ClearLayers();
    void SetLayersFromDocument(const Document* document);

    void OnAddLayerMenu(wxCommandEvent& event);
    void OnDeleteLayerMenu(wxCommandEvent& event);
    void OnDuplicateLayerMenu(wxCommandEvent& event);
    void OnRenameLayerMenu(wxCommandEvent& event);
    void OnMergeLayerMenu(wxCommandEvent& event);

private:
    struct LayerRow
    {
        wxPanel* panel = nullptr;
        IconButton* visibleButton = nullptr;
        IconButton* lockButton = nullptr;
        wxStaticBitmap* preview = nullptr;
        wxStaticText* nameText = nullptr;
        wxTextCtrl* nameEdit = nullptr;
        int documentLayerIndex = -1;
    };

private:
    void BuildUI();
    void RefreshMainFrameDocumentUI();
    void RebuildLayerRows();
    wxBitmap MakeEmptyPreviewBitmap() const;
    wxBitmap MakeLayerPreviewBitmap(int documentLayerIndex) const;
    void UpdateTopButtonsFromDocument();
    void UpdateRowButtonsFromDocument();

    void ShowLayerContextMenu(wxWindow* sourceWindow, const wxPoint& screenPos);

    void OnToggleAllVisible(wxCommandEvent& event);
    void OnToggleAllLocked(wxCommandEvent& event);
    void OnLayerRowClicked(wxMouseEvent& event);
    void OnToggleLayerVisible(wxCommandEvent& event);
    void OnToggleLayerLocked(wxCommandEvent& event);

    void ShowOpacityPopup();
    void HideOpacityPopup();
    void SetOpacityValueUI(int value);
    void OnOpacityArrowClicked(wxMouseEvent& event);
    void OnOpacitySliderChanged(wxCommandEvent& event);
    void OnOpacitySliderReleased(wxScrollEvent& event);
    void OnOpacityTextEnter(wxCommandEvent& event);
    void OnOpacityTextKillFocus(wxFocusEvent& event);

    void ShowFillPopup();
    void HideFillPopup();
    void SetFillValueUI(int value);
    void OnFillArrowClicked(wxMouseEvent& event);
    void OnFillSliderChanged(wxCommandEvent& event);
    void OnFillTextEnter(wxCommandEvent& event);
    void OnFillTextKillFocus(wxFocusEvent& event);

    void OnLayerRowRightClick(wxMouseEvent& event);

    // Rename layer
    void BeginInlineRename(int documentLayerIndex);
    void CommitInlineRename(wxWindow* sourceWindow);
    void CancelInlineRename();

    void OnInlineRenameEnter(wxCommandEvent& event);
    void OnInlineRenameKillFocus(wxFocusEvent& event);

private:
    Document* m_currentDocument = nullptr;
    bool m_committingRename = false;

    wxPanel* m_topPanel = nullptr;

    wxChoice* m_blendModeChoice = nullptr;

    wxTextCtrl* m_opacityText = nullptr;
    wxStaticBitmap* m_opacityArrow = nullptr;
    wxPopupTransientWindow* m_opacityPopup = nullptr;
    wxSlider* m_opacitySlider = nullptr;
    int m_opacityDragStartValue = 100;
    bool m_opacityDragging = false;

    wxTextCtrl* m_fillText = nullptr;
    wxStaticBitmap* m_fillArrow = nullptr;
    wxPopupTransientWindow* m_fillPopup = nullptr;
    wxSlider* m_fillSlider = nullptr;
    wxStaticText* m_fillLabel = nullptr;

    wxStaticText* m_lockLabel = nullptr;
    IconButton* m_allVisibleButton = nullptr;
    IconButton* m_allLockButton = nullptr;

    wxScrolledWindow* m_layerScroll = nullptr;
    wxBoxSizer* m_layerListSizer = nullptr;

    std::vector<LayerRow> m_rows;
};
