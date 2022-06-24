//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
//
// copyright            : (C) 2014 Eran Ifrah
// file name            : LLDBPlugin.cpp
//
// -------------------------------------------------------------------------
// A
//              _____           _      _     _ _
//             /  __ \         | |    | |   (_) |
//             | /  \/ ___   __| | ___| |    _| |_ ___
//             | |    / _ \ / _  |/ _ \ |   | | __/ _ )
//             | \__/\ (_) | (_| |  __/ |___| | ||  __/
//              \____/\___/ \__,_|\___\_____/_|\__\___|
//
//                                                  F i l e
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

#include "DebugAdapterClient.hpp"

#include "DAPMainView.h"
#include "DapDebuggerSettingsDlg.h"
#include "DapLoggingHelper.hpp"
#include "StringUtils.h"
#include "asyncprocess.h"
#include "bookmark_manager.h"
#include "clFileSystemWorkspace.hpp"
#include "clcommandlineparser.h"
#include "console_frame.h"
#include "debuggermanager.h"
#include "dirsaver.h"
#include "environmentconfig.h"
#include "event_notifier.h"
#include "file_logger.h"
#include "globals.h"
#include "macromanager.h"
#include "processreaderthread.h"

#include <wx/aui/framemanager.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/platinfo.h>
#include <wx/stc/stc.h>
#include <wx/xrc/xmlres.h>

static DebugAdapterClient* thePlugin = NULL;

namespace
{

#ifdef __WXMSW__
constexpr bool IS_WINDOWS = true;
#else
constexpr bool IS_WINDOWS = false;
#endif

const wxString DAP_MAIN_VIEW = _("Thread, stacks & variables");
const wxString DAP_MESSAGE_BOX_TITLE = "CodeLite - Debug Adapter Client";

// Reusing gdb ids so global debugger menu and accelerators work.
const int lldbRunToCursorContextMenuId = XRCID("dbg_run_to_cursor");
const int lldbJumpToCursorContextMenuId = XRCID("dbg_jump_cursor");
const int lldbAddWatchContextMenuId = XRCID("lldb_add_watch");

#if 0
wxString GetWatchWord(IEditor& editor)
{
    auto word = editor.GetSelection();
    if(word.IsEmpty()) {
        word = editor.GetWordAtCaret();
    }

    // Remove leading and trailing whitespace.
    word.Trim(true);
    word.Trim(false);

    return word;
}
#endif

// helper methods converting between clDebuggerBreakpoint and dap::XXXBreakpoint
bool is_source_breakpoint(const clDebuggerBreakpoint& bp)
{
    return bp.bp_type == BreakpointType::BP_type_break && !bp.file.empty() && bp.lineno > 0;
}

bool is_function_breakpoint(const clDebuggerBreakpoint& bp)
{
    return bp.bp_type == BreakpointType::BP_type_break && !bp.function_name.empty();
}

dap::SourceBreakpoint to_dap_source_bp(const clDebuggerBreakpoint& bp)
{
    dap::SourceBreakpoint d;
    d.line = bp.lineno;
    d.condition = bp.conditions;
    return d;
}

dap::FunctionBreakpoint to_dap_function_bp(const clDebuggerBreakpoint& bp)
{
    dap::FunctionBreakpoint d;
    d.name = bp.function_name;
    d.condition = bp.conditions;
    return d;
}

std::vector<wxString> to_string_array(const clEnvList_t& env_list)
{
    std::vector<wxString> arr;
    arr.reserve(env_list.size());
    for(const auto& vt : env_list) {
        arr.emplace_back(vt.first + "=" + vt.second);
    }
    return arr;
}

} // namespace

#define CHECK_IS_DAP_CONNECTED()  \
    if(!m_client.IsConnected()) { \
        event.Skip();             \
        return;                   \
    }

// Define the plugin entry point
CL_PLUGIN_API IPlugin* CreatePlugin(IManager* manager)
{
    if(thePlugin == 0) {
        thePlugin = new DebugAdapterClient(manager);
    }
    return thePlugin;
}

CL_PLUGIN_API PluginInfo* GetPluginInfo()
{
    static PluginInfo info;
    info.SetAuthor(wxT("eran"));
    info.SetName(wxT("DebugAdapterClient"));
    info.SetDescription(_("Debug Adapter Client"));
    info.SetVersion(wxT("v1.0"));
    return &info;
}

CL_PLUGIN_API int GetPluginInterfaceVersion() { return PLUGIN_INTERFACE_VERSION; }

