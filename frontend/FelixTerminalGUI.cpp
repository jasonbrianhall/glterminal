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
        wxScrolledWindow *scrolled = new wxScrolledWindow(panel, wxID_ANY);
        scrolled->SetScrollRate(5, 5);
        wxBoxSizer *scrolledSizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer *typeSizer = new wxBoxSizer(wxHORIZONTAL);
        typeSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Connection Type:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_connTypeChoice = new wxChoice(scrolled, wxID_ANY);
        m_connTypeChoice->Append("Local Shell");
        m_connTypeChoice->Append("Telnet");
        m_connTypeChoice->Append("Serial");
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

        // Telnet
        wxStaticBoxSizer *telnetBox = new wxStaticBoxSizer(wxVERTICAL, scrolled, "Telnet");
        
        wxBoxSizer *telnetHostPortSizer = new wxBoxSizer(wxHORIZONTAL);
        telnetHostPortSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Host:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_telnetHostCtrl = new wxTextCtrl(scrolled, wxID_ANY, "localhost");
        m_telnetHostCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnControlChange, this);
        telnetHostPortSizer->Add(m_telnetHostCtrl, 1, wxRIGHT, 16);
        
        telnetHostPortSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_telnetPortSpin = new wxSpinCtrl(scrolled, wxID_ANY, "23", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535);
        m_telnetPortSpin->Bind(wxEVT_SPINCTRL, &FelixTerminalFrame::OnControlChange, this);
        telnetHostPortSizer->Add(m_telnetPortSpin, 0);
        telnetBox->Add(telnetHostPortSizer, 0, wxEXPAND | wxALL, 4);
        
        wxBoxSizer *telnetTTypeSizer = new wxBoxSizer(wxHORIZONTAL);
        telnetTTypeSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Terminal Type:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_telnetTTypeCtrl = new wxTextCtrl(scrolled, wxID_ANY, "xterm-256color");
        m_telnetTTypeCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnControlChange, this);
        telnetTTypeSizer->Add(m_telnetTTypeCtrl, 1, wxEXPAND);
        telnetBox->Add(telnetTTypeSizer, 0, wxEXPAND | wxALL, 4);
        
        m_telnetRawCheck = new wxCheckBox(scrolled, wxID_ANY, "Raw Mode");
        m_telnetRawCheck->Bind(wxEVT_CHECKBOX, &FelixTerminalFrame::OnControlChange, this);
        telnetBox->Add(m_telnetRawCheck, 0, wxALL, 4);
        
        m_telnetSSLCheck = new wxCheckBox(scrolled, wxID_ANY, "Use SSL/TLS");
        m_telnetSSLCheck->Bind(wxEVT_CHECKBOX, &FelixTerminalFrame::OnControlChange, this);
        telnetBox->Add(m_telnetSSLCheck, 0, wxALL, 4);
        
        scrolledSizer->Add(telnetBox, 0, wxEXPAND | wxALL, 8);

        // Serial
        wxStaticBoxSizer *serialBox = new wxStaticBoxSizer(wxVERTICAL, scrolled, "Serial");
        
        wxBoxSizer *serialPortBaudSizer = new wxBoxSizer(wxHORIZONTAL);
        serialPortBaudSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_serialPortCtrl = new wxTextCtrl(scrolled, wxID_ANY, "");
        m_serialPortCtrl->SetToolTip("e.g., /dev/ttyUSB0 or COM3");
        m_serialPortCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnControlChange, this);
        serialPortBaudSizer->Add(m_serialPortCtrl, 1, wxRIGHT, 16);
        
        serialPortBaudSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Baud:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_serialBaudChoice = new wxChoice(scrolled, wxID_ANY);
        m_serialBaudChoice->Append("300");
        m_serialBaudChoice->Append("1200");
        m_serialBaudChoice->Append("2400");
        m_serialBaudChoice->Append("4800");
        m_serialBaudChoice->Append("9600");
        m_serialBaudChoice->Append("14400");
        m_serialBaudChoice->Append("19200");
        m_serialBaudChoice->Append("28800");
        m_serialBaudChoice->Append("38400");
        m_serialBaudChoice->Append("57600");
        m_serialBaudChoice->Append("115200");
        m_serialBaudChoice->Append("230400");
        m_serialBaudChoice->Append("460800");
        m_serialBaudChoice->SetSelection(4);  // Default to 9600
        m_serialBaudChoice->Bind(wxEVT_CHOICE, &FelixTerminalFrame::OnControlChange, this);
        serialPortBaudSizer->Add(m_serialBaudChoice, 0);
        serialBox->Add(serialPortBaudSizer, 0, wxEXPAND | wxALL, 4);
        
        scrolledSizer->Add(serialBox, 0, wxEXPAND | wxALL, 8);

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

        wxBoxSizer *authSizer = new wxBoxSizer(wxHORIZONTAL);
        authSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Auth Method:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshAuthChoice = new wxChoice(scrolled, wxID_ANY);
        m_sshAuthChoice->Append("SSH Agent");
        m_sshAuthChoice->Append("Specific Private Key");
        m_sshAuthChoice->SetSelection(0);
        m_sshAuthChoice->Bind(wxEVT_CHOICE, &FelixTerminalFrame::OnSSHAuthChange, this);
        authSizer->Add(m_sshAuthChoice, 1, wxEXPAND);
        sshBox->Add(authSizer, 0, wxEXPAND | wxALL, 4);

        wxBoxSizer *keyPathSizer = new wxBoxSizer(wxHORIZONTAL);
        keyPathSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Private Key:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshKeyCtrl = new wxTextCtrl(scrolled, wxID_ANY, "");
        m_sshKeyCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnControlChange, this);
        keyPathSizer->Add(m_sshKeyCtrl, 1);
        m_browseSshKeyBtn = new wxButton(scrolled, wxID_ANY, "Browse");
        m_browseSshKeyBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnBrowseSshKey, this);
        keyPathSizer->Add(m_browseSshKeyBtn, 0, wxLEFT, 4);
        sshBox->Add(keyPathSizer, 0, wxEXPAND | wxALL, 4);
        
        wxStaticText *agentNote = new wxStaticText(scrolled, wxID_ANY, 
            "Note: SSH Agent uses keys from ssh-agent, CAC, or ~/.ssh/config");
        agentNote->SetFont(agentNote->GetFont().MakeItalic());
        sshBox->Add(agentNote, 0, wxALL, 4);

        wxBoxSizer *knownHostsSizer = new wxBoxSizer(wxHORIZONTAL);
        knownHostsSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Known Hosts:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshKnownHostsCtrl = new wxTextCtrl(scrolled, wxID_ANY, "");
        m_sshKnownHostsCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnControlChange, this);
        knownHostsSizer->Add(m_sshKnownHostsCtrl, 1, wxEXPAND);
        sshBox->Add(knownHostsSizer, 0, wxEXPAND | wxALL, 4);

        m_sshX11Check = new wxCheckBox(scrolled, wxID_ANY, "Enable X11 Forwarding");
        m_sshX11Check->SetValue(true);
        m_sshX11Check->Bind(wxEVT_CHECKBOX, &FelixTerminalFrame::OnControlChange, this);
        sshBox->Add(m_sshX11Check, 0, wxALL, 4);

        // SSH Command Execution
        wxBoxSizer *sshCmdSizer = new wxBoxSizer(wxVERTICAL);
        sshCmdSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Execute Command (-c):"), 0, wxALL, 4);
        m_sshCommandCtrl = new wxTextCtrl(scrolled, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 50), 
                                          wxTE_MULTILINE | wxTE_WORDWRAP);
        m_sshCommandCtrl->SetToolTip("Command to execute on remote server (optional)");
        m_sshCommandCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnControlChange, this);
        sshCmdSizer->Add(m_sshCommandCtrl, 1, wxEXPAND | wxALL, 4);
        sshBox->Add(sshCmdSizer, 1, wxEXPAND | wxALL, 4);

        scrolledSizer->Add(sshBox, 0, wxEXPAND | wxALL, 8);

        scrolled->SetSizer(scrolledSizer);
        sizer->Add(scrolled, 1, wxEXPAND);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Connection");
    }

    void CreatePortForwardPanel(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

        wxStaticBoxSizer *localPFBox = new wxStaticBoxSizer(wxVERTICAL, panel, "Local Port Forward (-L)");
        m_localPFList = new wxListBox(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 80));
        localPFBox->Add(m_localPFList, 1, wxEXPAND | wxALL, 4);
        wxBoxSizer *localPFBtnSizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton *addLocalPFBtn = new wxButton(panel, wxID_ANY, "Add");
        wxButton *delLocalPFBtn = new wxButton(panel, wxID_ANY, "Remove");
        addLocalPFBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnAddLocalPF, this);
        delLocalPFBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnRemoveLocalPF, this);
        localPFBtnSizer->Add(addLocalPFBtn, 0, wxRIGHT, 4);
        localPFBtnSizer->Add(delLocalPFBtn, 0);
        localPFBox->Add(localPFBtnSizer, 0, wxEXPAND | wxALL, 4);
        sizer->Add(localPFBox, 1, wxEXPAND | wxALL, 8);

        wxStaticBoxSizer *remotePFBox = new wxStaticBoxSizer(wxVERTICAL, panel, "Remote Port Forward (-R)");
        m_remotePFList = new wxListBox(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 80));
        remotePFBox->Add(m_remotePFList, 1, wxEXPAND | wxALL, 4);
        wxBoxSizer *remotePFBtnSizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton *addRemotePFBtn = new wxButton(panel, wxID_ANY, "Add");
        wxButton *delRemotePFBtn = new wxButton(panel, wxID_ANY, "Remove");
        addRemotePFBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnAddRemotePF, this);
        delRemotePFBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnRemoveRemotePF, this);
        remotePFBtnSizer->Add(addRemotePFBtn, 0, wxRIGHT, 4);
        remotePFBtnSizer->Add(delRemotePFBtn, 0);
        remotePFBox->Add(remotePFBtnSizer, 0, wxEXPAND | wxALL, 4);
        sizer->Add(remotePFBox, 1, wxEXPAND | wxALL, 8);

        wxStaticBoxSizer *socksBox = new wxStaticBoxSizer(wxVERTICAL, panel, "SOCKS5 Dynamic Forward (-D)");
        m_socksList = new wxListBox(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 80));
        socksBox->Add(m_socksList, 1, wxEXPAND | wxALL, 4);
        wxBoxSizer *socksBtnSizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton *addSocksBtn = new wxButton(panel, wxID_ANY, "Add");
        wxButton *delSocksBtn = new wxButton(panel, wxID_ANY, "Remove");
        addSocksBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnAddSocks, this);
        delSocksBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnRemoveSocks, this);
        socksBtnSizer->Add(addSocksBtn, 0, wxRIGHT, 4);
        socksBtnSizer->Add(delSocksBtn, 0);
        socksBox->Add(socksBtnSizer, 0, wxEXPAND | wxALL, 4);
        sizer->Add(socksBox, 1, wxEXPAND | wxALL, 8);

        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Port Forward");
    }

    void CreateWebServerPanel(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        wxScrolledWindow *scrolled = new wxScrolledWindow(panel, wxID_ANY);
        scrolled->SetScrollRate(5, 5);
        wxBoxSizer *scrolledSizer = new wxBoxSizer(wxVERTICAL);

        // Enable Web Server
        m_webServerEnabledCheck = new wxCheckBox(scrolled, wxID_ANY, "Enable Web Server");
        m_webServerEnabledCheck->Bind(wxEVT_CHECKBOX, &FelixTerminalFrame::OnControlChange, this);
        scrolledSizer->Add(m_webServerEnabledCheck, 0, wxALL, 8);

        scrolledSizer->Add(new wxStaticLine(scrolled), 0, wxEXPAND | wxALL, 8);

        // Web Server Settings
        wxStaticBoxSizer *webBox = new wxStaticBoxSizer(wxVERTICAL, scrolled, "Web Server Configuration");

        wxBoxSizer *addrSizer = new wxBoxSizer(wxHORIZONTAL);
        addrSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Listen Address:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_webServerAddrCtrl = new wxTextCtrl(scrolled, wxID_ANY, "");
        m_webServerAddrCtrl->SetToolTip("IP address to listen on (e.g., 127.0.0.1, 0.0.0.0). Leave empty for default.");
        m_webServerAddrCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnControlChange, this);
        addrSizer->Add(m_webServerAddrCtrl, 1, wxEXPAND | wxRIGHT, 16);

        addrSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_webServerPortSpin = new wxSpinCtrl(scrolled, wxID_ANY, "53716", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535);
        m_webServerPortSpin->SetToolTip("Only used if Listen Address is specified");
        m_webServerPortSpin->Bind(wxEVT_SPINCTRL, &FelixTerminalFrame::OnControlChange, this);
        addrSizer->Add(m_webServerPortSpin, 0);
        webBox->Add(addrSizer, 0, wxEXPAND | wxALL, 4);

        wxBoxSizer *rootDirSizer = new wxBoxSizer(wxHORIZONTAL);
        rootDirSizer->Add(new wxStaticText(scrolled, wxID_ANY, "Root Directory:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_webRootDirCtrl = new wxTextCtrl(scrolled, wxID_ANY, "");
        m_webRootDirCtrl->SetToolTip("Directory to serve. Leave empty for default (/).");
        m_webRootDirCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnControlChange, this);
        rootDirSizer->Add(m_webRootDirCtrl, 1);
        m_browseWebRootBtn = new wxButton(scrolled, wxID_ANY, "Browse");
        m_browseWebRootBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnBrowseWebRoot, this);
        rootDirSizer->Add(m_browseWebRootBtn, 0, wxLEFT, 4);
        webBox->Add(rootDirSizer, 0, wxEXPAND | wxALL, 4);

        scrolledSizer->Add(webBox, 0, wxEXPAND | wxALL, 8);

        scrolledSizer->AddStretchSpacer();

        scrolled->SetSizer(scrolledSizer);
        sizer->Add(scrolled, 1, wxEXPAND);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Web Server");
    }

    void OnConnTypeChange(wxCommandEvent &event) {
        int sel = m_connTypeChoice->GetSelection();
        UpdatePreview();
    }

    void OnSSHAuthChange(wxCommandEvent &event) {
        int authType = m_sshAuthChoice->GetSelection();
        m_sshKeyCtrl->Enable(authType == 1);  // Only enable for "Specific Private Key"
        m_browseSshKeyBtn->Enable(authType == 1);
        UpdatePreview();
    }

    void OnBrowseSshKey(wxCommandEvent &event) {
        wxFileDialog dlg(this, "Select SSH Private Key", "", "", 
                        "All files (*.*)|*.*",
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            m_sshKeyCtrl->SetValue(dlg.GetPath());
            OnControlChange(event);
        }
    }

    void OnBrowseWebRoot(wxCommandEvent &event) {
        wxDirDialog dlg(this, "Select Root Directory for Web Server");
        if (dlg.ShowModal() == wxID_OK) {
            m_webRootDirCtrl->SetValue(dlg.GetPath());
            OnControlChange(event);
        }
    }

    void OnControlChange(wxCommandEvent &event) {
        UpdatePreview();
    }

    void UpdatePreview() {
        wxString cmd = FLT_EXECUTABLE;
        int connType = m_connTypeChoice->GetSelection();

        if (connType == 0) {  // Local Shell
            wxString shell = m_localShellCtrl->GetValue();
            if (!shell.empty()) {
                cmd += " " + shell;
            }

            // Local Shell Web Server
            if (m_webServerEnabledCheck->GetValue()) {
                wxString webCmd = BuildWebServerCommand();
                if (!webCmd.empty()) {
                    cmd += " " + webCmd;
                }
            }
        } else if (connType == 1) {  // Telnet
            wxString host = m_telnetHostCtrl->GetValue();
            int port = m_telnetPortSpin->GetValue();
            if (!host.empty()) {
                cmd += " --telnet " + host;
                if (port != 23) {
                    cmd += wxString::Format(":%d", port);
                }
            }
            wxString ttype = m_telnetTTypeCtrl->GetValue();
            if (!ttype.empty() && ttype != "xterm-256color") {
                cmd += " --telnet-ttype " + ttype;
            }
            if (m_telnetRawCheck->GetValue()) {
                cmd += " --raw";
            }
            if (m_telnetSSLCheck->GetValue()) {
                cmd += " --ssl";
            }

            // Telnet Web Server
            if (m_webServerEnabledCheck->GetValue()) {
                wxString webCmd = BuildWebServerCommand();
                if (!webCmd.empty()) {
                    cmd += " " + webCmd;
                }
            }
        } else if (connType == 2) {  // Serial
            cmd += " --serial";
            wxString port = m_serialPortCtrl->GetValue();
            if (!port.empty()) {
                cmd += " " + port;
            }
            int baudSel = m_serialBaudChoice->GetSelection();
            if (baudSel != wxNOT_FOUND) {
                wxString baudStr = m_serialBaudChoice->GetString(baudSel);
                int baud = wxAtoi(baudStr);
                if (baud != 9600) {
                    cmd += wxString::Format(" --serial-baud %d", baud);
                }
            }

            // Serial Web Server
            if (m_webServerEnabledCheck->GetValue()) {
                wxString webCmd = BuildWebServerCommand();
                if (!webCmd.empty()) {
                    cmd += " " + webCmd;
                }
            }
        } else if (connType == 3) {  // SSH
            wxString user = m_sshUserCtrl->GetValue();
            wxString host = m_sshHostCtrl->GetValue();
            int port = m_sshPortSpin->GetValue();
            
            if (!host.empty()) {
                cmd += " --ssh ";
                if (!user.empty()) {
                    cmd += user + "@";
                }
                cmd += host;
                if (port != 22) {
                    cmd += wxString::Format(":%d", port);
                }
            }

            int authMethod = m_sshAuthChoice->GetSelection();
            if (authMethod == 1) {  // Specific Private Key
                wxString keyPath = m_sshKeyCtrl->GetValue();
                if (!keyPath.empty()) {
                    cmd += " -i " + keyPath;
                }
            }

            wxString knownHosts = m_sshKnownHostsCtrl->GetValue();
            if (!knownHosts.empty()) {
                cmd += " --ssh-known-hosts " + knownHosts;
            }

            if (!m_sshX11Check->GetValue()) {
                cmd += " --no-x11";
            }

            for (const auto &pf : m_localPFEntries) {
                cmd += " -L " + pf;
            }
            for (const auto &pf : m_remotePFEntries) {
                cmd += " -R " + pf;
            }
            for (const auto &s : m_socksEntries) {
                cmd += " -D " + s;
            }

            // SSH Command Execution
            wxString sshCmd = m_sshCommandCtrl->GetValue();
            if (!sshCmd.empty()) {
                // Escape single quotes in command
                sshCmd.Replace("'", "'\\''");
                cmd += " -c '" + sshCmd + "'";
            }

            // SSH Web Server
            if (m_webServerEnabledCheck->GetValue()) {
                wxString webCmd = BuildWebServerCommand(false);  // Don't include --web-root for SSH
                if (!webCmd.empty()) {
                    cmd += " " + webCmd;
                }
            }
        }

        m_cmdPreview->SetValue(cmd);
    }

    wxString BuildWebServerCommand(bool includeWebRoot = true) {
        wxString addr = m_webServerAddrCtrl->GetValue();
        wxString rootDir = m_webRootDirCtrl->GetValue();

        wxString webCmd = "--webserver";
        
        // Only add address:port if address is not empty
        if (!addr.empty()) {
            int port = m_webServerPortSpin->GetValue();
            webCmd += wxString::Format(" %s:%d", addr, port);
        }

        if (includeWebRoot && !rootDir.empty() && rootDir != "/") {
            webCmd += " --web-root " + rootDir;
        }

        return webCmd;
    }

    void OnAddLocalPF(wxCommandEvent &event) {
        wxTextEntryDialog dlg(this, "Enter local port forward (local_port:remote_host:remote_port):", "Add Local Port Forward");
        if (dlg.ShowModal() == wxID_OK && !dlg.GetValue().empty()) {
            m_localPFEntries.push_back(dlg.GetValue());
            m_localPFList->Append(dlg.GetValue());
            UpdatePreview();
        }
    }

    void OnRemoveLocalPF(wxCommandEvent &event) {
        int sel = m_localPFList->GetSelection();
        if (sel != wxNOT_FOUND) {
            m_localPFEntries.erase(m_localPFEntries.begin() + sel);
            m_localPFList->Delete(sel);
            UpdatePreview();
        }
    }

    void OnAddRemotePF(wxCommandEvent &event) {
        wxTextEntryDialog dlg(this, "Enter remote port forward (remote_port:local_host:local_port):", "Add Remote Port Forward");
        if (dlg.ShowModal() == wxID_OK && !dlg.GetValue().empty()) {
            m_remotePFEntries.push_back(dlg.GetValue());
            m_remotePFList->Append(dlg.GetValue());
            UpdatePreview();
        }
    }

    void OnRemoveRemotePF(wxCommandEvent &event) {
        int sel = m_remotePFList->GetSelection();
        if (sel != wxNOT_FOUND) {
            m_remotePFEntries.erase(m_remotePFEntries.begin() + sel);
            m_remotePFList->Delete(sel);
            UpdatePreview();
        }
    }

    void OnAddSocks(wxCommandEvent &event) {
        wxTextEntryDialog dlg(this, "Enter SOCKS5 port (local_port):", "Add SOCKS5 Forward");
        if (dlg.ShowModal() == wxID_OK && !dlg.GetValue().empty()) {
            m_socksEntries.push_back(dlg.GetValue());
            m_socksList->Append(dlg.GetValue());
            UpdatePreview();
        }
    }

    void OnRemoveSocks(wxCommandEvent &event) {
        int sel = m_socksList->GetSelection();
        if (sel != wxNOT_FOUND) {
            m_socksEntries.erase(m_socksEntries.begin() + sel);
            m_socksList->Delete(sel);
            UpdatePreview();
        }
    }

    void CreateConfigDir() {
        wxString configDir = wxStandardPaths::Get().GetUserConfigDir() + wxFILE_SEP_PATH + "felixterminal";
        if (!wxDirExists(configDir)) {
            wxMkdir(configDir);
        }
    }

    void LoadSessions() {
        wxString configDir = wxStandardPaths::Get().GetUserConfigDir() + wxFILE_SEP_PATH + "felixterminal";
        wxDir dir(configDir);
        wxString filename;
        bool cont = dir.GetFirst(&filename, "*.cfg", wxDIR_FILES);
        
        while (cont) {
            wxString sessionName = filename.BeforeLast('.');
            m_sessionList->InsertItem(m_sessionList->GetItemCount(), sessionName);
            cont = dir.GetNext(&filename);
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

    void OnClear(wxCommandEvent &event) {
        // Reset all fields to defaults
        m_sessionNameCtrl->SetValue("New Session");
        m_connTypeChoice->SetSelection(0);
        
        m_localShellCtrl->SetValue("");
        
        m_telnetHostCtrl->SetValue("localhost");
        m_telnetPortSpin->SetValue(23);
        m_telnetTTypeCtrl->SetValue("xterm-256color");
        m_telnetRawCheck->SetValue(false);
        m_telnetSSLCheck->SetValue(false);
        
        m_serialPortCtrl->SetValue("");
        m_serialBaudChoice->SetSelection(4);  // Default to 9600
        
        m_sshUserCtrl->SetValue("");
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
        m_webServerAddrCtrl->SetValue("");
        m_webServerPortSpin->SetValue(53716);
        m_webRootDirCtrl->SetValue("");
        
        wxCommandEvent dummy;
        OnSSHAuthChange(dummy);
        OnConnTypeChange(dummy);
        UpdatePreview();
    }

    void OnClose(wxCommandEvent &event) {
        Close();
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
                else if (key == "telnetHost") cfg.telnetHost = val;
                else if (key == "telnetPort") cfg.telnetPort = wxAtoi(val);
                else if (key == "telnetTType") cfg.telnetTType = val;
                else if (key == "telnetRaw") cfg.telnetRaw = (val == "1");
                else if (key == "telnetSSL") cfg.telnetSSL = (val == "1");
                else if (key == "serialPort") cfg.serialPort = val;
                else if (key == "serialBaud") cfg.serialBaud = wxAtoi(val);
                else if (key == "sshUser") cfg.sshUser = val;
                else if (key == "sshHost") cfg.sshHost = val;
                else if (key == "sshPort") cfg.sshPort = wxAtoi(val);
                else if (key == "sshAuthMethod") cfg.sshAuthMethod = wxAtoi(val);
                else if (key == "sshKeyPath") cfg.sshKeyPath = val;
                else if (key == "sshKnownHosts") cfg.sshKnownHosts = val;
                else if (key == "sshX11") cfg.sshX11 = (val == "1");
                else if (key == "sshCommand") cfg.sshCommand = val;
                else if (key.StartsWith("localPF_")) cfg.localPF.push_back(val);
                else if (key.StartsWith("remotePF_")) cfg.remotePF.push_back(val);
                else if (key.StartsWith("socks_")) cfg.socks.push_back(val);
                else if (key == "webServerEnabled") cfg.webServerEnabled = (val == "1");
                else if (key == "webServerAddr") cfg.webServerAddr = val;
                else if (key == "webServerPort") cfg.webServerPort = wxAtoi(val);
                else if (key == "webRootDir") cfg.webRootDir = val;
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
        
        file.AddLine("telnetHost=" + cfg.telnetHost);
        file.AddLine(wxString::Format("telnetPort=%d", cfg.telnetPort));
        file.AddLine("telnetTType=" + cfg.telnetTType);
        file.AddLine(wxString::Format("telnetRaw=%d", cfg.telnetRaw ? 1 : 0));
        file.AddLine(wxString::Format("telnetSSL=%d", cfg.telnetSSL ? 1 : 0));
        
        file.AddLine("serialPort=" + cfg.serialPort);
        file.AddLine(wxString::Format("serialBaud=%d", cfg.serialBaud));
        
        file.AddLine("sshUser=" + cfg.sshUser);
        file.AddLine("sshHost=" + cfg.sshHost);
        file.AddLine(wxString::Format("sshPort=%d", cfg.sshPort));
        file.AddLine(wxString::Format("sshAuthMethod=%d", cfg.sshAuthMethod));
        file.AddLine("sshKeyPath=" + cfg.sshKeyPath);
        file.AddLine("sshKnownHosts=" + cfg.sshKnownHosts);
        file.AddLine(wxString::Format("sshX11=%d", cfg.sshX11 ? 1 : 0));
        file.AddLine("sshCommand=" + cfg.sshCommand);
        
        for (size_t i = 0; i < cfg.localPF.size(); i++) {
            file.AddLine(wxString::Format("localPF_%zu=%s", i, cfg.localPF[i]));
        }
        for (size_t i = 0; i < cfg.remotePF.size(); i++) {
            file.AddLine(wxString::Format("remotePF_%zu=%s", i, cfg.remotePF[i]));
        }
        for (size_t i = 0; i < cfg.socks.size(); i++) {
            file.AddLine(wxString::Format("socks_%zu=%s", i, cfg.socks[i]));
        }
        
        file.AddLine(wxString::Format("webServerEnabled=%d", cfg.webServerEnabled ? 1 : 0));
        file.AddLine("webServerAddr=" + cfg.webServerAddr);
        file.AddLine(wxString::Format("webServerPort=%d", cfg.webServerPort));
        file.AddLine("webRootDir=" + cfg.webRootDir);
        
        file.Write();
        file.Close();
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
