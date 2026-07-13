#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/listctrl.h>
#include <wx/textfile.h>
#include <wx/stdpaths.h>
#include <wx/dir.h>
#include <wx/filefn.h>
#ifdef _WIN32
#include <wx/msw/registry.h>
#endif
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
    
    // Telnet
    wxString telnetHost;
    int telnetPort = 23;
    wxString telnetTType = "xterm-256color";
    bool telnetRaw = false;
    bool telnetSSL = false;
    
    // Serial
    wxString serialPort;
    int serialBaud = 9600;
    
    // SSH
    wxString sshUser;
    wxString sshHost = "localhost";
    int sshPort = 22;
    int sshAuthMethod = 0;
    wxString sshKeyPath;
    wxString sshKnownHosts;
    bool sshX11 = true;
    wxString sshCommand;  // Command to execute via SSH
    std::vector<wxString> localPF;
    std::vector<wxString> remotePF;
    std::vector<wxString> socks;
    
    // Web Server
    bool webServerEnabled = false;
    wxString webServerAddr = "127.0.0.1";
    int webServerPort = 53716;
    wxString webRootDir = "/";
};

class FelixTerminalFrame : public wxFrame {
public:
    FelixTerminalFrame() : wxFrame(nullptr, wxID_ANY, "Felix Terminal", 
                                   wxDefaultPosition, wxSize(950, 650)) {
        
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

        // ---- Connection Tab (for type selection) ----
        CreateConnectionPanel(notebook, mainPanel);
        // ---- Local Shell Tab ----
        CreateLocalShellTab(notebook, mainPanel);
        // ---- Telnet Tab ----
        CreateTelnetTab(notebook, mainPanel);
        // ---- Serial Tab ----
        CreateSerialTab(notebook, mainPanel);
        // ---- SSH Tab ----
        CreateSSHTab(notebook, mainPanel);
        // ---- Port Forward Tab ----
        CreatePortForwardPanel(notebook, mainPanel);
        // ---- Web Server Tab ----
        CreateWebServerPanel(notebook, mainPanel);

        rightSizer->Add(notebook, 1, wxEXPAND | wxALL, 8);

        // ---- Command Preview ----
        wxStaticBoxSizer *cmdSizer = new wxStaticBoxSizer(wxVERTICAL, mainPanel, "Command Line");
        m_cmdPreview = new wxTextCtrl(mainPanel, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 50), 
                                       wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
        cmdSizer->Add(m_cmdPreview, 1, wxEXPAND | wxALL, 4);
        rightSizer->Add(cmdSizer, 0, wxEXPAND | wxALL, 8);

        // ---- Buttons ----
        wxBoxSizer *bottomBtnSizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton *clearBtn = new wxButton(mainPanel, wxID_ANY, "Clear");
        m_openBtn = new wxButton(mainPanel, wxID_ANY, "Open");
        m_closeBtn = new wxButton(mainPanel, wxID_CLOSE, "Close");
        
        clearBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnClear, this);
        m_openBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnOpen, this);
        m_closeBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnClose, this);

        bottomBtnSizer->Add(clearBtn, 0, wxRIGHT, 4);
        bottomBtnSizer->AddStretchSpacer();
        bottomBtnSizer->Add(m_openBtn, 0, wxRIGHT, 4);
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
    
    // Telnet
    wxTextCtrl *m_telnetHostCtrl;
    wxSpinCtrl *m_telnetPortSpin;
    wxTextCtrl *m_telnetTTypeCtrl;
    wxCheckBox *m_telnetRawCheck;
    wxCheckBox *m_telnetSSLCheck;
    
    // Serial
    wxTextCtrl *m_serialPortCtrl;
    wxChoice *m_serialBaudChoice;
    
    // SSH
    wxTextCtrl *m_sshUserCtrl;
    wxTextCtrl *m_sshHostCtrl;
    wxSpinCtrl *m_sshPortSpin;
    wxChoice *m_sshAuthChoice;
    wxTextCtrl *m_sshKeyCtrl;
    wxButton *m_browseSshKeyBtn;
    wxTextCtrl *m_sshKnownHostsCtrl;
    wxCheckBox *m_sshX11Check;
    wxTextCtrl *m_sshCommandCtrl;  // SSH command to execute

    // Port Forwarding
    wxListBox *m_localPFList;
    wxListBox *m_remotePFList;
    wxListBox *m_socksList;
    std::vector<wxString> m_localPFEntries;
    std::vector<wxString> m_remotePFEntries;
    std::vector<wxString> m_socksEntries;

    // Web Server
    wxCheckBox *m_webServerEnabledCheck;
    wxTextCtrl *m_webServerAddrCtrl;
    wxSpinCtrl *m_webServerPortSpin;
    wxTextCtrl *m_webRootDirCtrl;
    wxButton *m_browseWebRootBtn;

    void CreateConnectionPanel(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        
        wxStaticBoxSizer *typeBox = new wxStaticBoxSizer(wxVERTICAL, panel, "Connection Type");
        
        wxBoxSizer *typeSizer = new wxBoxSizer(wxHORIZONTAL);
        typeSizer->Add(new wxStaticText(panel, wxID_ANY, "Type:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        
        m_connTypeChoice = new wxChoice(panel, wxID_ANY);
        m_connTypeChoice->Append("Local Shell");
        m_connTypeChoice->Append("Telnet");
        m_connTypeChoice->Append("Serial");
        m_connTypeChoice->Append("SSH");
        m_connTypeChoice->SetSelection(0);
        m_connTypeChoice->Bind(wxEVT_CHOICE, &FelixTerminalFrame::OnConnTypeChange, this);
        
        typeSizer->Add(m_connTypeChoice, 1, wxEXPAND);
        typeBox->Add(typeSizer, 0, wxEXPAND | wxALL, 8);
        sizer->Add(typeBox, 0, wxEXPAND | wxALL, 8);
        
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Connection");
    }

    void CreateLocalShellTab(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        
        wxStaticBoxSizer *shellBox = new wxStaticBoxSizer(wxVERTICAL, panel, "Local Shell");
        wxBoxSizer *shellSizer = new wxBoxSizer(wxHORIZONTAL);
        shellSizer->Add(new wxStaticText(panel, wxID_ANY, "Shell:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        
        m_localShellCtrl = new wxTextCtrl(panel, wxID_ANY, "/bin/bash");
        shellSizer->Add(m_localShellCtrl, 1, wxEXPAND);
        shellBox->Add(shellSizer, 0, wxEXPAND | wxALL, 8);
        sizer->Add(shellBox, 0, wxEXPAND | wxALL, 8);
        
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Local Shell");
    }

    void CreateTelnetTab(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        
        wxStaticBoxSizer *telnetBox = new wxStaticBoxSizer(wxVERTICAL, panel, "Telnet Settings");
        
        wxBoxSizer *hostSizer = new wxBoxSizer(wxHORIZONTAL);
        hostSizer->Add(new wxStaticText(panel, wxID_ANY, "Host:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_telnetHostCtrl = new wxTextCtrl(panel, wxID_ANY, "localhost");
        hostSizer->Add(m_telnetHostCtrl, 1, wxEXPAND);
        telnetBox->Add(hostSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *portSizer = new wxBoxSizer(wxHORIZONTAL);
        portSizer->Add(new wxStaticText(panel, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_telnetPortSpin = new wxSpinCtrl(panel, wxID_ANY, "23", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, 23);
        portSizer->Add(m_telnetPortSpin, 1, wxEXPAND);
        telnetBox->Add(portSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *ttypeSizer = new wxBoxSizer(wxHORIZONTAL);
        ttypeSizer->Add(new wxStaticText(panel, wxID_ANY, "Terminal Type:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_telnetTTypeCtrl = new wxTextCtrl(panel, wxID_ANY, "xterm-256color");
        ttypeSizer->Add(m_telnetTTypeCtrl, 1, wxEXPAND);
        telnetBox->Add(ttypeSizer, 0, wxEXPAND | wxALL, 8);
        
        m_telnetRawCheck = new wxCheckBox(panel, wxID_ANY, "Raw Mode");
        telnetBox->Add(m_telnetRawCheck, 0, wxALL, 8);
        
        m_telnetSSLCheck = new wxCheckBox(panel, wxID_ANY, "Use SSL/TLS");
        telnetBox->Add(m_telnetSSLCheck, 0, wxALL, 8);
        
        sizer->Add(telnetBox, 0, wxEXPAND | wxALL, 8);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Telnet");
    }

    void CreateSerialTab(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        
        wxStaticBoxSizer *serialBox = new wxStaticBoxSizer(wxVERTICAL, panel, "Serial Settings");
        
        wxBoxSizer *portSizer = new wxBoxSizer(wxHORIZONTAL);
        portSizer->Add(new wxStaticText(panel, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_serialPortCtrl = new wxTextCtrl(panel, wxID_ANY, "COM1");
        portSizer->Add(m_serialPortCtrl, 1, wxEXPAND);
        serialBox->Add(portSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *baudSizer = new wxBoxSizer(wxHORIZONTAL);
        baudSizer->Add(new wxStaticText(panel, wxID_ANY, "Baud Rate:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_serialBaudChoice = new wxChoice(panel, wxID_ANY);
        m_serialBaudChoice->Append("1200");
        m_serialBaudChoice->Append("2400");
        m_serialBaudChoice->Append("4800");
        m_serialBaudChoice->Append("9600");
        m_serialBaudChoice->Append("19200");
        m_serialBaudChoice->Append("38400");
        m_serialBaudChoice->Append("57600");
        m_serialBaudChoice->Append("115200");
        m_serialBaudChoice->SetSelection(3);  // Default 9600
        baudSizer->Add(m_serialBaudChoice, 1, wxEXPAND);
        serialBox->Add(baudSizer, 0, wxEXPAND | wxALL, 8);
        
        sizer->Add(serialBox, 0, wxEXPAND | wxALL, 8);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Serial");
    }

    void CreateSSHTab(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        
        wxStaticBoxSizer *sshBox = new wxStaticBoxSizer(wxVERTICAL, panel, "SSH Settings");
        
        wxBoxSizer *userSizer = new wxBoxSizer(wxHORIZONTAL);
        userSizer->Add(new wxStaticText(panel, wxID_ANY, "User:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshUserCtrl = new wxTextCtrl(panel, wxID_ANY, "root");
        userSizer->Add(m_sshUserCtrl, 1, wxEXPAND);
        sshBox->Add(userSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *hostSizer = new wxBoxSizer(wxHORIZONTAL);
        hostSizer->Add(new wxStaticText(panel, wxID_ANY, "Host:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshHostCtrl = new wxTextCtrl(panel, wxID_ANY, "localhost");
        hostSizer->Add(m_sshHostCtrl, 1, wxEXPAND);
        sshBox->Add(hostSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *portSizer = new wxBoxSizer(wxHORIZONTAL);
        portSizer->Add(new wxStaticText(panel, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshPortSpin = new wxSpinCtrl(panel, wxID_ANY, "22", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, 22);
        portSizer->Add(m_sshPortSpin, 1, wxEXPAND);
        sshBox->Add(portSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *authSizer = new wxBoxSizer(wxHORIZONTAL);
        authSizer->Add(new wxStaticText(panel, wxID_ANY, "Auth Method:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshAuthChoice = new wxChoice(panel, wxID_ANY);
        m_sshAuthChoice->Append("Password");
        m_sshAuthChoice->Append("Public Key");
        m_sshAuthChoice->SetSelection(0);
        m_sshAuthChoice->Bind(wxEVT_CHOICE, &FelixTerminalFrame::OnSSHAuthChange, this);
        authSizer->Add(m_sshAuthChoice, 1, wxEXPAND);
        sshBox->Add(authSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *keySizer = new wxBoxSizer(wxHORIZONTAL);
        keySizer->Add(new wxStaticText(panel, wxID_ANY, "Key Path:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshKeyCtrl = new wxTextCtrl(panel, wxID_ANY, "");
        keySizer->Add(m_sshKeyCtrl, 1, wxEXPAND | wxRIGHT, 4);
        m_browseSshKeyBtn = new wxButton(panel, wxID_ANY, "Browse");
        m_browseSshKeyBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnBrowseSshKey, this);
        keySizer->Add(m_browseSshKeyBtn, 0, wxEXPAND);
        sshBox->Add(keySizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *knownHostsSizer = new wxBoxSizer(wxHORIZONTAL);
        knownHostsSizer->Add(new wxStaticText(panel, wxID_ANY, "Known Hosts:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshKnownHostsCtrl = new wxTextCtrl(panel, wxID_ANY, "");
        knownHostsSizer->Add(m_sshKnownHostsCtrl, 1, wxEXPAND);
        sshBox->Add(knownHostsSizer, 0, wxEXPAND | wxALL, 8);
        
        m_sshX11Check = new wxCheckBox(panel, wxID_ANY, "X11 Forwarding");
        m_sshX11Check->SetValue(true);
        sshBox->Add(m_sshX11Check, 0, wxALL, 8);
        
        wxBoxSizer *cmdSizer = new wxBoxSizer(wxHORIZONTAL);
        cmdSizer->Add(new wxStaticText(panel, wxID_ANY, "Command:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshCommandCtrl = new wxTextCtrl(panel, wxID_ANY, "");
        cmdSizer->Add(m_sshCommandCtrl, 1, wxEXPAND);
        sshBox->Add(cmdSizer, 0, wxEXPAND | wxALL, 8);
        
        sizer->Add(sshBox, 0, wxEXPAND | wxALL, 8);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "SSH");
    }

    void CreatePortForwardPanel(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);
        
        // Local Port Forward
        wxBoxSizer *localSizer = new wxBoxSizer(wxVERTICAL);
        localSizer->Add(new wxStaticText(panel, wxID_ANY, "Local Port Forward (L)"), 0, wxALL, 8);
        m_localPFList = new wxListBox(panel, wxID_ANY);
        localSizer->Add(m_localPFList, 1, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *localBtnSizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton *localAddBtn = new wxButton(panel, wxID_ANY, "Add");
        wxButton *localRemoveBtn = new wxButton(panel, wxID_ANY, "Remove");
        localAddBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnAddLocalPF, this);
        localRemoveBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnRemoveLocalPF, this);
        localBtnSizer->Add(localAddBtn, 0, wxRIGHT, 4);
        localBtnSizer->Add(localRemoveBtn, 0);
        localSizer->Add(localBtnSizer, 0, wxEXPAND | wxALL, 8);
        
        // Remote Port Forward
        wxBoxSizer *remoteSizer = new wxBoxSizer(wxVERTICAL);
        remoteSizer->Add(new wxStaticText(panel, wxID_ANY, "Remote Port Forward (R)"), 0, wxALL, 8);
        m_remotePFList = new wxListBox(panel, wxID_ANY);
        remoteSizer->Add(m_remotePFList, 1, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *remoteBtnSizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton *remoteAddBtn = new wxButton(panel, wxID_ANY, "Add");
        wxButton *remoteRemoveBtn = new wxButton(panel, wxID_ANY, "Remove");
        remoteAddBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnAddRemotePF, this);
        remoteRemoveBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnRemoveRemotePF, this);
        remoteBtnSizer->Add(remoteAddBtn, 0, wxRIGHT, 4);
        remoteBtnSizer->Add(remoteRemoveBtn, 0);
        remoteSizer->Add(remoteBtnSizer, 0, wxEXPAND | wxALL, 8);
        
        // SOCKS Forward
        wxBoxSizer *socksSizer = new wxBoxSizer(wxVERTICAL);
        socksSizer->Add(new wxStaticText(panel, wxID_ANY, "SOCKS Forward (D)"), 0, wxALL, 8);
        m_socksList = new wxListBox(panel, wxID_ANY);
        socksSizer->Add(m_socksList, 1, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *socksBtnSizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton *socksAddBtn = new wxButton(panel, wxID_ANY, "Add");
        wxButton *socksRemoveBtn = new wxButton(panel, wxID_ANY, "Remove");
        socksAddBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnAddSOCKS, this);
        socksRemoveBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnRemoveSOCKS, this);
        socksBtnSizer->Add(socksAddBtn, 0, wxRIGHT, 4);
        socksBtnSizer->Add(socksRemoveBtn, 0);
        socksSizer->Add(socksBtnSizer, 0, wxEXPAND | wxALL, 8);
        
        sizer->Add(localSizer, 1, wxEXPAND | wxALL, 0);
        sizer->Add(remoteSizer, 1, wxEXPAND | wxALL, 0);
        sizer->Add(socksSizer, 1, wxEXPAND | wxALL, 0);
        
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Port Forward");
    }

    void CreateWebServerPanel(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        
        wxStaticBoxSizer *webBox = new wxStaticBoxSizer(wxVERTICAL, panel, "Web Server Settings");
        
        m_webServerEnabledCheck = new wxCheckBox(panel, wxID_ANY, "Enable Web Server");
        webBox->Add(m_webServerEnabledCheck, 0, wxALL, 8);
        
        wxBoxSizer *addrSizer = new wxBoxSizer(wxHORIZONTAL);
        addrSizer->Add(new wxStaticText(panel, wxID_ANY, "Listen Address:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_webServerAddrCtrl = new wxTextCtrl(panel, wxID_ANY, "127.0.0.1");
        addrSizer->Add(m_webServerAddrCtrl, 1, wxEXPAND);
        webBox->Add(addrSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *portSizer = new wxBoxSizer(wxHORIZONTAL);
        portSizer->Add(new wxStaticText(panel, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_webServerPortSpin = new wxSpinCtrl(panel, wxID_ANY, "53716", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, 53716);
        portSizer->Add(m_webServerPortSpin, 1, wxEXPAND);
        webBox->Add(portSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *dirSizer = new wxBoxSizer(wxHORIZONTAL);
        dirSizer->Add(new wxStaticText(panel, wxID_ANY, "Root Directory:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_webRootDirCtrl = new wxTextCtrl(panel, wxID_ANY, "/");
        dirSizer->Add(m_webRootDirCtrl, 1, wxEXPAND | wxRIGHT, 4);
        m_browseWebRootBtn = new wxButton(panel, wxID_ANY, "Browse");
        m_browseWebRootBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnBrowseWebRoot, this);
        dirSizer->Add(m_browseWebRootBtn, 0, wxEXPAND);
        webBox->Add(dirSizer, 0, wxEXPAND | wxALL, 8);
        
        sizer->Add(webBox, 0, wxEXPAND | wxALL, 8);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Web Server");
    }

    void OnConnTypeChange(wxCommandEvent &event) {
        UpdatePreview();
    }

    void OnSSHAuthChange(wxCommandEvent &event) {
        int selection = m_sshAuthChoice->GetSelection();
        m_sshKeyCtrl->Enable(selection == 1);  // Enable key path only for public key auth
        m_browseSshKeyBtn->Enable(selection == 1);
    }

    void OnBrowseSshKey(wxCommandEvent &event) {
        wxFileDialog dlg(this, "Select SSH Key", "", "", "PEM files (*.pem)|*.pem|All files (*.*)|*.*", wxFD_OPEN);
        if (dlg.ShowModal() == wxID_OK) {
            m_sshKeyCtrl->SetValue(dlg.GetPath());
        }
    }

    void OnBrowseWebRoot(wxCommandEvent &event) {
        wxDirDialog dlg(this, "Select Web Root Directory");
        if (dlg.ShowModal() == wxID_OK) {
            m_webRootDirCtrl->SetValue(dlg.GetPath());
        }
    }

    void OnAddLocalPF(wxCommandEvent &event) {
        wxTextEntryDialog dlg(this, "Enter local port forward (e.g., 8080:remote:80):");
        if (dlg.ShowModal() == wxID_OK) {
            wxString entry = dlg.GetValue();
            if (!entry.empty()) {
                m_localPFList->Append(entry);
                m_localPFEntries.push_back(entry);
            }
        }
        UpdatePreview();
    }

    void OnRemoveLocalPF(wxCommandEvent &event) {
        int sel = m_localPFList->GetSelection();
        if (sel != wxNOT_FOUND) {
            m_localPFList->Delete(sel);
            m_localPFEntries.erase(m_localPFEntries.begin() + sel);
        }
        UpdatePreview();
    }

    void OnAddRemotePF(wxCommandEvent &event) {
        wxTextEntryDialog dlg(this, "Enter remote port forward (e.g., 8080:local:80):");
        if (dlg.ShowModal() == wxID_OK) {
            wxString entry = dlg.GetValue();
            if (!entry.empty()) {
                m_remotePFList->Append(entry);
                m_remotePFEntries.push_back(entry);
            }
        }
        UpdatePreview();
    }

    void OnRemoveRemotePF(wxCommandEvent &event) {
        int sel = m_remotePFList->GetSelection();
        if (sel != wxNOT_FOUND) {
            m_remotePFList->Delete(sel);
            m_remotePFEntries.erase(m_remotePFEntries.begin() + sel);
        }
        UpdatePreview();
    }

    void OnAddSOCKS(wxCommandEvent &event) {
        wxTextEntryDialog dlg(this, "Enter SOCKS forward (e.g., 1080):");
        if (dlg.ShowModal() == wxID_OK) {
            wxString entry = dlg.GetValue();
            if (!entry.empty()) {
                m_socksList->Append(entry);
                m_socksEntries.push_back(entry);
            }
        }
        UpdatePreview();
    }

    void OnRemoveSOCKS(wxCommandEvent &event) {
        int sel = m_socksList->GetSelection();
        if (sel != wxNOT_FOUND) {
            m_socksList->Delete(sel);
            m_socksEntries.erase(m_socksEntries.begin() + sel);
        }
        UpdatePreview();
    }

    void UpdatePreview() {
        wxString cmd = FLT_EXECUTABLE;
        cmd += " ";
        
        int connType = m_connTypeChoice->GetSelection();
        
        switch (connType) {
            case 0: // Local Shell
                cmd += "--local " + m_localShellCtrl->GetValue();
                break;
            case 1: // Telnet
                cmd += "--telnet " + m_telnetHostCtrl->GetValue() + ":" + wxString::Format("%d", m_telnetPortSpin->GetValue());
                if (m_telnetRawCheck->GetValue()) cmd += " --raw";
                if (m_telnetSSLCheck->GetValue()) cmd += " --ssl";
                break;
            case 2: // Serial
                cmd += "--serial " + m_serialPortCtrl->GetValue() + "," + m_serialBaudChoice->GetStringSelection();
                break;
            case 3: // SSH
                cmd += "--ssh " + m_sshUserCtrl->GetValue() + "@" + m_sshHostCtrl->GetValue() + ":" + wxString::Format("%d", m_sshPortSpin->GetValue());
                if (m_sshAuthChoice->GetSelection() == 1 && !m_sshKeyCtrl->GetValue().empty()) {
                    cmd += " -i " + m_sshKeyCtrl->GetValue();
                }
                if (m_sshX11Check->GetValue()) cmd += " -X";
                if (!m_sshCommandCtrl->GetValue().empty()) {
                    cmd += " -- " + m_sshCommandCtrl->GetValue();
                }
                break;
        }
        
        m_cmdPreview->SetValue(cmd);
    }

    SessionConfig GetCurrentConfig(const wxString &name) {
        SessionConfig cfg;
        cfg.name = name;
        cfg.connType = m_connTypeChoice->GetSelection();
        cfg.localShell = m_localShellCtrl->GetValue();
        
        cfg.telnetHost = m_telnetHostCtrl->GetValue();
        cfg.telnetPort = m_telnetPortSpin->GetValue();
        cfg.telnetTType = m_telnetTTypeCtrl->GetValue();
        cfg.telnetRaw = m_telnetRawCheck->GetValue();
        cfg.telnetSSL = m_telnetSSLCheck->GetValue();
        
        cfg.serialPort = m_serialPortCtrl->GetValue();
        int baudSel = m_serialBaudChoice->GetSelection();
        if (baudSel != wxNOT_FOUND) {
            cfg.serialBaud = wxAtoi(m_serialBaudChoice->GetString(baudSel));
        }
        
        cfg.sshUser = m_sshUserCtrl->GetValue();
        cfg.sshHost = m_sshHostCtrl->GetValue();
        cfg.sshPort = m_sshPortSpin->GetValue();
        cfg.sshAuthMethod = m_sshAuthChoice->GetSelection();
        cfg.sshKeyPath = m_sshKeyCtrl->GetValue();
        cfg.sshKnownHosts = m_sshKnownHostsCtrl->GetValue();
        cfg.sshX11 = m_sshX11Check->GetValue();
        cfg.sshCommand = m_sshCommandCtrl->GetValue();
        cfg.localPF = m_localPFEntries;
        cfg.remotePF = m_remotePFEntries;
        cfg.socks = m_socksEntries;
        
        cfg.webServerEnabled = m_webServerEnabledCheck->GetValue();
        cfg.webServerAddr = m_webServerAddrCtrl->GetValue();
        cfg.webServerPort = m_webServerPortSpin->GetValue();
        cfg.webRootDir = m_webRootDirCtrl->GetValue();
        
        return cfg;
    }

    void LoadSessions() {
        wxRegKey regKey(wxRegKey::HKCU, "Software\\FelixTerminal\\Sessions");
        if (!regKey.Exists()) return;
        
        wxString sessionName;
        long index = 0;
        if (regKey.GetFirstKey(sessionName, index)) {
            do {
                m_sessionList->InsertItem(m_sessionList->GetItemCount(), sessionName);
            } while (regKey.GetNextKey(sessionName, index));
        }
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
#undef HKEY_CURRENT_USER
        wxRegKey regKey(wxRegKey::HKCU, "Software\\FelixTerminal\\Sessions\\" + sessionName);
#define HKEY_CURRENT_USER ((HKEY)(LONG_PTR)(0x80000001))
        
        if (regKey.Exists()) {
            regKey.DeleteSelf();
            m_sessionList->DeleteItem(sel);
            wxMessageBox("Session deleted!", "Success", wxOK | wxICON_INFORMATION);
        }
    }

    void OnOpen(wxCommandEvent &event) {
        wxString cmdLine = m_cmdPreview->GetValue();
        wxExecute(cmdLine, wxEXEC_ASYNC);
        wxMessageBox("Felix Terminal launched!", "Success", wxOK | wxICON_INFORMATION);
    }

    void OnClear(wxCommandEvent &event) {
        // Reset all fields to defaults
        m_sessionNameCtrl->SetValue("New Session");
        m_connTypeChoice->SetSelection(0);
        
        m_telnetHostCtrl->SetValue("localhost");
        m_telnetPortSpin->SetValue(23);
        m_telnetTTypeCtrl->SetValue("xterm-256color");
        m_telnetRawCheck->SetValue(false);
        m_telnetSSLCheck->SetValue(false);
        
        m_serialPortCtrl->SetValue("COM1");
        m_serialBaudChoice->SetSelection(3);
        
        m_sshUserCtrl->SetValue("root");
        m_sshHostCtrl->SetValue("localhost");
        m_sshPortSpin->SetValue(22);
        m_sshAuthChoice->SetSelection(0);
        m_sshKeyCtrl->SetValue("");
        m_sshKnownHostsCtrl->SetValue("");
        m_sshX11Check->SetValue(true);
        m_sshCommandCtrl->SetValue("");
        
        m_localPFList->Clear();
        m_localPFEntries.clear();
        m_remotePFList->Clear();
        m_remotePFEntries.clear();
        m_socksList->Clear();
        m_socksEntries.clear();
        
        m_webServerEnabledCheck->SetValue(false);
        m_webServerAddrCtrl->SetValue("127.0.0.1");
        m_webServerPortSpin->SetValue(53716);
        m_webRootDirCtrl->SetValue("/");
        
        wxCommandEvent dummy;
        OnSSHAuthChange(dummy);
        UpdatePreview();
    }

    void OnClose(wxCommandEvent &event) {
        Close(true);
    }

    SessionConfig LoadSessionConfig(const wxString &name) {
        SessionConfig cfg;
        cfg.name = name;
        
#undef HKEY_CURRENT_USER
        wxRegKey regKey(wxRegKey::HKCU, "Software\\FelixTerminal\\Sessions\\" + name);
#define HKEY_CURRENT_USER ((HKEY)(LONG_PTR)(0x80000001))
        if (!regKey.Exists()) return cfg;  // Return empty config if key doesn't exist
        
        long lVal;
        
        if (regKey.QueryValue("connType", &lVal)) cfg.connType = lVal;
        
        wxString sVal;
        if (regKey.QueryValue("localShell", sVal)) cfg.localShell = sVal;
        
        if (regKey.QueryValue("telnetHost", sVal)) cfg.telnetHost = sVal;
        if (regKey.QueryValue("telnetPort", &lVal)) cfg.telnetPort = lVal;
        if (regKey.QueryValue("telnetTType", sVal)) cfg.telnetTType = sVal;
        if (regKey.QueryValue("telnetRaw", &lVal)) cfg.telnetRaw = (lVal != 0);
        if (regKey.QueryValue("telnetSSL", &lVal)) cfg.telnetSSL = (lVal != 0);
        
        if (regKey.QueryValue("serialPort", sVal)) cfg.serialPort = sVal;
        if (regKey.QueryValue("serialBaud", &lVal)) cfg.serialBaud = lVal;
        
        if (regKey.QueryValue("sshUser", sVal)) cfg.sshUser = sVal;
        if (regKey.QueryValue("sshHost", sVal)) cfg.sshHost = sVal;
        if (regKey.QueryValue("sshPort", &lVal)) cfg.sshPort = lVal;
        if (regKey.QueryValue("sshAuthMethod", &lVal)) cfg.sshAuthMethod = lVal;
        if (regKey.QueryValue("sshKeyPath", sVal)) cfg.sshKeyPath = sVal;
        if (regKey.QueryValue("sshKnownHosts", sVal)) cfg.sshKnownHosts = sVal;
        if (regKey.QueryValue("sshX11", &lVal)) cfg.sshX11 = (lVal != 0);
        if (regKey.QueryValue("sshCommand", sVal)) cfg.sshCommand = sVal;
        
        // Parse port forwards and SOCKS from pipe-delimited strings
        if (regKey.QueryValue("localPF", sVal)) {
            wxArrayString parts = wxSplit(sVal, '|');
            for (const auto &part : parts) {
                if (!part.empty()) cfg.localPF.push_back(part);
            }
        }
        if (regKey.QueryValue("remotePF", sVal)) {
            wxArrayString parts = wxSplit(sVal, '|');
            for (const auto &part : parts) {
                if (!part.empty()) cfg.remotePF.push_back(part);
            }
        }
        if (regKey.QueryValue("socks", sVal)) {
            wxArrayString parts = wxSplit(sVal, '|');
            for (const auto &part : parts) {
                if (!part.empty()) cfg.socks.push_back(part);
            }
        }
        
        if (regKey.QueryValue("webServerEnabled", &lVal)) cfg.webServerEnabled = (lVal != 0);
        if (regKey.QueryValue("webServerAddr", sVal)) cfg.webServerAddr = sVal;
        if (regKey.QueryValue("webServerPort", &lVal)) cfg.webServerPort = lVal;
        if (regKey.QueryValue("webRootDir", sVal)) cfg.webRootDir = sVal;
        
        return cfg;
    }

    void SaveSessionConfig(const SessionConfig &cfg) {
#undef HKEY_CURRENT_USER
        wxRegKey regKey(wxRegKey::HKCU, "Software\\FelixTerminal\\Sessions\\" + cfg.name);
#define HKEY_CURRENT_USER ((HKEY)(LONG_PTR)(0x80000001))
        if (!regKey.Exists()) regKey.Create();
        
        regKey.SetValue("connType", (long)cfg.connType);
        regKey.SetValue("localShell", cfg.localShell);
        
        regKey.SetValue("telnetHost", cfg.telnetHost);
        regKey.SetValue("telnetPort", (long)cfg.telnetPort);
        regKey.SetValue("telnetTType", cfg.telnetTType);
        regKey.SetValue("telnetRaw", cfg.telnetRaw ? 1L : 0L);
        regKey.SetValue("telnetSSL", cfg.telnetSSL ? 1L : 0L);
        
        regKey.SetValue("serialPort", cfg.serialPort);
        regKey.SetValue("serialBaud", (long)cfg.serialBaud);
        
        regKey.SetValue("sshUser", cfg.sshUser);
        regKey.SetValue("sshHost", cfg.sshHost);
        regKey.SetValue("sshPort", (long)cfg.sshPort);
        regKey.SetValue("sshAuthMethod", (long)cfg.sshAuthMethod);
        regKey.SetValue("sshKeyPath", cfg.sshKeyPath);
        regKey.SetValue("sshKnownHosts", cfg.sshKnownHosts);
        regKey.SetValue("sshX11", cfg.sshX11 ? 1L : 0L);
        regKey.SetValue("sshCommand", cfg.sshCommand);
        
        // Store port forwards and SOCKS as concatenated strings
        wxString localPFStr, remotePFStr, socksStr;
        for (size_t i = 0; i < cfg.localPF.size(); i++) {
            if (i > 0) localPFStr += "|";
            localPFStr += cfg.localPF[i];
        }
        for (size_t i = 0; i < cfg.remotePF.size(); i++) {
            if (i > 0) remotePFStr += "|";
            remotePFStr += cfg.remotePF[i];
        }
        for (size_t i = 0; i < cfg.socks.size(); i++) {
            if (i > 0) socksStr += "|";
            socksStr += cfg.socks[i];
        }
        
        if (!localPFStr.empty()) regKey.SetValue("localPF", localPFStr);
        if (!remotePFStr.empty()) regKey.SetValue("remotePF", remotePFStr);
        if (!socksStr.empty()) regKey.SetValue("socks", socksStr);
        
        regKey.SetValue("webServerEnabled", cfg.webServerEnabled ? 1L : 0L);
        regKey.SetValue("webServerAddr", cfg.webServerAddr);
        regKey.SetValue("webServerPort", (long)cfg.webServerPort);
        regKey.SetValue("webRootDir", cfg.webRootDir);
    }

    void LoadSessionToUI(const SessionConfig &cfg) {
        m_connTypeChoice->SetSelection(cfg.connType);
        m_localShellCtrl->SetValue(cfg.localShell);
        
        m_telnetHostCtrl->SetValue(cfg.telnetHost);
        m_telnetPortSpin->SetValue(cfg.telnetPort);
        m_telnetTTypeCtrl->SetValue(cfg.telnetTType);
        m_telnetRawCheck->SetValue(cfg.telnetRaw);
        m_telnetSSLCheck->SetValue(cfg.telnetSSL);
        
        m_serialPortCtrl->SetValue(cfg.serialPort);
        // Find and select the matching baud rate
        for (int i = 0; i < (int)m_serialBaudChoice->GetCount(); i++) {
            if (wxAtoi(m_serialBaudChoice->GetString(i)) == cfg.serialBaud) {
                m_serialBaudChoice->SetSelection(i);
                break;
            }
        }
        
        m_sshUserCtrl->SetValue(cfg.sshUser);
        m_sshHostCtrl->SetValue(cfg.sshHost);
        m_sshPortSpin->SetValue(cfg.sshPort);
        m_sshAuthChoice->SetSelection(cfg.sshAuthMethod);
        m_sshKeyCtrl->SetValue(cfg.sshKeyPath);
        m_sshKnownHostsCtrl->SetValue(cfg.sshKnownHosts);
        m_sshX11Check->SetValue(cfg.sshX11);
        m_sshCommandCtrl->SetValue(cfg.sshCommand);
        
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
        
        m_webServerEnabledCheck->SetValue(cfg.webServerEnabled);
        m_webServerAddrCtrl->SetValue(cfg.webServerAddr);
        m_webServerPortSpin->SetValue(cfg.webServerPort);
        m_webRootDirCtrl->SetValue(cfg.webRootDir);
        
        wxCommandEvent dummy;
        OnSSHAuthChange(dummy);
        OnConnTypeChange(dummy);
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