DebugAdapterClient::DebugAdapterClient(IManager* manager)
    : IPlugin(manager)
    , m_terminal_helper(LOG)
    , m_isPerspectiveLoaded(false)
{
    // setup custom logger for this module
    wxFileName logfile(clStandardPaths::Get().GetUserDataDir(), "debug-adapter-client.log");

    LOG.Open(logfile);
    LOG.SetCurrentLogLevel(FileLogger::Dbg);
    LOG_DEBUG(LOG) << "Debug Adapter Client startd" << endl;
    m_longName = _("Debug Adapter Client");
    m_shortName = wxT("DebugAdapterClient");

    // load settings
    wxFileName configuration_file(clStandardPaths::Get().GetUserDataDir(), "debug-adapter-client.conf");
    configuration_file.AppendDir("config");
    m_dap_store.Load(configuration_file);

    RegisterDebuggers();

    Bind(wxEVT_ASYNC_PROCESS_OUTPUT, &DebugAdapterClient::OnProcessOutput, this);
    Bind(wxEVT_ASYNC_PROCESS_TERMINATED, &DebugAdapterClient::OnProcessTerminated, this);

    // UI events
    EventNotifier::Get()->Bind(wxEVT_FILE_LOADED, &DebugAdapterClient::OnFileLoaded, this);
    EventNotifier::Get()->Bind(wxEVT_WORKSPACE_LOADED, &DebugAdapterClient::OnWorkspaceLoaded, this);
    EventNotifier::Get()->Bind(wxEVT_WORKSPACE_CLOSED, &DebugAdapterClient::OnWorkspaceClosed, this);

    EventNotifier::Get()->Bind(wxEVT_DBG_UI_START, &DebugAdapterClient::OnDebugStart, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_CONTINUE, &DebugAdapterClient::OnDebugContinue, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_NEXT, &DebugAdapterClient::OnDebugNext, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_STEP_IN, &DebugAdapterClient::OnDebugStepIn, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_STEP_OUT, &DebugAdapterClient::OnDebugStepOut, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_STOP, &DebugAdapterClient::OnDebugStop, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_IS_RUNNING, &DebugAdapterClient::OnDebugIsRunning, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_CAN_INTERACT, &DebugAdapterClient::OnDebugCanInteract, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_TOGGLE_BREAKPOINT, &DebugAdapterClient::OnToggleBreakpoint, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_INTERRUPT, &DebugAdapterClient::OnToggleInterrupt, this);
    EventNotifier::Get()->Bind(wxEVT_BUILD_STARTING, &DebugAdapterClient::OnBuildStarting, this);
    EventNotifier::Get()->Bind(wxEVT_INIT_DONE, &DebugAdapterClient::OnInitDone, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_EXPR_TOOLTIP, &DebugAdapterClient::OnDebugTooltip, this);
    EventNotifier::Get()->Bind(wxEVT_QUICK_DEBUG, &DebugAdapterClient::OnDebugQuickDebug, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_CORE_FILE, &DebugAdapterClient::OnDebugCoreFile, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_DELETE_ALL_BREAKPOINTS, &DebugAdapterClient::OnDebugDeleteAllBreakpoints,
                               this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_ATTACH_TO_PROCESS, &DebugAdapterClient::OnDebugAttachToProcess, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_ENABLE_ALL_BREAKPOINTS, &DebugAdapterClient::OnDebugEnableAllBreakpoints,
                               this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_DISABLE_ALL_BREAKPOINTS, &DebugAdapterClient::OnDebugDisableAllBreakpoints,
                               this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_NEXT_INST, &DebugAdapterClient::OnDebugNextInst, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_STEP_I, &DebugAdapterClient::OnDebugVOID, this); // not supported

    EventNotifier::Get()->Bind(wxEVT_DBG_UI_SHOW_CURSOR, &DebugAdapterClient::OnDebugShowCursor, this);
    wxTheApp->Bind(wxEVT_MENU, &DebugAdapterClient::OnSettings, this, XRCID("lldb_settings"));

    EventNotifier::Get()->Bind(wxEVT_COMMAND_MENU_SELECTED, &DebugAdapterClient::OnRunToCursor, this,
                               lldbRunToCursorContextMenuId);
    EventNotifier::Get()->Bind(wxEVT_COMMAND_MENU_SELECTED, &DebugAdapterClient::OnJumpToCursor, this,
                               lldbJumpToCursorContextMenuId);

    wxTheApp->Bind(wxEVT_COMMAND_MENU_SELECTED, &DebugAdapterClient::OnAddWatch, this, lldbAddWatchContextMenuId);

    dap::Initialize(); // register all dap objects

    m_client.SetWantsLogEvents(true);
    m_client.Bind(wxEVT_DAP_INITIALIZE_RESPONSE, &DebugAdapterClient::OnInitializeResponse, this);
    m_client.Bind(wxEVT_DAP_INITIALIZED_EVENT, &DebugAdapterClient::OnInitializedEvent, this);
    m_client.Bind(wxEVT_DAP_RUN_IN_TERMINAL_REQUEST, &DebugAdapterClient::OnRunInTerminal, this);
    m_client.Bind(wxEVT_DAP_EXITED_EVENT, &DebugAdapterClient::OnDapExited, this);
    m_client.Bind(wxEVT_DAP_TERMINATED_EVENT, &DebugAdapterClient::OnDapExited, this);
    m_client.Bind(wxEVT_DAP_LAUNCH_RESPONSE, &DebugAdapterClient::OnLaunchResponse, this);
    m_client.Bind(wxEVT_DAP_STOPPED_EVENT, &DebugAdapterClient::OnStoppedEvent, this);
    m_client.Bind(wxEVT_DAP_THREADS_RESPONSE, &DebugAdapterClient::OnThreadsResponse, this);
    m_client.Bind(wxEVT_DAP_STACKTRACE_RESPONSE, &DebugAdapterClient::OnStackTraceResponse, this);
    m_client.Bind(wxEVT_DAP_SET_FUNCTION_BREAKPOINT_RESPONSE, &DebugAdapterClient::OnSetFunctionBreakpointResponse,
                  this);
    m_client.Bind(wxEVT_DAP_SET_SOURCE_BREAKPOINT_RESPONSE, &DebugAdapterClient::OnSetSourceBreakpointResponse, this);
    m_client.Bind(wxEVT_DAP_LOG_EVENT, &DebugAdapterClient::OnDapLog, this);
}

void DebugAdapterClient::RegisterDebuggers()
{
    wxArrayString debuggers;
    debuggers.reserve(m_dap_store.GetEntries().size());
    for(const auto& entry : m_dap_store.GetEntries()) {
        debuggers.Add(entry.first);
    }
    DebuggerMgr::Get().RegisterDebuggers(m_shortName, debuggers);
}

