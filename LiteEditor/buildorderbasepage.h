//////////////////////////////////////////////////////////////////////
// This file was auto-generated by codelite's wxCrafter Plugin
// wxCrafter project file: depend_dlg_page.wxcp
// Do not modify this file by hand!
//////////////////////////////////////////////////////////////////////

#ifndef _CODELITE_LITEEDITOR_DEPEND_DLG_PAGE_BASE_CLASSES_H
#define _CODELITE_LITEEDITOR_DEPEND_DLG_PAGE_BASE_CLASSES_H

// clang-format off
#include <wx/settings.h>
#include <wx/xrc/xmlres.h>
#include <wx/xrc/xh_bmp.h>
#include <wx/dialog.h>
#include <wx/iconbndl.h>
#include <wx/artprov.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/choice.h>
#include <wx/arrstr.h>
#include <wx/dataview.h>
#include "clThemedListCtrl.h"
#include <wx/button.h>
#if wxVERSION_NUMBER >= 2900
#include <wx/persist.h>
#include <wx/persist/toplevel.h>
#include <wx/persist/bookctrl.h>
#include <wx/persist/treebook.h>
#endif

#ifdef WXC_FROM_DIP
#undef WXC_FROM_DIP
#endif
#if wxVERSION_NUMBER >= 3100
#define WXC_FROM_DIP(x) wxWindow::FromDIP(x, NULL)
#else
#define WXC_FROM_DIP(x) x
#endif

// clang-format on

class BuildOrderDialogBase : public wxDialog
{
protected:
    wxStaticText* m_staticText3;
    wxChoice* m_choiceProjectConfig;
    clThemedOrderedListCtrl* m_dvListCtrlProjects;
    wxButton* m_button22;
    wxButton* m_button24;
    clThemedListCtrl* m_dvListCtrlBuildOrder;
    wxButton* m_buttonUp;
    wxButton* m_buttonDown;
    wxButton* m_buttonApply;
    wxStdDialogButtonSizer* m_stdBtnSizer52;
    wxButton* m_button54;
    wxButton* m_button56;

protected:
    virtual void OnConfigChanged(wxCommandEvent& event) { event.Skip(); }
    virtual void OnMoveRight(wxCommandEvent& event) { event.Skip(); }
    virtual void OnMoveRightUI(wxUpdateUIEvent& event) { event.Skip(); }
    virtual void OnMoveLeftUI(wxUpdateUIEvent& event) { event.Skip(); }
    virtual void OnMoveLeft(wxCommandEvent& event) { event.Skip(); }
    virtual void OnMoveUp(wxCommandEvent& event) { event.Skip(); }
    virtual void OnMoveDown(wxCommandEvent& event) { event.Skip(); }
    virtual void OnApplyButton(wxCommandEvent& event) { event.Skip(); }
    virtual void OnApplyButtonUI(wxUpdateUIEvent& event) { event.Skip(); }
    virtual void OnButtonOK(wxCommandEvent& event) { event.Skip(); }

public:
    wxStaticText* GetStaticText3() { return m_staticText3; }
    wxChoice* GetChoiceProjectConfig() { return m_choiceProjectConfig; }
    clThemedOrderedListCtrl* GetDvListCtrlProjects() { return m_dvListCtrlProjects; }
    wxButton* GetButton22() { return m_button22; }
    wxButton* GetButton24() { return m_button24; }
    clThemedListCtrl* GetDvListCtrlBuildOrder() { return m_dvListCtrlBuildOrder; }
    wxButton* GetButtonUp() { return m_buttonUp; }
    wxButton* GetButtonDown() { return m_buttonDown; }
    wxButton* GetButtonApply() { return m_buttonApply; }
    BuildOrderDialogBase(wxWindow* parent, wxWindowID id = wxID_ANY, const wxString& title = wxT("Build Order"),
                         const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxSize(-1, -1),
                         long style = wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    virtual ~BuildOrderDialogBase();
};

#endif
