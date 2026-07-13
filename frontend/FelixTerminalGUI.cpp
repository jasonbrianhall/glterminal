#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/listctrl.h>
#include <wx/textfile.h>
#include <wx/stdpaths.h>
#include <wx/dir.h>
#include <wx/filefn.h>
#include <sstream>
#include <vector>

#ifdef __WXMSW__
#define FLT_EXECUTABLE "flt.exe"
#else
#define FLT_EXECUTABLE "./flt"
#endif

struct SessionConfig {
    wxString name;
    int connType = 0;
    wxString localShell;
    wxString sshUser;
    wxString sshHost = "localhost";
    int sshPort = 22;
    int sshAuthMethod = 0;
    wxString sshKeyPath;
    wxString sshPassword;
    wxString sshKnownHosts;
    bool sshX11 = true;
    std::vector<wxString> localPF;
    std::vector<wxString> remotePF;
    std::vector<wxString> socks;
};

class FelixTerminalFrame : public wxFrame {
public:
    FelixTerminalFrame() : wxFrame(nullptr, wxID_ANY, "Felix Terminal", 
                                   wxDefaultPosition, wxSize(950, 650)) {
        
        CreateConfigDir();
        
        wxPanel *mainPanel = new wxPanel(this);
        wxBoxSizer *mainSizer = new wxBoxSizer(wxHORIZONTAL);

        // ========== LEFT PANEL: Session List ==========
        wxBoxSizer *leftSizer = new wxBoxSizer(wxVERTICAL);
        leftSizer->Add(new wxStaticText(mainPanel, wxID_ANY, "Saved Sessions"), 0, wxALL, 8);
        
        m_sessionList = new wxListCtrl(mainPanel, wxID_ANY, wxDefaultPosition, wxSize(200, 400), wxLC_REPORT | wxLC_SINGLE_SEL);
        m_sessionList->AppendColumn("Session");
        m_sessionList->SetColumnWidth(0, 180);
        leftSizer->Add(m_sessionList, 1, wxEXPAND | wxALL, 8);

        wxBoxSizer *btnSizer = new wxBoxSizer(wxVERTICAL);
        m_loadBtn = new wxButton(mainPanel, wxID_ANY, "Load");
        m_saveBtn = new wxButton(mainPanel, wxID_ANY, "Save");
        m_deleteBtn = new wxButton(mainPanel, wxID_ANY, "Delete");
        
        m_loadBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnLoadSession, this);
        m_saveBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnSaveSession, this);
        m_deleteBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnDeleteSession, this);
        
        btnSizer->Add(m_loadBtn, 0, wxEXPAND | wxBOTTOM, 4);
        btnSizer->Add(m_saveBtn, 0, wxEXPAND | wxBOTTOM, 4);
        btnSizer->Add(m_deleteBtn, 0, wxEXPAND);
        leftSizer->Add(btnSizer, 0, wxEXPAND | wxALL, 8);

        mainSizer->Add(leftSizer, 0, wxEXPAND | wxALL, 0);
        mainSizer->Add(new wxStaticLine(mainPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_VERTICAL), 0, wxEXPAND);

        // ========== RIGHT PANEL: Settings ==========
        wxBoxSizer *rightSizer = new wxBoxSizer(wxVERTICAL);

        wxStaticBoxSizer *sessionNameBox = new wxStaticBoxSizer(wxHORIZONTAL, mainPanel, "Session Name");
        m_sessionNameCtrl = new wxTextCtrl(mainPanel, wxID_ANY, "New Session");
        sessionNameBox->Add(m_sessionNameCtrl, 1, wxEXPAND | wxALL, 4);
        rightSizer->Add(sessionNameBox, 0, wxEXPAND | wxALL, 8);

        // Notebook for settings
        wxNotebook *notebook = new wxNotebook(mainPanel, wxID_ANY);

        // ---- Connection Tab ----
        CreateConnectionPanel(notebook, mainPanel);
        // ---- Port Forward Tab ----
        CreatePortForwardPanel(notebook, mainPanel);

        rightSizer->Add(notebook, 1, wxEXPAND | wxALL, 8);

        // ---- Command Preview ----
        wxStaticBoxSizer *cmdSizer = new wxStaticBoxSizer(wxVERTICAL, mainPanel, "Command Line");
        m_cmdPreview = new wxTextCtrl(mainPanel, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 50), 
                                       wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
        cmdSizer->Add(m_cmdPreview, 1, wxEXPAND | wxALL, 4);
        rightSizer->Add(cmdSizer, 0, wxEXPAND | wxALL, 8);

        // ---- Buttons ----
        wxBoxSizer *bottomBtnSizer = new wxBoxSizer(wxHORIZONTAL);
        m_openBtn = new wxButton(mainPanel, wxID_ANY, "Open");
        m_closeBtn = new wxButton(mainPanel, wxID_CLOSE, "Close");
        
        m_openBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnOpen, this);
        m_closeBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnClose, this);

        bottomBtnSizer->Add(m_openBtn, 0, wxRIGHT, 4);
        bottomBtnSizer->AddStretchSpacer();
        bottomBtnSizer->Add(m_closeBtn, 0);
        rightSizer->Add(bottomBtnSizer, 0, wxEXPAND | wxALL, 8);

        mainSizer->Add(rightSizer, 1, wxEXPAND | wxALL, 0);
        mainPanel->SetSizer(mainSizer);

        LoadSessions();
        UpdatePreview();
    }