void DebugAdapterClient::UnPlug()
{
    DestroyUI();
    DebuggerMgr::Get().UnregisterDebuggers(m_shortName);

    // UI events
    EventNotifier::Get()->Unbind(wxEVT_FILE_LOADED, &DebugAdapterClient::OnFileLoaded, this);
    EventNotifier::Get()->Unbind(wxEVT_WORKSPACE_LOADED, &DebugAdapterClient::OnWorkspaceLoaded, this);
    EventNotifier::Get()->Unbind(wxEVT_WORKSPACE_CLOSED, &DebugAdapterClient::OnWorkspaceClosed, this);

    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_START, &DebugAdapterClient::OnDebugStart, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_CONTINUE, &DebugAdapterClient::OnDebugContinue, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_NEXT, &DebugAdapterClient::OnDebugNext, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_STOP, &DebugAdapterClient::OnDebugStop, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_IS_RUNNING, &DebugAdapterClient::OnDebugIsRunning, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_CAN_INTERACT, &DebugAdapterClient::OnDebugCanInteract, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_STEP_IN, &DebugAdapterClient::OnDebugStepIn, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_STEP_OUT, &DebugAdapterClient::OnDebugStepOut, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_TOGGLE_BREAKPOINT, &DebugAdapterClient::OnToggleBreakpoint, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_INTERRUPT, &DebugAdapterClient::OnToggleInterrupt, this);
    EventNotifier::Get()->Unbind(wxEVT_BUILD_STARTING, &DebugAdapterClient::OnBuildStarting, this);
    EventNotifier::Get()->Unbind(wxEVT_INIT_DONE, &DebugAdapterClient::OnInitDone, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_EXPR_TOOLTIP, &DebugAdapterClient::OnDebugTooltip, this);
    EventNotifier::Get()->Unbind(wxEVT_QUICK_DEBUG, &DebugAdapterClient::OnDebugQuickDebug, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_CORE_FILE, &DebugAdapterClient::OnDebugCoreFile, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_DELETE_ALL_BREAKPOINTS, &DebugAdapterClient::OnDebugDeleteAllBreakpoints,
                                 this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_ATTACH_TO_PROCESS, &DebugAdapterClient::OnDebugAttachToProcess, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_ENABLE_ALL_BREAKPOINTS, &DebugAdapterClient::OnDebugEnableAllBreakpoints,
                                 this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_DISABLE_ALL_BREAKPOINTS,
                                 &DebugAdapterClient::OnDebugDisableAllBreakpoints, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_STEP_I, &DebugAdapterClient::OnDebugVOID, this); // Not supported
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_NEXT_INST, &DebugAdapterClient::OnDebugNextInst, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_SHOW_CURSOR, &DebugAdapterClient::OnDebugShowCursor, this);
    wxTheApp->Unbind(wxEVT_MENU, &DebugAdapterClient::OnSettings, this, XRCID("lldb_settings"));

    EventNotifier::Get()->Unbind(wxEVT_COMMAND_MENU_SELECTED, &DebugAdapterClient::OnRunToCursor, this,
                                 lldbRunToCursorContextMenuId);
    EventNotifier::Get()->Unbind(wxEVT_COMMAND_MENU_SELECTED, &DebugAdapterClient::OnJumpToCursor, this,
                                 lldbJumpToCursorContextMenuId);

    // Dap events
    m_client.Unbind(wxEVT_DAP_EXITED_EVENT, &DebugAdapterClient::OnDapExited, this);
    m_client.Unbind(wxEVT_DAP_TERMINATED_EVENT, &DebugAdapterClient::OnDapExited, this);
    m_client.Unbind(wxEVT_DAP_INITIALIZED_EVENT, &DebugAdapterClient::OnInitializedEvent, this);
    m_client.Unbind(wxEVT_DAP_LAUNCH_RESPONSE, &DebugAdapterClient::OnLaunchResponse, this);
    m_client.Unbind(wxEVT_DAP_STOPPED_EVENT, &DebugAdapterClient::OnStoppedEvent, this);
    m_client.Unbind(wxEVT_DAP_THREADS_RESPONSE, &DebugAdapterClient::OnThreadsResponse, this);
    m_client.Unbind(wxEVT_DAP_SET_FUNCTION_BREAKPOINT_RESPONSE, &DebugAdapterClient::OnSetFunctionBreakpointResponse,
                    this);
    m_client.Unbind(wxEVT_DAP_SET_SOURCE_BREAKPOINT_RESPONSE, &DebugAdapterClient::OnSetSourceBreakpointResponse, this);
}

DebugAdapterClient::~DebugAdapterClient() {}

bool DebugAdapterClient::ShowThreadNames() const { return m_showThreadNames; }

wxString DebugAdapterClient::GetFilenameForDisplay(const wxString& fileName) const
{
    if(m_showFileNamesOnly) {
        return wxFileName(fileName).GetFullName();
    } else {
        return fileName;
    }
}

void DebugAdapterClient::CreateToolBar(clToolBar* toolbar) { wxUnusedVar(toolbar); }

void DebugAdapterClient::CreatePluginMenu(wxMenu* pluginsMenu)
{
    // We want to add an entry in the global settings menu
    // Menu Bar > Settings > LLDB Settings

    // Get the main frame's menubar
    auto mb = clGetManager()->GetMenuBar();
    if(mb) {
        wxMenu* settingsMenu(NULL);
        int menuPos = mb->FindMenu(_("Settings"));
        if(menuPos != wxNOT_FOUND) {
            settingsMenu = mb->GetMenu(menuPos);
            if(settingsMenu) {
                settingsMenu->Append(XRCID("lldb_settings"), _("Debug Adapter Client..."));
            }
        }
    }
}

void DebugAdapterClient::HookPopupMenu(wxMenu* menu, MenuType type)
{
    wxUnusedVar(type);
    wxUnusedVar(menu);
}

void DebugAdapterClient::ClearDebuggerMarker()
{
    IEditor::List_t editors;
    m_mgr->GetAllEditors(editors);

    for(auto editor : editors) {
        editor->GetCtrl()->MarkerDeleteAll(smt_indicator);
    }
}

void DebugAdapterClient::SetDebuggerMarker(wxStyledTextCtrl* stc, int lineno)
{
    stc->MarkerDeleteAll(smt_indicator);
    stc->MarkerAdd(lineno, smt_indicator);
    int caretPos = stc->PositionFromLine(lineno);
    stc->SetSelection(caretPos, caretPos);
    stc->SetCurrentPos(caretPos);
    stc->EnsureCaretVisible();
}

