#include "History.h"
#include "Document.h"
#include "Theme.h"

#include <wx/sizer.h>

History::History(wxWindow* parent)
: wxPanel(parent, wxID_ANY)
{
    wxBoxSizer* rootSizer = new wxBoxSizer(wxVERTICAL);

    wxPanel* borderPanel = new wxPanel(this, wxID_ANY);
    borderPanel->SetBackgroundColour(wxColour(45, 45, 48));

    m_historyScroll = new wxScrolledWindow(borderPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_historyScroll->SetScrollRate(0, 10);
    m_historyScroll->SetBackgroundColour(wxColour(65, 65, 68));

    m_historySizer = new wxBoxSizer(wxVERTICAL);
    m_historyScroll->SetSizer(m_historySizer);

    wxBoxSizer* borderSizer = new wxBoxSizer(wxVERTICAL);
    borderSizer->Add(m_historyScroll, 1, wxEXPAND | wxALL, 1);
    borderPanel->SetSizer(borderSizer);

    rootSizer->Add(borderPanel, 1, wxEXPAND | wxALL, 10);
    SetSizer(rootSizer);
}

void History::ClearRows()
{
    if (!m_historySizer)
        return;

    m_historySizer->Clear(true);
    m_rows.clear();
    m_rowTexts.clear();
    m_labelCache.clear();
}

void History::SetRowSelected(size_t index, bool selected)
{
    if (index >= m_rows.size() || index >= m_rowTexts.size())
        return;

    const ThemeColors& colors = Theme::Get(AppTheme::Dark);

    if (selected)
    {
        m_rows[index]->SetBackgroundColour(colors.selected);
        m_rowTexts[index]->SetForegroundColour(*wxBLACK);
    }
    else
    {
        m_rows[index]->SetBackgroundColour(wxColour(85, 85, 90));
        m_rowTexts[index]->SetForegroundColour(*wxWHITE);
    }

    m_rows[index]->Refresh(false);
    m_rowTexts[index]->Refresh(false);
}

void History::AddHistoryRow(const wxString& label, bool selected)
{
    if (!m_historyScroll || !m_historySizer)
        return;

    wxPanel* row = new wxPanel(m_historyScroll, wxID_ANY);
    row->SetMinSize(wxSize(-1, 28));

    wxBoxSizer* rowSizer = new wxBoxSizer(wxHORIZONTAL);

    wxStaticText* text = new wxStaticText(row, wxID_ANY, label);
    rowSizer->Add(text, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);

    row->SetSizer(rowSizer);

    m_historySizer->Add(row, 0, wxEXPAND);

    wxPanel* separator = new wxPanel(m_historyScroll, wxID_ANY);
    separator->SetMinSize(wxSize(-1, 1));
    separator->SetBackgroundColour(wxColour(45, 45, 48));
    m_historySizer->Add(separator, 0, wxEXPAND);

    m_rows.push_back(row);
    m_rowTexts.push_back(text);

    SetRowSelected(m_rows.size() - 1, selected);
}

bool History::CanAppendOnly(const std::vector<wxString>& labels) const
{
    if (labels.size() != m_labelCache.size() + 1)
        return false;

    for (size_t i = 0; i < m_labelCache.size(); ++i)
    {
        if (labels[i] != m_labelCache[i])
            return false;
    }

    return true;
}

void History::RefreshFromDocument(Document* doc)
{
    if (!m_historySizer || !m_historyScroll)
        return;

    if (!doc)
    {
        Freeze();
        ClearRows();
        m_historyScroll->Layout();
        m_historyScroll->FitInside();
        Thaw();
        return;
    }

    const std::vector<wxString>& labels = doc->GetHistoryLabels();

    if (labels == m_labelCache)
        return;

    if (CanAppendOnly(labels))
    {
        Freeze();

        if (!m_rows.empty())
            SetRowSelected(m_rows.size() - 1, false);

        AddHistoryRow(labels.back(), true);

        m_labelCache = labels;

        m_historyScroll->Layout();
        m_historyScroll->FitInside();

        const int scrollY = m_historyScroll->GetVirtualSize().GetHeight();
        m_historyScroll->Scroll(-1, scrollY);

        Thaw();
        return;
    }

    Freeze();

    ClearRows();

    for (size_t i = 0; i < labels.size(); ++i)
        AddHistoryRow(labels[i], i == labels.size() - 1);

    m_labelCache = labels;

    m_historyScroll->Layout();
    m_historyScroll->FitInside();

    const int scrollY = m_historyScroll->GetVirtualSize().GetHeight();
    m_historyScroll->Scroll(-1, scrollY);

    Thaw();
}
