#pragma once

#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <vector>

class Document;

class History : public wxPanel
{
public:
    explicit History(wxWindow* parent);

    void RefreshFromDocument(Document* doc);

private:
    void ClearRows();
    void AddHistoryRow(const wxString& label, bool selected);
    void SetRowSelected(size_t index, bool selected);
    bool CanAppendOnly(const std::vector<wxString>& labels) const;

private:
    wxScrolledWindow* m_historyScroll = nullptr;
    wxBoxSizer* m_historySizer = nullptr;

    std::vector<wxPanel*> m_rows;
    std::vector<wxStaticText*> m_rowTexts;
    std::vector<wxString> m_labelCache;
};