void DebugAdapterClient::OnDebugContinue(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    event.Skip();

    // call continue
    m_client.Continue();
    LOG_DEBUG(LOG) << "Sending 'continue' command" << endl;
}

void DebugAdapterClient::OnDebugStart(clDebugEvent& event)
{
    if(m_client.IsConnected()) {
        // already running, do nothing
        return;
    }

    LOG_DEBUG(LOG) << "debug-start event is called for debugger:" << event.GetDebuggerName() << endl;

    if(!IsDebuggerOwnedByPlugin(event.GetDebuggerName())) {
        event.Skip();
        LOG_DEBUG(LOG) << "Skipping" << endl;
        return;
    }

    // fetch the requested debugger details
    DapEntry dap_server;
    m_dap_store.Get(event.GetDebuggerName(), &dap_server);

    LOG_DEBUG(LOG) << "working directory is:" << ::wxGetCwd() << endl;

    // the following 4 variables are used for launching the debugger
    wxString working_directory;
    wxString exepath;
    wxString args;
    clEnvList_t env;

    if(clCxxWorkspaceST::Get()->IsOpen()) {
        // standard C++ workspace
        ProjectPtr project = clCxxWorkspaceST::Get()->GetProject(event.GetProjectName());
        if(!project) {
            ::wxMessageBox(wxString() << _("Could not locate project: ") << event.GetProjectName(),
                           DAP_MESSAGE_BOX_TITLE, wxICON_ERROR | wxOK | wxCENTER);
            LOG_ERROR(LOG) << "unable to locate project:" << event.GetProjectName() << endl;
            return;
        }

        BuildConfigPtr bldConf = project->GetBuildConfiguration();
        if(!bldConf) {
            ::wxMessageBox(wxString() << _("Could not locate the requested build configuration"), DAP_MESSAGE_BOX_TITLE,
                           wxICON_ERROR | wxOK | wxCENTER);
            return;
        }

        // Determine the executable to debug, working directory and arguments
        LOG_DEBUG(LOG) << "Preparing environment variables.." << endl;
        env = bldConf->GetEnvironment(project.Get());
        LOG_DEBUG(LOG) << "Success" << endl;
        exepath = bldConf->GetCommand();

        // Get the debugging arguments.
        if(bldConf->GetUseSeparateDebugArgs()) {
            args = bldConf->GetDebugArgs();
        } else {
            args = bldConf->GetCommandArguments();
        }

        working_directory = MacroManager::Instance()->Expand(bldConf->GetWorkingDirectory(), m_mgr, project->GetName());
        exepath = MacroManager::Instance()->Expand(exepath, m_mgr, project->GetName());

        if(working_directory.empty()) {
            working_directory = wxGetCwd();
        }
        wxFileName fn(exepath);
        if(fn.IsRelative()) {
            fn.MakeAbsolute(working_directory);
        }
        exepath = fn.GetFullPath();
    } else if(clFileSystemWorkspace::Get().IsOpen()) {
        LOG_SYSTEM(LOG) << "File System workspace is not yet supported" << endl;
    }

    // start the debugger
    StartAndConnectToDapServer(dap_server, exepath, args, working_directory, env);
}

void DebugAdapterClient::OnDebugNext(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    LOG_DEBUG(LOG) << "-> Next" << endl;
    m_client.Next();
}

void DebugAdapterClient::OnDebugStop(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    LOG_DEBUG(LOG) << "-> Stop" << endl;
    DoCleanup();
}

void DebugAdapterClient::OnDebugIsRunning(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    event.SetAnswer(m_client.IsConnected());
}

void DebugAdapterClient::OnDebugCanInteract(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    event.SetAnswer(m_client.IsConnected() && m_client.CanInteract());
}

void DebugAdapterClient::OnDebugStepIn(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    m_client.StepIn();
    LOG_DEBUG(LOG) << "-> StopIn" << endl;
}

void DebugAdapterClient::OnDebugStepOut(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    m_client.StepOut();
    LOG_DEBUG(LOG) << "-> StopOut" << endl;
}

void DebugAdapterClient::RestoreUI()
{
    // Save current perspective before destroying the session
    if(m_isPerspectiveLoaded) {
        m_mgr->SavePerspective("DAP");

        // Restore the old perspective
        m_mgr->LoadPerspective("Default");
        m_isPerspectiveLoaded = false;
    }

    DestroyUI();
}

void DebugAdapterClient::LoadPerspective()
{
    // Save the current persepctive we start debguging
    m_mgr->SavePerspective("Default");

    // Load the LLDB perspective
    m_mgr->LoadPerspective("DAP");
    m_isPerspectiveLoaded = true;

    // Make sure that all the panes are visible
    ShowPane(DAP_MAIN_VIEW, true);

    // Hide the output pane
    wxAuiPaneInfo& pi = m_mgr->GetDockingManager()->GetPane("Output View");
    if(pi.IsOk() && pi.IsShown()) {
        pi.Hide();
    }
    m_mgr->GetDockingManager()->Update();
}

void DebugAdapterClient::ShowPane(const wxString& paneName, bool show)
{
    wxAuiPaneInfo& pi = m_mgr->GetDockingManager()->GetPane(paneName);
    if(pi.IsOk()) {
        if(show) {
            if(!pi.IsShown()) {
                pi.Show();
            }
        } else {
            if(pi.IsShown()) {
                pi.Hide();
            }
        }
    }
}

void DebugAdapterClient::DestroyUI()
{
    // Destroy the callstack window
    if(m_threadsView) {
        wxAuiPaneInfo& pi = m_mgr->GetDockingManager()->GetPane(DAP_MAIN_VIEW);
        if(pi.IsOk()) {
            m_mgr->GetDockingManager()->DetachPane(m_threadsView);
        }
        m_threadsView->Destroy();
        m_threadsView = NULL;
    }
    ClearDebuggerMarker();
    m_mgr->GetDockingManager()->Update();
}

