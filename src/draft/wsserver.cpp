/*
 * 
 */
#include <libwebsockets.h>
#include <signal.h>
#include <string>
#include <unordered_map>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;

struct Client {
    struct lws *wsi;
    string id;
};

static unordered_map<string, Client> clients;
static volatile sig_atomic_t do_term = 0;

static int callback_ws(struct lws *wsi, enum lws_callback_reasons reason, 
    void *user, void *in, size_t len)
{
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
            cout << "Connection established" << "\n";
            char path[1024];
            memset(path, 0, sizeof(path));
            int cnt = lws_hdr_copy(wsi, path, sizeof(path), WSI_TOKEN_GET_URI);
            if (cnt > 0 && strlen(path) > 1) {
                string client_id = string(path + 1); // skip leading '/';
                clients[client_id] = { wsi, client_id };
                cout << "Client " << client_id << " connected\n";
            }
        }
        case LWS_CALLBACK_RECEIVE: {
            string data((char *)in, len);
            cout << "Received: " << data << "\n";
            try {
                json message = json::parse(data);
                string dest_id = message["id"];
                auto dest_it = clients.find(dest_id);
                if (dest_it != clients.end()) {
                    // Replace id with sender's id
                    for (auto &pair : clients) {
                        if (pair.second.wsi == wsi) {
                            message["id"] = pair.second.id;
                            break;
                        }
                    }
                    string out = message.dump();
                    unsigned char *buf = (unsigned char *)malloc(LWS_PRE + out.size());
                    memcpy(buf + LWS_PRE, out.c_str(), out.size());
                    lws_write(dest_it->second.wsi, buf + LWS_PRE, out.size(), LWS_WRITE_TEXT);
                    free(buf);
                    cout << "Forwarded to " << dest_id << "\n";
                } else {
                    cout << "Client " << dest_id << " not found\n";
                }
            } catch (const exception &e) {
                cerr << "JSON parse error: " << e.what() << "\n";
            }
            break;
        }

        case LWS_CALLBACK_CLOSED: {
            for (auto it = clients.begin(); it != clients.end(); ) {
                if (it->second.wsi == wsi) {
                    cout << "Client " << it->second.id << " disconnected\n";
                    it = clients.erase(it);
                } else {
                    ++it;
                }
            }
            break;
        }

        default:
            break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    { "ws-protocol", callback_ws, 0, 4096 },
    { NULL, NULL, 0, 0 }
};

void sig_handler_term(int sig) {
    do_term = 1;
}

int main(int argc, char **argv) {
    struct lws_context_creation_info info;
    struct lws_context *context;
    memset(&info, 0, sizeof(info));

    string endpoint = (argc > 1) ? argv[1] : "127.0.0.1:8000";
    string ssl_cert = (argc > 2) ? argv[2] : "";

    string host;
    int port;
    size_t pos = endpoint.find(':');
    if (pos != string::npos) {
        host = endpoint.substr(0, pos);
        port = stoi(endpoint.substr(pos + 1));
    } else {
        host = "127.0.0.1";
        port = stoi(endpoint);
    }

    info.port = port;
    info.protocols = protocols;
    if (!ssl_cert.empty()) {
        info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        info.ssl_cert_filepath = ssl_cert.c_str();
        info.ssl_private_key_filepath = ssl_cert.c_str();
    }

    signal(SIGINT, sig_handler_term);

    context = lws_create_context(&info);
    if (!context) {
        cerr << "Failed to create context\n";
        return 1;
    }

    cout << "Listening on " << endpoint << "\n";

    while (!do_term) {
        lws_service(context, 1000);
    }

    lws_context_destroy(context);
    return 0;
}