private:
    wxListCtrl *m_sessionList;
    wxButton *m_loadBtn;
    wxButton *m_saveBtn;
    wxButton *m_deleteBtn;
    wxButton *m_openBtn;
    wxButton *m_closeBtn;
    wxTextCtrl *m_sessionNameCtrl;
    wxTextCtrl *m_cmdPreview;

    // Connection
    wxChoice *m_connTypeChoice;
    wxTextCtrl *m_localShellCtrl;
    wxTextCtrl *m_sshUserCtrl;
    wxTextCtrl *m_sshHostCtrl;
    wxSpinCtrl *m_sshPortSpin;
    wxChoice *m_sshAuthChoice;
    wxTextCtrl *m_sshKeyCtrl;
    wxButton *m_browseSshKeyBtn;
    wxTextCtrl *m_sshPassCtrl;
    wxTextCtrl *m_sshKnownHostsCtrl;
    wxCheckBox *m_sshX11Check;

    // Port Forwarding
    wxListBox *m_localPFList;
    wxListBox *m_remotePFList;
    wxListBox *m_socksList;
    std::vector<wxString> m_localPFEntries;
    std::vector<wxString> m_remotePFEntries;
    std::vector<wxString> m_socksEntries;

    void CreateConnectionPanel(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        wxScrolledWindow *scrolled = new wxScrolledWindow(panel, wxID_ANY);
        scrolled->SetScrollRate(5, 5);
        wxBoxSizer *scrolledSizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer *typeSizer = new wxBoxSizer(wxHORIZONTAL);
        typeSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Connection Type:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_connTypeChoice = new wxChoice(scrolled, wxID_ANY);
        m_connTypeChoice->Append("Local Shell");
        m_connTypeChoice->Append("SSH");
        m_connTypeChoice->SetSelection(0);
        m_connTypeChoice->Bind(wxEVT_CHOICE, &FelixTerminalFrame::OnConnTypeChange, this);
        typeSizer->Add(m_connTypeChoice, 1, wxEXPAND);
        scrolledSizer->Add(typeSizer, 0, wxEXPAND | wxALL, 8);

        scrolledSizer->Add(new wxStaticLine(scrolled), 0, wxEXPAND | wxALL, 8);

        // Local Shell
        wxStaticBoxSizer *localBox = new wxStaticBoxSizer(wxVERTICAL, scrolled, "Local Shell");
        wxBoxSizer *shellSizer = new wxBoxSizer(wxHORIZONTAL);
        shellSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Shell:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_localShellCtrl = new wxTextCtrl(scrolled, wxID_ANY, "");
        m_localShellCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnControlChange, this);
        shellSizer->Add(m_localShellCtrl, 1, wxEXPAND);
        localBox->Add(shellSizer, 0, wxEXPAND | wxALL, 4);
        scrolledSizer->Add(localBox, 0, wxEXPAND | wxALL, 8);

        // SSH
        wxStaticBoxSizer *sshBox = new wxStaticBoxSizer(wxVERTICAL, scrolled, "SSH");
        wxBoxSizer *sshUserHostSizer = new wxBoxSizer(wxHORIZONTAL);
        sshUserHostSizer->Add(new wxStaticText(scrolled, wxID_ANY, "User:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshUserCtrl = new wxTextCtrl(scrolled, wxID_ANY, "");
        m_sshUserCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnControlChange, this);
        sshUserHostSizer->Add(m_sshUserCtrl, 0, wxRIGHT, 16);
        sshUserHostSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Host:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshHostCtrl = new wxTextCtrl(scrolled, wxID_ANY, "localhost");
        m_sshHostCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnControlChange, this);
        sshUserHostSizer->Add(m_sshHostCtrl, 1, wxEXPAND);
        sshBox->Add(sshUserHostSizer, 0, wxEXPAND | wxALL, 4);

        wxBoxSizer *sshPortSizer = new wxBoxSizer(wxHORIZONTAL);
        sshPortSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshPortSpin = new wxSpinCtrl(scrolled, wxID_ANY, "22", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535);
        m_sshPortSpin->Bind(wxEVT_SPINCTRL, &FelixTerminalFrame::OnControlChange, this);
        sshPortSizer->Add(m_sshPortSpin, 0);
        sshBox->Add(sshPortSizer, 0, wxEXPAND | wxALL, 4);

        wxBoxSizer *sshAuthSizer = new wxBoxSizer(wxHORIZONTAL);
        sshAuthSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Auth:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshAuthChoice = new wxChoice(scrolled, wxID_ANY);
        m_sshAuthChoice->Append("SSH Agent (default)");
        m_sshAuthChoice->Append("Private Key (-i)");
        m_sshAuthChoice->Append("Password");
        m_sshAuthChoice->SetSelection(0);
        m_sshAuthChoice->Bind(wxEVT_CHOICE, &FelixTerminalFrame::OnSSHAuthChange, this);
        sshAuthSizer->Add(m_sshAuthChoice, 1, wxEXPAND);
        sshBox->Add(sshAuthSizer, 0, wxEXPAND | wxALL, 4);

        wxBoxSizer *sshKeySizer = new wxBoxSizer(wxHORIZONTAL);
        sshKeySizer->Add(new wxStaticText(scrolled, wxID_ANY, "Key:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshKeyCtrl = new wxTextCtrl(scrolled, wxID_ANY, "");
        m_sshKeyCtrl->Enable(false);
        m_sshKeyCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnControlChange, this);
        sshKeySizer->Add(m_sshKeyCtrl, 1, wxEXPAND | wxRIGHT, 4);
        m_browseSshKeyBtn = new wxButton(scrolled, wxID_ANY, "Browse");
        m_browseSshKeyBtn->Enable(false);
        m_browseSshKeyBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnBrowseSshKey, this);
        sshKeySizer->Add(m_browseSshKeyBtn, 0);
        sshBox->Add(sshKeySizer, 0, wxEXPAND | wxALL, 4);

        wxBoxSizer *sshPassSizer = new wxBoxSizer(wxHORIZONTAL);
        sshPassSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Password:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshPassCtrl = new wxTextCtrl(scrolled, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
        m_sshPassCtrl->Enable(false);
        m_sshPassCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnControlChange, this);
        sshPassSizer->Add(m_sshPassCtrl, 1, wxEXPAND);
        sshBox->Add(sshPassSizer, 0, wxEXPAND | wxALL, 4);

        wxBoxSizer *sshKnownSizer = new wxBoxSizer(wxHORIZONTAL);
        sshKnownSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Known Hosts:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshKnownHostsCtrl = new wxTextCtrl(scrolled, wxID_ANY, "");
        m_sshKnownHostsCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnControlChange, this);
        sshKnownSizer->Add(m_sshKnownHostsCtrl, 1, wxEXPAND);
        sshBox->Add(sshKnownSizer, 0, wxEXPAND | wxALL, 4);

        m_sshX11Check = new wxCheckBox(scrolled, wxID_ANY, "X11 Forwarding (default on)");
        m_sshX11Check->SetValue(true);
        m_sshX11Check->Bind(wxEVT_CHECKBOX, &FelixTerminalFrame::OnControlChange, this);
        sshBox->Add(m_sshX11Check, 0, wxALL, 4);

        scrolledSizer->Add(sshBox, 0, wxEXPAND | wxALL, 8);
        scrolledSizer->AddStretchSpacer();

        scrolled->SetSizer(scrolledSizer);
        sizer->Add(scrolled, 1, wxEXPAND);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Connection");
    }

    void CreatePortForwardPanel(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        wxScrolledWindow *scrolled = new wxScrolledWindow(panel, wxID_ANY);
        scrolled->SetScrollRate(5, 5);
        wxBoxSizer *scrolledSizer = new wxBoxSizer(wxVERTICAL);

        scrolledSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Port Forwarding (SSH only)"), 0, wxALL, 8);

        // Local port forwarding
        scrolledSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Local Port Forward (-L): local_port:remote_host:remote_port"), 0, wxALL, 8);
        wxBoxSizer *localInputSizer = new wxBoxSizer(wxHORIZONTAL);
        wxTextCtrl *localPFCtrl = new wxTextCtrl(scrolled, wxID_ANY, "");
        localInputSizer->Add(localPFCtrl, 1, wxEXPAND | wxRIGHT, 4);
        wxButton *addLocalBtn = new wxButton(scrolled, wxID_ANY, "Add");
        addLocalBtn->Bind(wxEVT_BUTTON, [this, localPFCtrl](wxCommandEvent&) {
            if (!localPFCtrl->IsEmpty()) {
                m_localPFEntries.push_back(localPFCtrl->GetValue());
                m_localPFList->Append(localPFCtrl->GetValue());
                localPFCtrl->Clear();
                UpdatePreview();
            }
        });
        localInputSizer->Add(addLocalBtn, 0);
        scrolledSizer->Add(localInputSizer, 0, wxEXPAND | wxALL, 4);
        m_localPFList = new wxListBox(scrolled, wxID_ANY, wxDefaultPosition, wxSize(-1, 80));
        scrolledSizer->Add(m_localPFList, 0, wxEXPAND | wxALL, 4);
        wxButton *removeLocalBtn = new wxButton(scrolled, wxID_ANY, "Remove Selected");
        removeLocalBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            int sel = m_localPFList->GetSelection();
            if (sel != wxNOT_FOUND) {
                m_localPFList->Delete(sel);
                m_localPFEntries.erase(m_localPFEntries.begin() + sel);
                UpdatePreview();
            }
        });
        scrolledSizer->Add(removeLocalBtn, 0, wxALL, 4);

        scrolledSizer->Add(new wxStaticLine(scrolled), 0, wxEXPAND | wxALL, 8);

        // Remote port forwarding
        scrolledSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Remote Port Forward (-R): remote_port:local_host:local_port"), 0, wxALL, 8);
        wxBoxSizer *remoteInputSizer = new wxBoxSizer(wxHORIZONTAL);
        wxTextCtrl *remotePFCtrl = new wxTextCtrl(scrolled, wxID_ANY, "");
        remoteInputSizer->Add(remotePFCtrl, 1, wxEXPAND | wxRIGHT, 4);
        wxButton *addRemoteBtn = new wxButton(scrolled, wxID_ANY, "Add");
        addRemoteBtn->Bind(wxEVT_BUTTON, [this, remotePFCtrl](wxCommandEvent&) {
            if (!remotePFCtrl->IsEmpty()) {
                m_remotePFEntries.push_back(remotePFCtrl->GetValue());
                m_remotePFList->Append(remotePFCtrl->GetValue());
                remotePFCtrl->Clear();
                UpdatePreview();
            }
        });
        remoteInputSizer->Add(addRemoteBtn, 0);
        scrolledSizer->Add(remoteInputSizer, 0, wxEXPAND | wxALL, 4);
        m_remotePFList = new wxListBox(scrolled, wxID_ANY, wxDefaultPosition, wxSize(-1, 80));
        scrolledSizer->Add(m_remotePFList, 0, wxEXPAND | wxALL, 4);
        wxButton *removeRemoteBtn = new wxButton(scrolled, wxID_ANY, "Remove Selected");
        removeRemoteBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            int sel = m_remotePFList->GetSelection();
            if (sel != wxNOT_FOUND) {
                m_remotePFList->Delete(sel);
                m_remotePFEntries.erase(m_remotePFEntries.begin() + sel);
                UpdatePreview();
            }
        });
        scrolledSizer->Add(removeRemoteBtn, 0, wxALL, 4);

        scrolledSizer->Add(new wxStaticLine(scrolled), 0, wxEXPAND | wxALL, 8);

        // SOCKS5 dynamic forwarding
        scrolledSizer->Add(new wxStaticText(scrolled, wxID_ANY, "SOCKS5 Dynamic (-D): port"), 0, wxALL, 8);
        wxBoxSizer *socksInputSizer = new wxBoxSizer(wxHORIZONTAL);
        wxTextCtrl *socksCtrl = new wxTextCtrl(scrolled, wxID_ANY, "");
        socksInputSizer->Add(socksCtrl, 1, wxEXPAND | wxRIGHT, 4);
        wxButton *addSocksBtn = new wxButton(scrolled, wxID_ANY, "Add");
        addSocksBtn->Bind(wxEVT_BUTTON, [this, socksCtrl](wxCommandEvent&) {
            if (!socksCtrl->IsEmpty()) {
                m_socksEntries.push_back(socksCtrl->GetValue());
                m_socksList->Append(socksCtrl->GetValue());
                socksCtrl->Clear();
                UpdatePreview();
            }
        });
        socksInputSizer->Add(addSocksBtn, 0);
        scrolledSizer->Add(socksInputSizer, 0, wxEXPAND | wxALL, 4);
        m_socksList = new wxListBox(scrolled, wxID_ANY, wxDefaultPosition, wxSize(-1, 80));
        scrolledSizer->Add(m_socksList, 0, wxEXPAND | wxALL, 4);
        wxButton *removeSocksBtn = new wxButton(scrolled, wxID_ANY, "Remove Selected");
        removeSocksBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            int sel = m_socksList->GetSelection();
            if (sel != wxNOT_FOUND) {
                m_socksList->Delete(sel);
                m_socksEntries.erase(m_socksEntries.begin() + sel);
                UpdatePreview();
            }
        });
        scrolledSizer->Add(removeSocksBtn, 0, wxALL, 4);
        scrolledSizer->AddStretchSpacer();

        scrolled->SetSizer(scrolledSizer);
        sizer->Add(scrolled, 1, wxEXPAND);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Port Forward");
    }

    void CreateConfigDir() {
        wxString configPath = wxStandardPaths::Get().GetUserConfigDir() + wxFILE_SEP_PATH + "felixterminal";
        if (!wxDirExists(configPath)) {
            wxMkdir(configPath);
        }
    }

    void LoadSessions() {
        m_sessionList->DeleteAllItems();
        wxString configPath = wxStandardPaths::Get().GetUserConfigDir() + wxFILE_SEP_PATH + "felixterminal";
        wxDir dir(configPath);
        wxString filename;
        long index = 0;
        if (dir.IsOpened()) {
            bool cont = dir.GetFirst(&filename, "*.cfg", wxDIR_FILES);
            while (cont) {
                m_sessionList->InsertItem(index++, filename.BeforeLast('.'));
                cont = dir.GetNext(&filename);
            }
        }
    }

    void OnConnTypeChange(wxCommandEvent &event) {
        UpdatePreview();
    }

    void OnSSHAuthChange(wxCommandEvent &event) {
        int sel = m_sshAuthChoice->GetSelection();
        m_sshKeyCtrl->Enable(sel == 1);
        m_browseSshKeyBtn->Enable(sel == 1);
        m_sshPassCtrl->Enable(sel == 2);
        UpdatePreview();
    }

    void OnBrowseSshKey(wxCommandEvent &event) {
        wxString homeDir = wxGetHomeDir();
        wxFileDialog dlg(this, "Select SSH Private Key", homeDir + "/.ssh", "", 
                        "All files (*)|*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            m_sshKeyCtrl->SetValue(dlg.GetPath());
            UpdatePreview();
        }
    }

    void OnControlChange(wxCommandEvent &event) {
        UpdatePreview();
    }

    void UpdatePreview() {
        std::stringstream cmd;
        cmd << FLT_EXECUTABLE;

        int connType = m_connTypeChoice->GetSelection();

        if (connType == 1) {  // SSH
            cmd << " --ssh ";
            wxString user = m_sshUserCtrl->GetValue();
            wxString host = m_sshHostCtrl->GetValue();
            int port = m_sshPortSpin->GetValue();
            
            if (!user.empty()) cmd << std::string(user.mb_str()) << "@";
            cmd << std::string(host.mb_str());
            if (port != 22) cmd << ":" << port;

            int authSel = m_sshAuthChoice->GetSelection();
            if (authSel == 1 && !m_sshKeyCtrl->IsEmpty()) {
                cmd << " -i " << std::string(m_sshKeyCtrl->GetValue().mb_str());
            }
            if (authSel == 2 && !m_sshPassCtrl->IsEmpty()) {
                cmd << " --ssh-password " << std::string(m_sshPassCtrl->GetValue().mb_str());
            }
            if (!m_sshKnownHostsCtrl->IsEmpty()) {
                cmd << " --ssh-known-hosts " << std::string(m_sshKnownHostsCtrl->GetValue().mb_str());
            }
            if (!m_sshX11Check->GetValue()) {
                cmd << " --no-x11";
            }

            for (const auto &pf : m_localPFEntries) {
                cmd << " -L " << std::string(pf.mb_str());
            }
            for (const auto &pf : m_remotePFEntries) {
                cmd << " -R " << std::string(pf.mb_str());
            }
            for (const auto &socks : m_socksEntries) {
                cmd << " -D " << std::string(socks.mb_str());
            }
        }
        else if (connType == 0) {  // Local shell
            wxString shell = m_localShellCtrl->GetValue();
            if (!shell.empty()) {
                cmd << " " << std::string(shell.mb_str());
            }
        }

        m_cmdPreview->SetValue(cmd.str());
    }

    void OnLoadSession(wxCommandEvent &event) {
        int sel = m_sessionList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (sel == -1) {
            wxMessageBox("Please select a session", "Error", wxOK | wxICON_ERROR);
            return;
        }

        wxString sessionName = m_sessionList->GetItemText(sel);
        SessionConfig cfg = LoadSessionConfig(sessionName);
        LoadSessionToUI(cfg);
        m_sessionNameCtrl->SetValue(sessionName);
    }

    void OnSaveSession(wxCommandEvent &event) {
        wxString sessionName = m_sessionNameCtrl->GetValue();
        if (sessionName.empty()) {
            wxMessageBox("Please enter a session name", "Error", wxOK | wxICON_ERROR);
            return;
        }

        SessionConfig cfg = GetCurrentConfig(sessionName);
        SaveSessionConfig(cfg);

        bool found = false;
        for (int i = 0; i < (int)m_sessionList->GetItemCount(); i++) {
            if (m_sessionList->GetItemText(i) == sessionName) {
                found = true;
                break;
            }
        }
        if (!found) {
            m_sessionList->InsertItem(m_sessionList->GetItemCount(), sessionName);
        }

        wxMessageBox("Session saved!", "Success", wxOK | wxICON_INFORMATION);
    }

    void OnDeleteSession(wxCommandEvent &event) {
        int sel = m_sessionList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (sel == -1) {
            wxMessageBox("Please select a session", "Error", wxOK | wxICON_ERROR);
            return;
        }

        wxString sessionName = m_sessionList->GetItemText(sel);
        wxString configPath = wxStandardPaths::Get().GetUserConfigDir() + wxFILE_SEP_PATH + 
                             "felixterminal" + wxFILE_SEP_PATH + sessionName + ".cfg";
        
        if (wxFileExists(configPath)) {
            wxRemoveFile(configPath);
            m_sessionList->DeleteItem(sel);
            wxMessageBox("Session deleted!", "Success", wxOK | wxICON_INFORMATION);
        }
    }

    void OnOpen(wxCommandEvent &event) {
        wxString cmdLine = m_cmdPreview->GetValue();
        wxExecute(cmdLine, wxEXEC_ASYNC);
        wxMessageBox("Felix Terminal launched!", "Success", wxOK | wxICON_INFORMATION);
    }

    void OnClose(wxCommandEvent &event) {
        Close();
    }

    SessionConfig GetCurrentConfig(const wxString &name) {
        SessionConfig cfg;
        cfg.name = name;
        cfg.connType = m_connTypeChoice->GetSelection();
        cfg.localShell = m_localShellCtrl->GetValue();
        cfg.sshUser = m_sshUserCtrl->GetValue();
        cfg.sshHost = m_sshHostCtrl->GetValue();
        cfg.sshPort = m_sshPortSpin->GetValue();
        cfg.sshAuthMethod = m_sshAuthChoice->GetSelection();
        cfg.sshKeyPath = m_sshKeyCtrl->GetValue();
        cfg.sshPassword = m_sshPassCtrl->GetValue();
        cfg.sshKnownHosts = m_sshKnownHostsCtrl->GetValue();
        cfg.sshX11 = m_sshX11Check->GetValue();
        cfg.localPF = m_localPFEntries;
        cfg.remotePF = m_remotePFEntries;
        cfg.socks = m_socksEntries;
        return cfg;
    }

    SessionConfig LoadSessionConfig(const wxString &name) {
        SessionConfig cfg;
        wxString configPath = wxStandardPaths::Get().GetUserConfigDir() + wxFILE_SEP_PATH + 
                             "felixterminal" + wxFILE_SEP_PATH + name + ".cfg";
        
        wxTextFile file(configPath);
        if (file.Exists()) {
            file.Open();
            for (size_t i = 0; i < file.GetLineCount(); i++) {
                wxString line = file[i];
                if (line.empty() || line[0] == '#') continue;
                
                wxString key = line.BeforeFirst('=');
                wxString val = line.AfterFirst('=');
                
                if (key == "connType") cfg.connType = wxAtoi(val);
                else if (key == "localShell") cfg.localShell = val;
                else if (key == "sshUser") cfg.sshUser = val;
                else if (key == "sshHost") cfg.sshHost = val;
                else if (key == "sshPort") cfg.sshPort = wxAtoi(val);
                else if (key == "sshAuthMethod") cfg.sshAuthMethod = wxAtoi(val);
                else if (key == "sshKeyPath") cfg.sshKeyPath = val;
                else if (key == "sshPassword") cfg.sshPassword = val;
                else if (key == "sshKnownHosts") cfg.sshKnownHosts = val;
                else if (key == "sshX11") cfg.sshX11 = (val == "1");
                else if (key.StartsWith("localPF_")) cfg.localPF.push_back(val);
                else if (key.StartsWith("remotePF_")) cfg.remotePF.push_back(val);
                else if (key.StartsWith("socks_")) cfg.socks.push_back(val);
            }
            file.Close();
        }
        return cfg;
    }

    void SaveSessionConfig(const SessionConfig &cfg) {
        wxString configPath = wxStandardPaths::Get().GetUserConfigDir() + wxFILE_SEP_PATH + 
                             "felixterminal" + wxFILE_SEP_PATH + cfg.name + ".cfg";
        
        wxTextFile file(configPath);
        if (file.Exists()) file.Open();
        else file.Create();
        
        file.Clear();
        file.AddLine("# Felix Terminal Session Config");
        file.AddLine(wxString::Format("connType=%d", cfg.connType));
        file.AddLine("localShell=" + cfg.localShell);
        file.AddLine("sshUser=" + cfg.sshUser);
        file.AddLine("sshHost=" + cfg.sshHost);
        file.AddLine(wxString::Format("sshPort=%d", cfg.sshPort));
        file.AddLine(wxString::Format("sshAuthMethod=%d", cfg.sshAuthMethod));
        file.AddLine("sshKeyPath=" + cfg.sshKeyPath);
        file.AddLine("sshPassword=" + cfg.sshPassword);
        file.AddLine("sshKnownHosts=" + cfg.sshKnownHosts);
        file.AddLine(wxString::Format("sshX11=%d", cfg.sshX11 ? 1 : 0));
        
        for (size_t i = 0; i < cfg.localPF.size(); i++) {
            file.AddLine(wxString::Format("localPF_%zu=%s", i, cfg.localPF[i]));
        }
        for (size_t i = 0; i < cfg.remotePF.size(); i++) {
            file.AddLine(wxString::Format("remotePF_%zu=%s", i, cfg.remotePF[i]));
        }
        for (size_t i = 0; i < cfg.socks.size(); i++) {
            file.AddLine(wxString::Format("socks_%zu=%s", i, cfg.socks[i]));
        }
        
        file.Write();
        file.Close();
    }

    void LoadSessionToUI(const SessionConfig &cfg) {
        m_connTypeChoice->SetSelection(cfg.connType);
        m_localShellCtrl->SetValue(cfg.localShell);
        m_sshUserCtrl->SetValue(cfg.sshUser);
        m_sshHostCtrl->SetValue(cfg.sshHost);
        m_sshPortSpin->SetValue(cfg.sshPort);
        m_sshAuthChoice->SetSelection(cfg.sshAuthMethod);
        m_sshKeyCtrl->SetValue(cfg.sshKeyPath);
        m_sshPassCtrl->SetValue(cfg.sshPassword);
        m_sshKnownHostsCtrl->SetValue(cfg.sshKnownHosts);
        m_sshX11Check->SetValue(cfg.sshX11);
        
        m_localPFList->Clear();
        m_localPFEntries.clear();
        for (const auto &pf : cfg.localPF) {
            m_localPFList->Append(pf);
            m_localPFEntries.push_back(pf);
        }
        
        m_remotePFList->Clear();
        m_remotePFEntries.clear();
        for (const auto &pf : cfg.remotePF) {
            m_remotePFList->Append(pf);
            m_remotePFEntries.push_back(pf);
        }
        
        m_socksList->Clear();
        m_socksEntries.clear();
        for (const auto &s : cfg.socks) {
            m_socksList->Append(s);
            m_socksEntries.push_back(s);
        }
        
        wxCommandEvent dummy;
        OnSSHAuthChange(dummy);
        UpdatePreview();
    }
};

class FelixApp : public wxApp {
public:
    bool OnInit() override {
        FelixTerminalFrame *frame = new FelixTerminalFrame();
        frame->Show();
        return true;
    }
};

wxIMPLEMENT_APP(FelixApp);