void DebugAdapterClient::InitializeUI()
{
    wxWindow* parent = m_mgr->GetDockingManager()->GetManagedWindow();
    if(!m_threadsView) {
        m_threadsView = new DAPMainView(parent, &m_client);
        m_mgr->GetDockingManager()->AddPane(m_threadsView, wxAuiPaneInfo()
                                                               .MinSize(200, 200)
                                                               .Layer(10)
                                                               .Right()
                                                               .Position(1)
                                                               .CloseButton()
                                                               .Caption(DAP_MAIN_VIEW)
                                                               .Name(DAP_MAIN_VIEW));
    }
}

void DebugAdapterClient::OnToggleBreakpoint(clDebugEvent& event)
{
    event.Skip();

    // User toggled a breakpoint
    IEditor* editor = clGetManager()->GetActiveEditor();
    CHECK_PTR_RET(editor);

    if(editor->GetRemotePathOrLocal() != event.GetFileName()) {
        event.Skip(false);
        return;
    }

    const wxString& path = event.GetFileName();

    // if a session is running, re-apply the breakpoints
    ApplyBreakpoints(path);
}

void DebugAdapterClient::DoCleanup()
{
    ClearDebuggerMarker();
    m_client.Reset();
    m_raisOnBpHit = false;
    StopProcess();
    m_session.Clear();
    m_terminal_helper.Terminate();
}

void DebugAdapterClient::OnWorkspaceClosed(clWorkspaceEvent& event)
{
    event.Skip();
    DoCleanup();
}

void DebugAdapterClient::OnWorkspaceLoaded(clWorkspaceEvent& event) { event.Skip(); }

void DebugAdapterClient::OnToggleInterrupt(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    event.Skip();
    m_client.Pause();
}

void DebugAdapterClient::OnBuildStarting(clBuildEvent& event)
{
    if(m_client.IsConnected()) {
        // lldb session is active, prompt the user
        if(::wxMessageBox(_("A debug session is running\nCancel debug session and continue building?"),
                          DAP_MESSAGE_BOX_TITLE, wxICON_QUESTION | wxYES_NO | wxYES_DEFAULT | wxCENTER) == wxYES) {
            clDebugEvent dummy;
            OnDebugStop(dummy);
            event.Skip();
        } else {
            // do nothing - this will cancel the build
        }

    } else {
        event.Skip();
    }
}

void DebugAdapterClient::OnAddWatch(wxCommandEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    // FIXME
}

void DebugAdapterClient::OnRunToCursor(wxCommandEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    // FIXME
}

void DebugAdapterClient::OnJumpToCursor(wxCommandEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    // FIXME
}

void DebugAdapterClient::OnSettings(wxCommandEvent& event)
{
    event.Skip();
    clDapSettingsStore store = m_dap_store;
    DapDebuggerSettingsDlg dlg(EventNotifier::Get()->TopFrame(), store);
    if(dlg.ShowModal() != wxID_OK) {
        return;
    }
    m_dap_store = store;
    m_dap_store.Store();

    // refresh the list of debuggers we are registering by this plugin
    RegisterDebuggers();
}

void DebugAdapterClient::OnInitDone(wxCommandEvent& event) { event.Skip(); }

void DebugAdapterClient::OnDebugTooltip(clDebugEvent& event) { CHECK_IS_DAP_CONNECTED(); }

void DebugAdapterClient::OnDebugQuickDebug(clDebugEvent& event)
{
#if 0
    if(!DoInitializeDebugger(event, true,
                             clDebuggerTerminalPOSIX::MakeExeTitle(event.GetExecutableName(), event.GetArguments()))) {
        return;
    }

    LLDBConnectReturnObject retObj;
    LLDBSettings settings;
    settings.Load();
    if(m_connector.Connect(retObj, settings, 5)) {

        // Apply the environment
        EnvSetter env;

        // Get list of breakpoints and add them ( we will apply them later on )
        clDebuggerBreakpoint::Vec_t gdbBps;
        m_mgr->GetAllBreakpoints(gdbBps);

        // remove all breakpoints from previous session
        m_connector.DeleteAllBreakpoints();

        // apply the serialized breakpoints
        // In 'Quick Debug' we stop on main
        m_connector.AddBreakpoint("main");
        m_connector.AddBreakpoints(gdbBps);

        // Setup pivot folder if needed
        SetupPivotFolder(retObj);

        LLDBCommand startCommand;
        startCommand.FillEnvFromMemory();
        startCommand.SetExecutable(event.GetExecutableName());
        startCommand.SetCommandArguments(event.GetArguments());
        startCommand.SetWorkingDirectory(event.GetWorkingDirectory());
        startCommand.SetStartupCommands(event.GetStartupCommands());
        startCommand.SetRedirectTTY(m_debuggerTerminal.GetTty());
        m_connector.Start(startCommand);

    } else {
        // Failed to connect, notify and perform cleanup
        DoCleanup();
        wxString message;
        message << _("Could not connect to codelite-lldb at '") << m_connector.GetConnectString() << "'";
        ::wxMessageBox(message, DAP_MESSAGE_BOX_TITLE, wxICON_ERROR | wxOK | wxCENTER);
        return;
    }
#endif
}

