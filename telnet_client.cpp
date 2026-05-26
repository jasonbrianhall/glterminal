#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <thread>
#include <chrono>
#include <termios.h>
#include <fstream>

class TelnetClient {
private:
    int socket_fd;
    std::string host;
    int port;
    bool suppress_local_echo;
    bool negotiation_complete;
    std::string last_sent;

public:
    TelnetClient(const std::string& h, int p) 
        : host(h), port(p), socket_fd(-1), suppress_local_echo(false), negotiation_complete(false), last_sent("") {}

    ~TelnetClient() {
        disconnect();
    }

    bool connect() {
        socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0) {
            std::cerr << "Error: Could not create socket\n";
            return false;
        }

        struct hostent* server = gethostbyname(host.c_str());
        if (server == nullptr) {
            std::cerr << "Error: Could not resolve hostname\n";
            close(socket_fd);
            return false;
        }

        struct sockaddr_in serv_addr;
        std::memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(port);
        std::memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);

        if (::connect(socket_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            std::cerr << "Error: Could not connect to server\n";
            close(socket_fd);
            return false;
        }

        std::cout << "Connected to " << host << ":" << port << "\n";
        return true;
    }

    void disconnect() {
        if (socket_fd >= 0) {
            close(socket_fd);
            socket_fd = -1;
        }
    }

    void receive_loop() {
        unsigned char buffer[4096];
        while (socket_fd >= 0) {
            int n = recv(socket_fd, buffer, sizeof(buffer), 0);
            if (n > 0) {
                process_telnet_data(buffer, n);
            } else if (n == 0) {
                std::cout << "\nConnection closed by server\n";
                socket_fd = -1;
                break;
            } else {
                socket_fd = -1;
                break;
            }
        }
    }

    void process_telnet_data(unsigned char* data, int len) {
        int i = 0;
        while (i < len) {
            unsigned char byte = data[i];

            // Send terminal type after negotiation starts
            if (!negotiation_complete && byte >= 32 && byte < 127) {
                negotiation_complete = true;
                send_terminal_type();
            }

            // Telnet IAC
            if (byte == 255 && i + 1 < len) {
                unsigned char cmd = data[i + 1];
                
                // Subnegotiation
                if (cmd == 250) {
                    i += 2;
                    unsigned char subopt = (i < len) ? data[i] : 0;
                    i++;
                    
                    // TTYPE subnegotiation
                    if (subopt == 24) {
                        unsigned char subtype = (i < len) ? data[i] : 0;
                        i++;
                        
                        // Skip to SE
                        while (i < len) {
                            if (data[i] == 255 && i + 1 < len && data[i + 1] == 240) {
                                i += 2;
                                break;
                            }
                            i++;
                        }
                        
                        // Send terminal type in response
                        if (subtype == 1) {  // SEND
                            send_terminal_type();
                        }
                        continue;
                    }
                    
                    // Skip other subnegotiations
                    while (i < len) {
                        if (data[i] == 255 && i + 1 < len && data[i + 1] == 240) {
                            i += 2;
                            break;
                        }
                        i++;
                    }
                    continue;
                }
                
                // DO/DONT/WILL/WONT
                if (cmd >= 251 && cmd <= 254 && i + 2 < len) {
                    handle_telnet_command(cmd, data[i + 2]);
                    i += 3;
                    continue;
                }
                
                i++;
                continue;
            }

            // OSC sequences: ESC ] ... (BEL or ESC \) - LOG THEM
            if (byte == 27 && i + 1 < len && data[i + 1] == ']') {
                std::ofstream log("data.log", std::ios::app);
                log << "OSC: ";
                i += 2;
                while (i < len) {
                    if (data[i] == 0x07) {
                        log << "[BEL]";
                        i++;
                        break;
                    }
                    if (data[i] == 27 && i + 1 < len && data[i + 1] == '\\') {
                        log << "[ST]";
                        i += 2;
                        break;
                    }
                    log << (char)data[i];
                    i++;
                }
                log << "\n";
                log.close();
                continue;
            }

            // CSI sequences: ESC [ ... (pass through for colors)
            if (byte == 27 && i + 1 < len && data[i + 1] == '[') {
                std::string csi;
                csi += (char)byte;
                i++;
                csi += (char)data[i];
                i++;
                while (i < len && !std::isalpha(data[i])) {
                    csi += (char)data[i];
                    i++;
                }
                if (i < len) {
                    csi += (char)data[i];
                    i++;
                }
                // Log and print
                std::ofstream log("data.log", std::ios::app);
                log << "CSI: " << csi << "\n";
                log.close();
                std::cout << csi << std::flush;
                continue;
            }

            // Carriage return
            if (byte == '\r') {
                i++;
                if (i < len && (data[i] == '\n' || data[i] == '\0')) {
                    i++;
                }
                std::cout << '\n' << std::flush;
                continue;
            }

            // Regular characters
            if (byte >= 32 && byte < 127) {
                // Skip printing if this matches what we just sent (echo suppression)
                if (!last_sent.empty() && last_sent[0] == byte) {
                    last_sent = last_sent.substr(1);
                    i++;
                } else {
                    std::cout << (char)byte << std::flush;
                    i++;
                }
            } else {
                i++;
            }
        }
    }

    void handle_telnet_command(unsigned char cmd, unsigned char opt) {
        unsigned char response[3] = {255, 0, opt};
        const int ECHO_OPTION = 1;
        const int TTYPE_OPTION = 24;  // Terminal Type

        if (cmd == 253) {  // DO
            if (opt == ECHO_OPTION) {
                response[1] = 251;  // WILL
                suppress_local_echo = false;
            } else if (opt == TTYPE_OPTION) {
                response[1] = 251;  // WILL - we support terminal type
            } else {
                response[1] = 252;  // WONT
            }
        }
        else if (cmd == 254) {  // DONT
            if (opt == ECHO_OPTION) {
                response[1] = 252;  // WONT
                suppress_local_echo = true;
            } else {
                response[1] = 252;  // WONT
            }
        }
        else if (cmd == 251) {  // WILL
            if (opt == TTYPE_OPTION) {
                response[1] = 253;  // DO - we want to know terminal type
            } else {
                response[1] = 254;  // DONT
            }
        }
        else if (cmd == 252) {  // WONT
            response[1] = 254;  // DONT
        }

        send(socket_fd, response, 3, 0);
    }

    void send_terminal_type() {
        // IAC SB TTYPE IS "xterm-256color" IAC SE
        unsigned char ttype[] = {255, 250, 24, 0, 'x', 't', 'e', 'r', 'm', '-', '2', '5', '6', 'c', 'o', 'l', 'o', 'r', 255, 240};
        send(socket_fd, ttype, sizeof(ttype), 0);
    }

    void send_message(const std::string& msg) {
        if (socket_fd >= 0) {
            send(socket_fd, msg.c_str(), msg.length(), 0);
            last_sent = msg;
        }
    }

    bool is_connected() const {
        return socket_fd >= 0;
    }

    bool should_suppress_echo() const {
        return suppress_local_echo;
    }

    void reset_echo() {
        suppress_local_echo = false;
    }
};

void input_thread_func(TelnetClient& client) {
    while (client.is_connected()) {
        std::string line;
        
        if (client.should_suppress_echo()) {
            termios oldt, newt;
            tcgetattr(STDIN_FILENO, &oldt);
            newt = oldt;
            newt.c_lflag &= ~ECHO;
            tcsetattr(STDIN_FILENO, TCSANOW, &newt);
            
            std::getline(std::cin, line);
            
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            std::cout << '\n' << std::flush;
            
            client.send_message(line + "\n");
            client.reset_echo();
        } else {
            if (!std::getline(std::cin, line)) break;
            client.send_message(line + "\n");
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <hostname> <port>\n";
        return 1;
    }

    std::string host = argv[1];
    int port = std::stoi(argv[2]);

    TelnetClient client(host, port);

    if (!client.connect()) {
        return 1;
    }

    std::thread recv_thread(&TelnetClient::receive_loop, &client);
    recv_thread.detach();

    input_thread_func(client);

    client.disconnect();
    return 0;
}
