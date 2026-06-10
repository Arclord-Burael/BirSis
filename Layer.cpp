#include "AppIDs.h"
#include "Layer.h"
#include "Document.h"
#include "MainFrame.h"
#include "CompactSpinCtrl.h"
#include "tools/EmbeddedIcons.h"
#include "Theme.h"

#include <wx/aui/auibook.h>
#include <wx/textctrl.h>
#include <wx/msgdlg.h>
#include <wx/menu.h>
#include <wx/popupwin.h>
#include <wx/renderer.h>
#include <wx/dcmemory.h>
#include <cstdint>

namespace
{
    int PtrToLayerIndex(void* ptr)
    {
        return static_cast<int>(reinterpret_cast<intptr_t>(ptr));
    }

    void* LayerIndexToPtr(int index)
    {
        return reinterpret_cast<void*>(static_cast<intptr_t>(index));
    }

    wxBitmap MakeBlankIcon()
    {
        return wxBitmap();
    }

    wxBitmap MakeNativeDropArrowBitmap(wxWindow* win, const wxSize& size)
    {
        wxBitmap bmp(size.x, size.y);
        wxMemoryDC dc(bmp);

        dc.SetBackground(wxBrush(wxColour(45, 45, 48)));
        dc.Clear();

        const int cx = size.x / 2;
        const int cy = size.y / 2 + 1;

        wxPoint pts[3] =
        {
            wxPoint(cx - 4, cy - 2),
            wxPoint(cx + 4, cy - 2),
            wxPoint(cx,     cy + 3)
        };

        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(wxColour(235, 235, 235)));
        dc.DrawPolygon(3, pts);

        dc.SelectObject(wxNullBitmap);
        return bmp;
    }
}

Layer::Layer(wxWindow* parent)
: wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(280, -1))
{
    SetMinSize(wxSize(200, 200));
    BuildUI();
}


void Layer::RefreshMainFrameDocumentUI()
{
    MainFrame* mainFrame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
    if (mainFrame)
        mainFrame->RefreshDocumentDependentUI();
}