void DebugAdapterClient::OnDebugCoreFile(clDebugEvent& event)
{
    // FIXME
#if 0
    if(event.GetDebuggerName() != LLDB_DEBUGGER_NAME) {
        event.Skip();
        return;
    }

#ifdef __WXMSW__
    ::wxMessageBox(_("Debug core file with LLDB is not supported under Windows"), DAP_MESSAGE_BOX_TITLE,
                   wxOK | wxCENTER | wxICON_WARNING);
    return;
#endif

    if(!DoInitializeDebugger(event, false, clDebuggerTerminalPOSIX::MakeCoreTitle(event.GetCoreFile()))) {
        return;
    }

    LLDBConnectReturnObject retObj;
    LLDBSettings settings;
    settings.Load();

    if(m_connector.Connect(retObj, settings, 5)) {

        // Apply the environment
        EnvSetter env;

        // remove all breakpoints from previous session
        m_connector.DeleteAllBreakpoints();

        LLDBCommand startCommand;
        startCommand.FillEnvFromMemory();
        startCommand.SetCommandType(kCommandDebugCoreFile);
        startCommand.SetExecutable(event.GetExecutableName());
        startCommand.SetCorefile(event.GetCoreFile());
        startCommand.SetWorkingDirectory(event.GetWorkingDirectory());
        startCommand.SetRedirectTTY(m_debuggerTerminal.GetTty());
        m_connector.OpenCoreFile(startCommand);
    } else {
        // Failed to connect, notify and perform cleanup
        DoCleanup();
        wxString message;
        message << _("Could not connect to codelite-lldb at '") << m_connector.GetConnectString() << "'";
        ::wxMessageBox(message, DAP_MESSAGE_BOX_TITLE, wxICON_ERROR | wxOK | wxCENTER);
        return;
    }
#endif
}

void DebugAdapterClient::OnDebugAttachToProcess(clDebugEvent& event)
{
#if 0
    if(event.GetDebuggerName() != LLDB_DEBUGGER_NAME) {
        event.Skip();
        return;
    }

#ifdef __WXMSW__
    ::wxMessageBox(_("Attach to process with LLDB is not supported under Windows"), DAP_MESSAGE_BOX_TITLE,
                   wxOK | wxCENTER | wxICON_WARNING);
    return;
#endif

    if(!DoInitializeDebugger(event, true, clDebuggerTerminalPOSIX::MakePidTitle(event.GetInt())))
        return;

    LLDBConnectReturnObject retObj;
    LLDBSettings settings;
    settings.Load();

    if(m_connector.Connect(retObj, settings, 5)) {

        // Apply the environment
        EnvSetter env;

        // remove all breakpoints from previous session
        m_connector.DeleteAllBreakpoints();
        LLDBSettings settings;
        settings.Load();

        // Attach to the process
        LLDBCommand command;
        command.SetCommandType(kCommandAttachProcess);
        command.SetProcessID(event.GetInt());
        command.SetSettings(settings);
        m_connector.AttachProcessWithPID(command);

    } else {
        // Failed to connect, notify and perform cleanup
        DoCleanup();
        wxString message;
        message << _("Could not connect to codelite-lldb at '") << m_connector.GetConnectString() << "'";
        ::wxMessageBox(message, DAP_MESSAGE_BOX_TITLE, wxICON_ERROR | wxOK | wxCENTER);
        return;
    }
#endif
}

void DebugAdapterClient::OnDebugDeleteAllBreakpoints(clDebugEvent& event)
{
    event.Skip();
    // FIXME
}

void DebugAdapterClient::OnDebugDisableAllBreakpoints(clDebugEvent& event) { event.Skip(); }

void DebugAdapterClient::OnDebugEnableAllBreakpoints(clDebugEvent& event) { event.Skip(); }

void DebugAdapterClient::OnDebugVOID(clDebugEvent& event) { CHECK_IS_DAP_CONNECTED(); }

void DebugAdapterClient::OnDebugNextInst(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    // FIXME
}

void DebugAdapterClient::OnDebugShowCursor(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    // FIXME
}

/// --------------------------------------------------------------------------
/// dap events starting here
/// --------------------------------------------------------------------------

void DebugAdapterClient::OnDapExited(DAPEvent& event)
{
    event.Skip();
    LOG_DEBUG(LOG) << "dap-server exited" << endl;
    DoCleanup();
}

void DebugAdapterClient::OnDapLog(DAPEvent& event)
{
    event.Skip();
    LOG_DEBUG(LOG) << event.GetString() << endl;
}

void DebugAdapterClient::OnLaunchResponse(DAPEvent& event)
{
    // Check that the debugee was started successfully
    dap::LaunchResponse* resp = event.GetDapResponse()->As<dap::LaunchResponse>();

    if(resp && !resp->success) {
        // launch failed!
        wxMessageBox("Failed to launch debuggee: " + resp->message, DAP_MESSAGE_BOX_TITLE,
                     wxICON_ERROR | wxOK | wxOK_DEFAULT | wxCENTRE);
        CallAfter(&DebugAdapterClient::DoCleanup);
    }
}

void DebugAdapterClient::OnInitializeResponse(DAPEvent& event)
{
    LOG_DEBUG(LOG) << "got initialize response" << endl;
    LOG_DEBUG(LOG) << "Starting debugger for command:" << endl;
    LOG_DEBUG(LOG) << m_session.command << endl;
    LOG_DEBUG(LOG) << "working directory:" << m_session.working_directory << endl;

    // FIXME: apply the environment here
    auto v = m_session.command;
    LOG_DEBUG(LOG) << "Calling Launch() with command:" << v << endl;
    m_client.Launch(std::move(v), m_session.working_directory);
}

/// DAP server responded to our `initialize` request
void DebugAdapterClient::OnInitializedEvent(DAPEvent& event)
{
    // place a single breakpoint on main
    dap::FunctionBreakpoint main_bp{ "main" };
    m_session.need_to_set_breakpoints = true;
    m_client.SetFunctionBreakpoints({ main_bp });

    // place all breakpoints
    m_client.ConfigurationDone();
}