void Layer::BuildUI()
{
    wxBoxSizer* rootSizer = new wxBoxSizer(wxVERTICAL);

    m_topPanel = new wxPanel(this, wxID_ANY);
    m_topPanel->SetBackgroundColour(wxColour(65, 65, 68));

    wxBoxSizer* topPanelSizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* filterRow = new wxBoxSizer(wxHORIZONTAL);

    m_blendModeChoice = new wxChoice(m_topPanel, wxID_ANY, wxDefaultPosition, wxSize(125, 28));
    m_blendModeChoice->Append("Normal");
    m_blendModeChoice->Append("Dissolve");
    m_blendModeChoice->Append("Darken");
    m_blendModeChoice->Append("Multiply");
    m_blendModeChoice->Append("Color Burn");
    m_blendModeChoice->Append("Lighten");
    m_blendModeChoice->Append("Screen");
    m_blendModeChoice->Append("Overlay");
    m_blendModeChoice->Append("Soft Light");
    m_blendModeChoice->Append("Hard Light");
    m_blendModeChoice->SetSelection(0);
    m_blendModeChoice->SetBackgroundColour(wxColour(45, 45, 48));
    m_blendModeChoice->SetForegroundColour(*wxWHITE);

    wxStaticText* opacityLabel = new wxStaticText(m_topPanel, wxID_ANY, "Opacity:");
    opacityLabel->SetForegroundColour(*wxWHITE);

    wxPanel* opacityBox = new wxPanel(m_topPanel, wxID_ANY, wxDefaultPosition, wxSize(64, 24));
    opacityBox->SetBackgroundColour(wxColour(45, 45, 48));

    wxBoxSizer* opacityBoxSizer = new wxBoxSizer(wxHORIZONTAL);

    m_opacityText = new wxTextCtrl(opacityBox, wxID_ANY, "100", wxDefaultPosition, wxSize(42, 22), wxTE_PROCESS_ENTER | wxBORDER_NONE);

    m_opacityText->SetBackgroundColour(wxColour(45, 45, 48));
    m_opacityText->SetForegroundColour(*wxWHITE);
    m_opacityText->SetInsertionPointEnd();

    m_opacityArrow = new wxStaticBitmap(
    opacityBox,
    wxID_ANY,
    MakeNativeDropArrowBitmap(opacityBox, wxSize(18, 22)),
    wxDefaultPosition,
    wxSize(18, 22)
);

m_opacityArrow->SetBackgroundColour(wxColour(45, 45, 48));

    opacityBoxSizer->Add(m_opacityText, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 3);
    opacityBoxSizer->AddStretchSpacer(1);
    opacityBoxSizer->Add(m_opacityArrow, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
    opacityBox->SetSizer(opacityBoxSizer);

    m_opacityArrow->Bind(wxEVT_LEFT_DOWN, &Layer::OnOpacityArrowClicked, this);
    opacityBox->Bind(wxEVT_LEFT_DOWN, &Layer::OnOpacityArrowClicked, this);
    m_opacityText->Bind(wxEVT_TEXT_ENTER, &Layer::OnOpacityTextEnter, this);
    m_opacityText->Bind(wxEVT_KILL_FOCUS, &Layer::OnOpacityTextKillFocus, this);

    filterRow->Add(m_blendModeChoice, 0, wxALIGN_CENTER_VERTICAL);
    filterRow->AddStretchSpacer(1);
    filterRow->Add(opacityLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    filterRow->Add(opacityBox, 0, wxALIGN_CENTER_VERTICAL);

    wxBoxSizer* lockRow = new wxBoxSizer(wxHORIZONTAL);

    m_lockLabel = new wxStaticText(m_topPanel, wxID_ANY, "Lock:");
    m_lockLabel->SetForegroundColour(*wxWHITE);

    m_allVisibleButton = new IconButton(m_topPanel, wxID_ANY, GetEyeToolIcon(16, 16), "Show / Hide All Layers");
    m_allLockButton = new IconButton(m_topPanel, wxID_ANY, GetLockToolIcon(16, 16), "Lock / Unlock All Layers");

    m_allVisibleButton->SetButtonSize(wxSize(24, 24));
    m_allLockButton->SetButtonSize(wxSize(24, 24));

    m_allVisibleButton->SetBackgroundColour(m_topPanel->GetBackgroundColour());
    m_allLockButton->SetBackgroundColour(m_topPanel->GetBackgroundColour());

    m_fillLabel = new wxStaticText(m_topPanel, wxID_ANY, "Fill:");
    m_fillLabel->SetForegroundColour(*wxWHITE);

    wxPanel* fillBox = new wxPanel(m_topPanel, wxID_ANY, wxDefaultPosition, wxSize(64, 24));
    fillBox->SetBackgroundColour(wxColour(45, 45, 48));

    wxBoxSizer* fillBoxSizer = new wxBoxSizer(wxHORIZONTAL);

    m_fillText = new wxTextCtrl(fillBox, wxID_ANY, "100", wxDefaultPosition, wxSize(42, 22), wxTE_PROCESS_ENTER | wxBORDER_NONE);

    m_fillText->SetBackgroundColour(wxColour(45, 45, 48));
    m_fillText->SetForegroundColour(*wxWHITE);
    m_fillText->SetInsertionPointEnd();

    m_fillArrow = new wxStaticBitmap(fillBox, wxID_ANY, MakeNativeDropArrowBitmap(fillBox, wxSize(18, 22)), wxDefaultPosition, wxSize(18, 22));

    m_fillArrow->SetBackgroundColour(wxColour(45, 45, 48));

    fillBoxSizer->Add(m_fillText, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 3);
    fillBoxSizer->AddStretchSpacer(1);
    fillBoxSizer->Add(m_fillArrow, 0, wxALIGN_CENTER_VERTICAL);
    fillBox->SetSizer(fillBoxSizer);

    m_fillArrow->Bind(wxEVT_LEFT_DOWN, &Layer::OnFillArrowClicked, this);
    fillBox->Bind(wxEVT_LEFT_DOWN, &Layer::OnFillArrowClicked, this);
    m_fillText->Bind(wxEVT_TEXT_ENTER, &Layer::OnFillTextEnter, this);
    m_fillText->Bind(wxEVT_KILL_FOCUS, &Layer::OnFillTextKillFocus, this);

    lockRow->Add(m_lockLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    lockRow->Add(m_allVisibleButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
    lockRow->Add(m_allLockButton, 0, wxALIGN_CENTER_VERTICAL);
    lockRow->AddStretchSpacer(1);
    lockRow->Add(m_fillLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    lockRow->Add(fillBox, 0, wxALIGN_CENTER_VERTICAL);

    topPanelSizer->Add(filterRow, 0, wxEXPAND | wxBOTTOM, 5);
    topPanelSizer->Add(lockRow, 0, wxEXPAND);

    m_topPanel->SetSizer(topPanelSizer);

    wxPanel* borderPanel = new wxPanel(this, wxID_ANY);
    borderPanel->SetBackgroundColour(wxColour(45, 45, 48));

    m_layerScroll = new wxScrolledWindow(borderPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_layerScroll->SetScrollRate(0, 10);
    m_layerScroll->SetBackgroundColour(wxColor(65, 65, 68));

    m_layerListSizer = new wxBoxSizer(wxVERTICAL);
    m_layerScroll->SetSizer(m_layerListSizer);

    wxBoxSizer* borderSizer = new wxBoxSizer(wxVERTICAL);
    borderSizer->Add(m_layerScroll, 1, wxEXPAND | wxALL, 1);
    borderPanel->SetSizer(borderSizer);

    wxBoxSizer* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
    wxButton* addBtn = new wxButton(this, wxID_ANY, "+", wxDefaultPosition, wxSize(28, 28));
    wxButton* delBtn = new wxButton(this, wxID_ANY, "-", wxDefaultPosition, wxSize(28, 28));
    wxButton* dupBtn = new wxButton(this, wxID_ANY, "D", wxDefaultPosition, wxSize(28, 28));

    buttonSizer->Add(addBtn, 0, wxRIGHT, 6);
    buttonSizer->Add(delBtn, 0, wxRIGHT, 6);
    buttonSizer->Add(dupBtn, 0);

    rootSizer->AddSpacer(6);
    rootSizer->Add(m_topPanel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    rootSizer->Add(borderPanel, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    rootSizer->Add(buttonSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

    SetSizer(rootSizer);

    m_allVisibleButton->Bind(wxEVT_BUTTON, &Layer::OnToggleAllVisible, this);
    m_allLockButton->Bind(wxEVT_BUTTON, &Layer::OnToggleAllLocked, this);

    Bind(wxEVT_MENU, &Layer::OnAddLayerMenu, this, ID_LAYER_MENU_ADD);
    Bind(wxEVT_MENU, &Layer::OnDeleteLayerMenu, this, ID_LAYER_MENU_DELETE);
    Bind(wxEVT_MENU, &Layer::OnDuplicateLayerMenu, this, ID_LAYER_MENU_DUPLICATE);
    Bind(wxEVT_MENU, &Layer::OnRenameLayerMenu, this, ID_LAYER_MENU_RENAME);
    Bind(wxEVT_MENU, &Layer::OnMergeLayerMenu, this, ID_LAYER_MENU_MERGE);
}

void Layer::SetOpacityValueUI(int value)
{
    value = std::max(0, std::min(100, value));

    if (m_opacityText)
        m_opacityText->ChangeValue(wxString::Format("%d", value));

    if (m_opacitySlider)
        m_opacitySlider->SetValue(value);
}

void Layer::ShowOpacityPopup()
{
    if (!m_opacityArrow || !m_opacityText)
        return;

    HideOpacityPopup();
    HideFillPopup();

    m_opacityPopup = new wxPopupTransientWindow(this, wxBORDER_SIMPLE);

    wxPanel* panel = new wxPanel(m_opacityPopup, wxID_ANY);
    panel->SetBackgroundColour(wxColour(62, 62, 66));

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    long currentValue = 100;
    m_opacityText->GetValue().ToLong(&currentValue);
    currentValue = std::max(0L, std::min(100L, currentValue));

    wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);

    m_opacitySlider = new wxSlider(panel, wxID_ANY, static_cast<int>(currentValue), 0, 100, wxDefaultPosition, wxSize(170, -1), wxSL_HORIZONTAL);

    m_opacitySlider->Bind(wxEVT_SLIDER, &Layer::OnOpacitySliderChanged, this);
    m_opacitySlider->Bind(wxEVT_SCROLL_THUMBRELEASE, &Layer::OnOpacitySliderReleased, this);

    wxStaticText* valueText = new wxStaticText(panel, wxID_ANY, wxString::Format("%ld", currentValue), wxDefaultPosition, wxSize(32, 22), wxALIGN_CENTER);
    valueText->SetForegroundColour(*wxWHITE);
    valueText->SetBackgroundColour(wxColour(62, 62, 66));

    valueText->Bind(wxEVT_UPDATE_UI, [this, valueText](wxUpdateUIEvent&)
    {
        if (m_opacityText)
            valueText->SetLabel(m_opacityText->GetValue());
    });

    row->Add(m_opacitySlider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    row->Add(valueText, 0, wxALIGN_CENTER_VERTICAL);

    sizer->Add(row, 0, wxEXPAND | wxALL, 8);
    panel->SetSizerAndFit(sizer);

    wxBoxSizer* popupSizer = new wxBoxSizer(wxVERTICAL);
    popupSizer->Add(panel, 1, wxEXPAND);
    m_opacityPopup->SetSizerAndFit(popupSizer);

    const wxPoint screenPos = m_opacityText->ClientToScreen(wxPoint(0, m_opacityText->GetSize().y + 2));
    m_opacityPopup->Position(screenPos, wxSize(0, 0));
    m_opacityPopup->Popup();
}

void Layer::HideOpacityPopup()
{
    if (!m_opacityPopup)
        return;

    m_opacityPopup->Dismiss();
    m_opacityPopup->Destroy();
    m_opacityPopup = nullptr;
    m_opacitySlider = nullptr;
}

void Layer::OnOpacityArrowClicked(wxMouseEvent& event)
{
    ShowOpacityPopup();
    event.Skip(false);
}

void Layer::OnOpacitySliderChanged(wxCommandEvent& event)
{
    const int value = event.GetInt();

    SetOpacityValueUI(value);

    if (!m_currentDocument)
        return;

    const int layerIndex = m_currentDocument->GetSelectedLayer();

    if (layerIndex < 0)
        return;

    if (!m_opacityDragging)
    {
        m_opacityDragging = true;
        m_opacityDragStartValue = m_currentDocument->GetLayerOpacity(layerIndex);
        m_currentDocument->BeginHistoryTransaction("Layer Opacity");
        wxLogMessage("m_opacityDragging=%d", m_opacityDragging);
    }

    m_currentDocument->SetLayerOpacityInternal(layerIndex, value);
}

void Layer::OnOpacitySliderReleased(wxScrollEvent& event)
{
    if (!m_currentDocument)
        return;

    if (m_opacityDragging)
    {
        m_currentDocument->EndHistoryTransaction();
        m_opacityDragging = false;
        wxLogMessage("m_opacityDragging reset to false");
    }
}

void Layer::OnOpacityTextEnter(wxCommandEvent& event)
{
    long value = 100;
    if (!m_opacityText || !m_opacityText->GetValue().ToLong(&value))
        value = 100;

    value = std::max(0L, std::min(100L, value));
    SetOpacityValueUI(static_cast<int>(value));

    // ADD: actually apply to the document with a history entry
    if (m_currentDocument)
    {
        const int layerIndex = m_currentDocument->GetSelectedLayer();
        if (layerIndex >= 0)
            m_currentDocument->SetLayerOpacity(layerIndex, static_cast<int>(value));
    }
    event.Skip(false);
}

void Layer::OnOpacityTextKillFocus(wxFocusEvent& event)
{
    long value = 100;

    if (m_opacityText && m_opacityText->GetValue().ToLong(&value))
        SetOpacityValueUI(static_cast<int>(value));
    else
        SetOpacityValueUI(100);

    event.Skip();
}

void Layer::SetFillValueUI(int value)
{
    value = std::max(0, std::min(100, value));

    if (m_fillText)
        m_fillText->ChangeValue(wxString::Format("%d", value));

    if (m_fillSlider)
        m_fillSlider->SetValue(value);
}

void Layer::ShowFillPopup()
{
    if (!m_fillArrow || !m_fillText)
        return;

    HideFillPopup();
    HideOpacityPopup();

    m_fillPopup = new wxPopupTransientWindow(this, wxBORDER_SIMPLE);

    wxPanel* panel = new wxPanel(m_fillPopup, wxID_ANY);
    panel->SetBackgroundColour(wxColour(62, 62, 66));

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    long currentValue = 100;
    m_fillText->GetValue().ToLong(&currentValue);
    currentValue = std::max(0L, std::min(100L, currentValue));

    wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);

    m_fillSlider = new wxSlider(
        panel,
        wxID_ANY,
        static_cast<int>(currentValue),
        0,
        100,
        wxDefaultPosition,
        wxSize(170, -1),
        wxSL_HORIZONTAL
    );

    m_fillSlider->Bind(wxEVT_SLIDER, &Layer::OnFillSliderChanged, this);

    wxStaticText* valueText = new wxStaticText(
        panel,
        wxID_ANY,
        wxString::Format("%ld", currentValue),
        wxDefaultPosition,
        wxSize(32, 22),
        wxALIGN_CENTER
    );

    valueText->SetForegroundColour(*wxWHITE);
    valueText->SetBackgroundColour(wxColour(62, 62, 66));

    valueText->Bind(wxEVT_UPDATE_UI, [this, valueText](wxUpdateUIEvent&)
    {
        if (m_fillText)
            valueText->SetLabel(m_fillText->GetValue());
    });

    row->Add(m_fillSlider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    row->Add(valueText, 0, wxALIGN_CENTER_VERTICAL);

    sizer->Add(row, 0, wxEXPAND | wxALL, 8);
    panel->SetSizerAndFit(sizer);

    wxBoxSizer* popupSizer = new wxBoxSizer(wxVERTICAL);
    popupSizer->Add(panel, 1, wxEXPAND);
    m_fillPopup->SetSizerAndFit(popupSizer);

    const wxPoint screenPos = m_fillText->ClientToScreen(wxPoint(0, m_fillText->GetSize().y + 2));
    m_fillPopup->Position(screenPos, wxSize(0, 0));
    m_fillPopup->Popup();
}

void Layer::HideFillPopup()
{
    if (!m_fillPopup)
        return;

    m_fillPopup->Dismiss();
    m_fillPopup->Destroy();
    m_fillPopup = nullptr;
    m_fillSlider = nullptr;
}

void Layer::OnFillArrowClicked(wxMouseEvent& event)
{
    ShowFillPopup();
    event.Skip(false);
}

void Layer::OnFillSliderChanged(wxCommandEvent& event)
{
    SetFillValueUI(event.GetInt());

    // Actual selected-layer fill will be wired later.
}

void Layer::OnFillTextEnter(wxCommandEvent& event)
{
    long value = 100;

    if (m_fillText && m_fillText->GetValue().ToLong(&value))
        SetFillValueUI(static_cast<int>(value));
    else
        SetFillValueUI(100);

    event.Skip(false);
}

void Layer::OnFillTextKillFocus(wxFocusEvent& event)
{
    long value = 100;

    if (m_fillText && m_fillText->GetValue().ToLong(&value))
        SetFillValueUI(static_cast<int>(value));
    else
        SetFillValueUI(100);

    event.Skip();
}

void Layer::ClearLayers()
{
    m_currentDocument = nullptr;

    if (m_layerListSizer)
        m_layerListSizer->Clear(true);

    m_rows.clear();

    if (m_allVisibleButton)
    {
        m_allVisibleButton->Enable(false);
        m_allVisibleButton->SetIcon(MakeBlankIcon());
    }

    if (m_allLockButton)
    {
        m_allLockButton->Enable(false);
        m_allLockButton->SetIcon(MakeBlankIcon());
    }

    if (m_layerScroll)
    {
        m_layerScroll->Layout();
        m_layerScroll->FitInside();
    }
}

void Layer::SetLayersFromDocument(const Document* document)
{
    m_currentDocument = const_cast<Document*>(document);
    RebuildLayerRows();
    UpdateTopButtonsFromDocument();
    UpdateRowButtonsFromDocument();
}

void Layer::RebuildLayerRows()
{
    if (!m_layerListSizer)
        return;

    m_layerListSizer->Clear(true);
    m_rows.clear();

    if (!m_currentDocument)
    {
        if (m_layerScroll)
        {
            m_layerScroll->Layout();
            m_layerScroll->FitInside();
        }
        return;
    }

    const std::vector<DocumentLayer>& layers = m_currentDocument->GetLayers();

    for (int i = static_cast<int>(layers.size()) - 1; i >= 0; --i)
    {
        LayerRow row;
        row.documentLayerIndex = i;

        row.panel = new wxPanel(m_layerScroll, wxID_ANY);
        row.panel->SetMinSize(wxSize(-1, 39));
        row.panel->SetBackgroundColour(m_layerScroll->GetBackgroundColour());

        wxBoxSizer* rowSizer = new wxBoxSizer(wxHORIZONTAL);

        row.visibleButton = new IconButton(row.panel, wxID_ANY, GetEyeToolIcon(16, 16), "Show / Hide Layer");
        row.lockButton = new IconButton(row.panel, wxID_ANY, GetLockToolIcon(16, 16), "Lock / Unlock Layer");

        row.visibleButton->SetButtonSize(wxSize(24, 24));
        row.lockButton->SetButtonSize(wxSize(24, 24));

        row.visibleButton->SetBackgroundColour(row.panel->GetBackgroundColour());
        row.lockButton->SetBackgroundColour(row.panel->GetBackgroundColour());

        row.preview = new wxStaticBitmap(row.panel, wxID_ANY, MakeLayerPreviewBitmap(i), wxDefaultPosition, wxSize(36, 22));
        row.preview->SetBackgroundColour(row.panel->GetBackgroundColour());

        row.nameText = new wxStaticText(row.panel, wxID_ANY, layers[i].name);
        row.nameText->SetForegroundColour(*wxWHITE);

        row.nameEdit = new wxTextCtrl(row.panel, wxID_ANY, layers[i].name, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        row.nameEdit->Hide();

        row.visibleButton->SetClientData(LayerIndexToPtr(i));
        row.lockButton->SetClientData(LayerIndexToPtr(i));
        row.panel->SetClientData(LayerIndexToPtr(i));
        row.preview->SetClientData(LayerIndexToPtr(i));
        row.nameText->SetClientData(LayerIndexToPtr(i));
        row.nameEdit->SetClientData(LayerIndexToPtr(i));

        row.nameEdit->Bind(wxEVT_TEXT_ENTER, &Layer::OnInlineRenameEnter, this);
        row.nameEdit->Bind(wxEVT_KILL_FOCUS, &Layer::OnInlineRenameKillFocus, this);

        row.visibleButton->Bind(wxEVT_BUTTON, &Layer::OnToggleLayerVisible, this);
        row.lockButton->Bind(wxEVT_BUTTON, &Layer::OnToggleLayerLocked, this);

        row.panel->Bind(wxEVT_LEFT_DOWN, &Layer::OnLayerRowClicked, this);
        row.preview->Bind(wxEVT_LEFT_DOWN, &Layer::OnLayerRowClicked, this);
        row.nameText->Bind(wxEVT_LEFT_DOWN, &Layer::OnLayerRowClicked, this);

        row.panel->Bind(wxEVT_RIGHT_DOWN, &Layer::OnLayerRowRightClick, this);
        row.preview->Bind(wxEVT_RIGHT_DOWN, &Layer::OnLayerRowRightClick, this);
        row.nameText->Bind(wxEVT_RIGHT_DOWN, &Layer::OnLayerRowRightClick, this);

        rowSizer->Add(row.visibleButton, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
        rowSizer->Add(row.lockButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        rowSizer->Add(row.preview, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        rowSizer->Add(row.nameText, 1, wxALIGN_CENTER_VERTICAL);
        rowSizer->Add(row.nameEdit, 1, wxALIGN_CENTER_VERTICAL);

        row.panel->SetSizer(rowSizer);
        m_layerListSizer->Add(row.panel, 0, wxEXPAND);

        wxPanel* separator = new wxPanel(m_layerScroll, wxID_ANY);
        separator->SetMinSize(wxSize(-1, 1));
        separator->SetBackgroundColour(wxColour(45, 45, 48));
        m_layerListSizer->Add(separator, 0, wxEXPAND);

        m_rows.push_back(row);
    }

    if (m_layerScroll)
    {
        m_layerScroll->Layout();
        m_layerScroll->FitInside();
    }
}

wxBitmap Layer::MakeEmptyPreviewBitmap() const
{
    wxBitmap bmp(36, 22);
    wxMemoryDC dc(bmp);

    dc.SetBackground(wxBrush(wxColour(80, 80, 80)));
    dc.Clear();

    dc.SetPen(wxPen(wxColour(35, 35, 35), 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(0, 0, 36, 22);

    dc.SelectObject(wxNullBitmap);
    return bmp;
}

wxBitmap Layer::MakeLayerPreviewBitmap(int documentLayerIndex) const
{
    if (!m_currentDocument)
        return MakeEmptyPreviewBitmap();

    const DocumentLayer* layer = m_currentDocument->GetLayer(documentLayerIndex);

    if (!layer || !layer->image.IsOk() || layer->image.GetWidth() <= 0 || layer->image.GetHeight() <= 0)
        return MakeEmptyPreviewBitmap();

    wxImage image = layer->image.Copy();

    if (!image.IsOk() || !image.GetData())
        return MakeEmptyPreviewBitmap();

    unsigned char* data = image.GetData();
    const int w = image.GetWidth();
    const int h = image.GetHeight();

    if (image.HasAlpha())
    {
        unsigned char* alpha = image.GetAlpha();

        if (alpha)
        {
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    const int p = y * w + x;
                    const int i = p * 3;
                    const int a = alpha[p];

                    data[i + 0] = static_cast<unsigned char>((data[i + 0] * a + 80 * (255 - a)) / 255);
                    data[i + 1] = static_cast<unsigned char>((data[i + 1] * a + 80 * (255 - a)) / 255);
                    data[i + 2] = static_cast<unsigned char>((data[i + 2] * a + 80 * (255 - a)) / 255);
                    alpha[p] = 255;
                }
            }
        }
    }

    wxImage scaled = image.Scale(36, 22, wxIMAGE_QUALITY_HIGH);

    if (!scaled.IsOk())
        return MakeEmptyPreviewBitmap();

    return wxBitmap(scaled);
}

void Layer::UpdateTopButtonsFromDocument()
{
    if (!m_allVisibleButton || !m_allLockButton)
        return;

    if (!m_currentDocument || m_currentDocument->GetLayerCount() <= 0)
    {
        m_allVisibleButton->Enable(false);
        m_allLockButton->Enable(false);
        m_allVisibleButton->SetIcon(MakeBlankIcon());
        m_allLockButton->SetIcon(MakeBlankIcon());
        return;
    }

    m_allVisibleButton->Enable(true);
    m_allLockButton->Enable(true);

    const bool allVisible = m_currentDocument->AreAllLayersVisible();
    const bool allLocked = m_currentDocument->AreAllLayersLocked();

    m_allVisibleButton->SetIcon(allVisible ? GetEyeToolIcon(16, 16) : MakeBlankIcon());
    m_allLockButton->SetIcon(allLocked ? GetLockToolIcon(16, 16) : MakeBlankIcon());

    m_allVisibleButton->SetToolTip(allVisible ? "Hide All Layers" : "Show All Layers");
    m_allLockButton->SetToolTip(allLocked ? "Unlock All Layers" : "Lock All Layers");
}

// ----------------------------------------------------------------------------
// Layer update color
// ----------------------------------------------------------------------------
void Layer::UpdateRowButtonsFromDocument()
{
    if (!m_currentDocument)
        return;

    const int selectedLayer = m_currentDocument->GetSelectedLayer();

    if (selectedLayer >= 0)
        SetOpacityValueUI(m_currentDocument->GetLayerOpacity(selectedLayer));
    else
        SetOpacityValueUI(100);

    for (LayerRow& row : m_rows)
    {
        const bool validLayer = row.documentLayerIndex >= 0 && row.documentLayerIndex < m_currentDocument->GetLayerCount();

        if (!validLayer)
            continue;

        const bool selected = row.documentLayerIndex == selectedLayer;
        const bool visible = m_currentDocument->IsLayerVisible(row.documentLayerIndex);
        const bool locked = m_currentDocument->IsLayerLocked(row.documentLayerIndex);

        if (row.panel)
            row.panel->SetBackgroundColour(selected ? wxColour(74, 92, 120) : m_layerScroll->GetBackgroundColour());

        if (row.visibleButton)
        {
            row.visibleButton->SetIcon(visible ? GetEyeToolIcon(16, 16) : MakeBlankIcon());
            row.visibleButton->SetBackgroundColour(row.panel->GetBackgroundColour());
        }

        if (row.lockButton)
        {
            row.lockButton->SetIcon(locked ? GetLockToolIcon(16, 16) : MakeBlankIcon());
            row.lockButton->SetBackgroundColour(row.panel->GetBackgroundColour());
        }

        if (row.preview)
        {
            row.preview->SetBitmap(MakeLayerPreviewBitmap(row.documentLayerIndex));
            row.preview->SetBackgroundColour(row.panel->GetBackgroundColour());
        }

        if (row.nameText)
        {
            row.nameText->SetLabel(m_currentDocument->GetLayer(row.documentLayerIndex)->name);
            row.nameText->SetForegroundColour(*wxWHITE);
            row.nameText->Refresh();
        }

        if (row.nameEdit)
            row.nameEdit->SetBackgroundColour(wxColour(35, 35, 38));

        if (row.panel)
            row.panel->Refresh();

        if (row.visibleButton)
            row.visibleButton->Refresh();

        if (row.lockButton)
            row.lockButton->Refresh();

        if (row.preview)
            row.preview->Refresh();
    }

    if (m_layerScroll)
    {
        m_layerScroll->Layout();
        m_layerScroll->FitInside();
        m_layerScroll->Refresh();
    }
}

void Layer::OnToggleAllVisible(wxCommandEvent& event)
{
    if (!m_currentDocument)
    {
        event.Skip();
        return;
    }

    const bool allVisible = m_currentDocument->AreAllLayersVisible();
    m_currentDocument->SetAllLayersVisible(!allVisible);

    UpdateTopButtonsFromDocument();
    UpdateRowButtonsFromDocument();
    RefreshMainFrameDocumentUI();
}

void Layer::OnToggleAllLocked(wxCommandEvent& event)
{
    if (!m_currentDocument)
    {
        event.Skip();
        return;
    }

    const bool allLocked = m_currentDocument->AreAllLayersLocked();
    m_currentDocument->SetAllLayersLocked(!allLocked);

    UpdateTopButtonsFromDocument();
    UpdateRowButtonsFromDocument();
    RefreshMainFrameDocumentUI();
}

void Layer::OnLayerRowClicked(wxMouseEvent& event)
{
    if (!m_currentDocument)
    {
        event.Skip();
        return;
    }

    wxWindow* src = dynamic_cast<wxWindow*>(event.GetEventObject());
    if (!src)
    {
        event.Skip();
        return;
    }

    const int index = PtrToLayerIndex(src->GetClientData());
    m_currentDocument->SetSelectedLayer(index);

    UpdateRowButtonsFromDocument();
}

void Layer::OnToggleLayerVisible(wxCommandEvent& event)
{
    if (!m_currentDocument)
    {
        event.Skip();
        return;
    }

    wxWindow* src = dynamic_cast<wxWindow*>(event.GetEventObject());
    if (!src)
    {
        event.Skip();
        return;
    }

    const int index = PtrToLayerIndex(src->GetClientData());
    const bool visible = m_currentDocument->IsLayerVisible(index);
    m_currentDocument->SetLayerVisible(index, !visible);

    UpdateRowButtonsFromDocument();
}

void Layer::OnToggleLayerLocked(wxCommandEvent& event)
{
    if (!m_currentDocument)
    {
        event.Skip();
        return;
    }

    wxWindow* src = dynamic_cast<wxWindow*>(event.GetEventObject());
    if (!src)
    {
        event.Skip();
        return;
    }

    const int index = PtrToLayerIndex(src->GetClientData());
    const bool locked = m_currentDocument->IsLayerLocked(index);
    m_currentDocument->SetLayerLocked(index, !locked);

    UpdateRowButtonsFromDocument();
}

// ----------------------------------------------------------------------------
// Layer right click
// ----------------------------------------------------------------------------
void Layer::OnLayerRowRightClick(wxMouseEvent& event)
{
    if (!m_currentDocument)
    {
        event.Skip();
        return;
    }

    wxWindow* src = dynamic_cast<wxWindow*>(event.GetEventObject());
    if (!src)
    {
        event.Skip();
        return;
    }

    const int index = PtrToLayerIndex(src->GetClientData());
    m_currentDocument->SetSelectedLayer(index);
    UpdateRowButtonsFromDocument();

    ShowLayerContextMenu(src, src->ClientToScreen(event.GetPosition()));
}

void Layer::ShowLayerContextMenu(wxWindow* sourceWindow, const wxPoint& screenPos)
{
    if (!sourceWindow)
        return;

    wxMenu menu;
    menu.Append(ID_LAYER_MENU_ADD, "Add Layer");
    menu.Append(ID_LAYER_MENU_DELETE, "Delete Layer");
    menu.Append(ID_LAYER_MENU_DUPLICATE, "Duplicate Layer");
    menu.AppendSeparator();
    menu.Append(ID_LAYER_MENU_RENAME, "Rename Layer");
    menu.Append(ID_LAYER_MENU_MERGE, "Merge Layer");

    sourceWindow->PopupMenu(&menu, sourceWindow->ScreenToClient(screenPos));
}

void Layer::OnAddLayerMenu(wxCommandEvent& event)
{
    if (!m_currentDocument)
    {
        event.Skip();
        return;
    }

    m_currentDocument->AddLayer();

    RebuildLayerRows();
    UpdateTopButtonsFromDocument();
    UpdateRowButtonsFromDocument();
    RefreshMainFrameDocumentUI();
}

void Layer::OnDeleteLayerMenu(wxCommandEvent& event)
{
    if (!m_currentDocument)
    {
        event.Skip();
        return;
    }

    if (m_currentDocument->GetLayerCount() <= 1)
    {
        wxMessageBox("You cannot delete the last layer.", "Delete Layer", wxOK | wxICON_INFORMATION, this);
        return;
    }

    if (!m_currentDocument->SelectedLayerHasAnyPixel())
    {
        m_currentDocument->DeleteSelectedLayer();
        RebuildLayerRows();
        UpdateTopButtonsFromDocument();
        UpdateRowButtonsFromDocument();
        RefreshMainFrameDocumentUI();
        return;
    }

    const int result = wxMessageBox("Do you really want to delete it?", "Delete Layer", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);

    if (result != wxYES)
        return;

    m_currentDocument->DeleteSelectedLayer();
    RebuildLayerRows();
    UpdateTopButtonsFromDocument();
    UpdateRowButtonsFromDocument();
    RefreshMainFrameDocumentUI();
}

void Layer::OnDuplicateLayerMenu(wxCommandEvent& event)
{
    if (!m_currentDocument)
    {
        event.Skip();
        return;
    }

    m_currentDocument->DuplicateSelectedLayer();

    RebuildLayerRows();
    UpdateTopButtonsFromDocument();
    UpdateRowButtonsFromDocument();
    RefreshMainFrameDocumentUI();
}

void Layer::OnRenameLayerMenu(wxCommandEvent& event)
{
    if (!m_currentDocument)
    {
        event.Skip();
        return;
    }

    BeginInlineRename(m_currentDocument->GetSelectedLayer());
}

void Layer::OnMergeLayerMenu(wxCommandEvent& event)
{
    event.Skip(false);
}

void Layer::BeginInlineRename(int documentLayerIndex)
{
    if (!m_currentDocument)
        return;

    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        LayerRow& row = m_rows[i];

        if (!row.nameText || !row.nameEdit)
            continue;

        if (row.documentLayerIndex == documentLayerIndex)
        {
            row.nameEdit->SetValue(row.nameText->GetLabel());
            row.nameText->Hide();
            row.nameEdit->Show();
            row.panel->Layout();
            row.nameEdit->SetFocus();
            row.nameEdit->SetSelection(-1, -1);
        }
        else
        {
            row.nameEdit->Hide();
            row.nameText->Show();
        }
    }

    if (m_layerScroll)
        m_layerScroll->Layout();
}

void Layer::CommitInlineRename(wxWindow* sourceWindow)
{
    if (m_committingRename)
        return;

    if (!m_currentDocument || !sourceWindow)
        return;

    wxTextCtrl* edit = wxDynamicCast(sourceWindow, wxTextCtrl);
    if (!edit)
        return;

    m_committingRename = true;

    const int documentLayerIndex = PtrToLayerIndex(edit->GetClientData());
    wxString newName = edit->GetValue();
    newName.Trim(true).Trim(false);

    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        LayerRow& row = m_rows[i];

        if (row.documentLayerIndex != documentLayerIndex)
            continue;

        if (!row.nameEdit || !row.nameText || !row.panel)
            break;

        if (!newName.IsEmpty())
        {
            m_currentDocument->SetSelectedLayer(documentLayerIndex);
            m_currentDocument->SetSelectedLayerName(newName);
            row.nameText->SetLabel(newName);
        }

        row.nameEdit->Hide();
        row.nameText->Show();
        row.panel->Layout();

        break;
    }

    UpdateRowButtonsFromDocument();
    RefreshMainFrameDocumentUI();

    if (m_layerScroll)
        m_layerScroll->Layout();

    m_committingRename = false;
}

void Layer::CancelInlineRename()
{
    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        LayerRow& row = m_rows[i];
        if (!row.nameText || !row.nameEdit)
            continue;

        row.nameEdit->Hide();
        row.nameText->Show();
        row.panel->Layout();
    }

    if (m_layerScroll)
        m_layerScroll->Layout();
}

void Layer::OnInlineRenameEnter(wxCommandEvent& event)
{
    CommitInlineRename(dynamic_cast<wxWindow*>(event.GetEventObject()));
    event.Skip(false);
}

void Layer::OnInlineRenameKillFocus(wxFocusEvent& event)
{
    if (!m_committingRename)
        CommitInlineRename(dynamic_cast<wxWindow*>(event.GetEventObject()));

    event.Skip();
}