void DebugAdapterClient::OnStoppedEvent(DAPEvent& event)
{
    // got stopped event
    if(m_session.need_to_set_breakpoints) {
        ApplyBreakpoints(wxEmptyString);
        m_session.need_to_set_breakpoints = false;
    }

    dap::StoppedEvent* stopped_data = event.GetDapEvent()->As<dap::StoppedEvent>();
    if(stopped_data) {
        LOG_DEBUG(LOG) << "Stopped reason:" << stopped_data->reason << endl;
        LOG_DEBUG(LOG) << "All threads stopped:" << stopped_data->allThreadsStopped << endl;
        LOG_DEBUG(LOG) << "Stopped thread ID:" << stopped_data->threadId
                       << "(active thread ID:" << m_client.GetActiveThreadId() << ")" << endl;
        m_client.GetThreads();
    }
}

void DebugAdapterClient::OnThreadsResponse(DAPEvent& event)
{
    CHECK_PTR_RET(m_threadsView);

    auto response = event.GetDapResponse()->As<dap::ThreadsResponse>();
    CHECK_PTR_RET(response);

    m_threadsView->UpdateThreads(m_client.GetActiveThreadId(), response);
    auto threads_to_update = m_threadsView->GetExpandedThreads();
    for(int thread_id : threads_to_update) {
        m_client.GetFrames(thread_id);
    }
}

void DebugAdapterClient::OnStackTraceResponse(DAPEvent& event)
{
    CHECK_PTR_RET(m_threadsView);

    auto response = event.GetDapResponse()->As<dap::StackTraceResponse>();
    CHECK_PTR_RET(response);

    LOG_DEBUG(LOG) << "Requesting frames for thread-id:" << response->threadId << endl;
    m_threadsView->UpdateFrames(response->threadId, response);
    if(!response->stackFrames.empty()) {
        auto frame = response->stackFrames[0];
        LOG_DEBUG(LOG) << "Frame path:" << frame.source.path << endl;
        wxString filepath = NormaliseReceivedPath(frame.source.path);
        LOG_DEBUG(LOG) << "Normalising file:" << frame.source.path << "->" << filepath << endl;
        int line_number = frame.line;

        clGetManager()->OpenFileAndAsyncExecute(filepath, [this, line_number, filepath](IEditor* editor) {
            LOG_DEBUG(LOG) << "setting debugger marker at:" << filepath << ":" << line_number << endl;
            this->SetDebuggerMarker(editor->GetCtrl(), line_number - 1);
        });
    }
}

void DebugAdapterClient::OnSetFunctionBreakpointResponse(DAPEvent& event)
{
    auto resp = event.GetDapResponse()->As<dap::SetFunctionBreakpointsResponse>();
    CHECK_PTR_RET(resp);
    LOG_DEBUG(LOG) << "Function breakpoint response:" << resp->success << ". message:" << resp->message << endl;
    for(const auto& bp : resp->breakpoints) {
        LOG_DEBUG(LOG) << "verified:" << bp.verified << "file:" << bp.source.path << "line:" << bp.line << endl;
    }
}

void DebugAdapterClient::OnSetSourceBreakpointResponse(DAPEvent& event)
{
    auto resp = event.GetDapResponse()->As<dap::SetBreakpointsResponse>();
    CHECK_PTR_RET(resp);
    LOG_DEBUG(LOG) << "Source breakpoint response:" << resp->success << ". message:" << resp->message << endl;
    for(const auto& bp : resp->breakpoints) {
        LOG_DEBUG(LOG) << "verified:" << bp.verified << "file:" << bp.source.path << "line:" << bp.line << endl;
    }
}

void DebugAdapterClient::OnRunInTerminal(DAPEvent& event)
{
    auto request = event.GetDapRequest()->As<dap::RunInTerminalRequest>();
    CHECK_PTR_RET(request);

    int process_id = m_terminal_helper.RunProcess(request->arguments.args, wxEmptyString, {});
    // send the response back to the dap server
    auto response = m_client.MakeRequest<dap::RunInTerminalResponse>();
    LOG_DEBUG(LOG) << "RunInTerminal process ID:" << process_id << endl;
    response.request_seq = request->seq;
    if(process_id == wxNOT_FOUND) {
        response.success = false;
        response.processId = 0;
    } else {
        response.success = true;
        response.processId = process_id;
    }
    m_client.SendResponse(response);
}

/// --------------------------------------------------------------------------
/// dap events stops here
/// --------------------------------------------------------------------------

void DebugAdapterClient::ApplyBreakpoints(const wxString& path)
{
    if(!m_client.IsConnected()) {
        return;
    }

    std::unordered_map<wxString, std::vector<dap::SourceBreakpoint>> dap_source_breakpoints;
    std::vector<dap::FunctionBreakpoint> dap_function_breakpoints;

    clDebuggerBreakpoint::Vec_t all_bps;
    clGetManager()->GetAllBreakpoints(all_bps);

    if(all_bps.empty()) {
        return;
    }

    dap_source_breakpoints.reserve(all_bps.size());
    dap_function_breakpoints.reserve(all_bps.size());
    for(const auto& bp : all_bps) {
        if(is_source_breakpoint(bp)) {
            if(path.empty() || path == bp.file) {
                // all breakpoints
                if(dap_source_breakpoints.count(bp.file) == 0) {
                    dap_source_breakpoints.insert({ bp.file, {} });
                }
                dap_source_breakpoints[bp.file].push_back(to_dap_source_bp(bp));
            }

        } else if(is_function_breakpoint(bp)) {
            dap_function_breakpoints.push_back(to_dap_function_bp(bp));
        }
    }

    // dont pass empty array, it will tell dap to clear all breakpoints
    for(const auto& vt : dap_source_breakpoints) {
        if(!vt.second.empty()) {
            wxFileName filepath(vt.first);
            LOG_DEBUG(LOG) << "Applying breakpoints for file:" << filepath << endl;
            LOG_DEBUG(LOG) << vt.second << endl;
            wxString source_path = NormalisePathForSend(filepath.GetFullPath());
            m_client.SetBreakpointsFile(source_path, vt.second);
        }
    }

    if(!dap_function_breakpoints.empty()) {
        LOG_DEBUG(LOG) << "Applying function breakpoints:" << endl;
        LOG_DEBUG(LOG) << dap_function_breakpoints << endl;
        m_client.SetFunctionBreakpoints(dap_function_breakpoints);
    }
}

void DebugAdapterClient::RefreshBreakpointsMarkersForEditor(IEditor* editor) { CHECK_PTR_RET(editor); }

bool DebugAdapterClient::LaunchProcess(const DapEntry& dap_server)
{
    wxDELETE(m_dap_server);

    if(dap_server.IsRemote()) {
        // launch ssh process
        auto env_list = StringUtils::BuildEnvFromString(dap_server.GetEnvironment());
        m_dap_server = ::CreateAsyncProcess(this, dap_server.GetCommand(),
                                            IProcessCreateDefault | IProcessCreateSSH | IProcessWrapInShell,
                                            wxEmptyString, &env_list, dap_server.GetSshAccount());
    } else {
        // launch local process
        auto env_list = StringUtils::ResolveEnvList(dap_server.GetEnvironment());
        m_dap_server = ::CreateAsyncProcess(this, dap_server.GetCommand(),
                                            IProcessNoRedirect | IProcessWrapInShell | IProcessCreateWithHiddenConsole,
                                            wxEmptyString, &env_list);
    }
    return m_dap_server != nullptr;
}

void DebugAdapterClient::StartAndConnectToDapServer(const DapEntry& dap_server, const wxString& exepath,
                                                    const wxString& args, const wxString& working_directory,
                                                    const clEnvList_t& env)
{
    // Reset the client
    m_client.Reset();
    LOG_DEBUG(LOG) << "Connecting to dap-server:" << dap_server.GetName() << endl;
    LOG_DEBUG(LOG) << "exepath:" << exepath << endl;
    LOG_DEBUG(LOG) << "args:" << args << endl;
    LOG_DEBUG(LOG) << "working_directory:" << working_directory << endl;
    LOG_DEBUG(LOG) << "env:" << to_string_array(env) << endl;

    // first, launch the process
    if(!LaunchProcess(dap_server)) {
        return;
    }

    wxBusyCursor cursor;
    // For this demo, we use socket transport. But you may choose
    // to write your own transport that implements the dap::Transport interface
    // This is useful when the user wishes to use stdin/out for communicating with
    // the dap and not over socket
    dap::SocketTransport* transport = new dap::SocketTransport();
    if(!transport->Connect(dap_server.GetConnectionString(), 10)) {
        wxMessageBox("Failed to connect to DAP server", DAP_MESSAGE_BOX_TITLE, wxICON_ERROR | wxOK | wxCENTRE);
        wxDELETE(transport);
        m_client.Reset();
        return;
    }
    m_session.Clear();
    wxArrayString command_array = StringUtils::BuildArgv(args);
    command_array.Insert(exepath, 0);
    m_session.command = { command_array.begin(), command_array.end() };
    if(m_session.command.empty()) {
        wxMessageBox(_("Can't debug an empty command!"), DAP_MESSAGE_BOX_TITLE, wxICON_ERROR | wxOK | wxCENTRE);
        DoCleanup();
        return;
    }

    m_session.working_directory = working_directory;
    m_session.environment = env;
    m_session.dap_server = dap_server;

    // Notify about debug start event
    // + load the UI
    InitializeUI();
    LoadPerspective();

    // Fire CodeLite IDE event indicating that a debug session started
    clDebugEvent cl_event{ wxEVT_DEBUG_STARTED };
    EventNotifier::Get()->AddPendingEvent(cl_event);

    // construct new client with the transport
    m_client.SetTransport(transport);
    m_client.Initialize(); // send protocol Initialize request
}

void DebugAdapterClient::OnFileLoaded(clCommandEvent& event) { event.Skip(); }

bool DebugAdapterClient::IsDebuggerOwnedByPlugin(const wxString& name) const
{
    return m_dap_store.GetEntries().count(name) != 0;
}

void DebugAdapterClient::OnProcessOutput(clProcessEvent& event)
{
    wxUnusedVar(event);
    LOG_DEBUG(LOG) << "server output:" << event.GetOutput() << endl;
}

void DebugAdapterClient::OnProcessTerminated(clProcessEvent& event)
{
    wxUnusedVar(event);
    wxDELETE(m_dap_server);

    m_client.Reset();

    RestoreUI();
    LOG_DEBUG(LOG) << "dap-server terminated" << endl;

    clDebugEvent e(wxEVT_DEBUG_ENDED);
    EventNotifier::Get()->AddPendingEvent(e);
}

void DebugAdapterClient::StopProcess()
{
    if(m_dap_server) {
        // wxEVT_DEBUG_ENDED is sent in OnProcessTerminated() handler
        LOG_DEBUG(LOG) << "Terminating dap-server..." << endl;
        m_dap_server->Terminate();

    } else {
        clDebugEvent e(wxEVT_DEBUG_ENDED);
        EventNotifier::Get()->AddPendingEvent(e);
    }
}

wxString DebugAdapterClient::NormalisePathForSend(const wxString& path) const
{
    wxFileName fn(path);
    if(m_session.dap_server.UseRelativePath()) {
        return fn.GetFullName();
    }
    return NormaliseReceivedPath(path);
}

wxString DebugAdapterClient::NormaliseReceivedPath(const wxString& path) const
{
    wxFileName fn(path);
    wxPathFormat path_format = wxPATH_NATIVE;
    if(m_session.debug_over_ssh || m_session.dap_server.IsUsingUnixPath()) {
        path_format = wxPATH_UNIX;
    }

    if(fn.IsRelative()) {
        fn.MakeAbsolute(m_session.working_directory, path_format);
        if(!m_session.debug_over_ssh) {
            // try to locate the file locally
            if(IS_WINDOWS && !fn.FileExists()) {
                if(fn.HasVolume()) {
                    fn.SetVolume("C");
                }
            }
        }
    }
    return fn.GetFullPath(path_format);
}