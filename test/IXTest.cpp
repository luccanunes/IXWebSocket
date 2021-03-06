/*
 *  IXTest.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2018 Machine Zone. All rights reserved.
 */

#include "IXTest.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixcobra/IXCobraMetricsPublisher.h>
#include <ixcrypto/IXUuid.h>
#include <mutex>
#include <random>
#include <stack>
#include <stdlib.h>
#include <string>
#include <thread>


namespace ix
{
    std::atomic<size_t> incomingBytes(0);
    std::atomic<size_t> outgoingBytes(0);
    std::mutex TLogger::_mutex;
    std::stack<int> freePorts;

    void setupWebSocketTrafficTrackerCallback()
    {
        ix::WebSocket::setTrafficTrackerCallback([](size_t size, bool incoming) {
            if (incoming)
            {
                incomingBytes += size;
            }
            else
            {
                outgoingBytes += size;
            }
        });
    }

    void reportWebSocketTraffic()
    {
        TLogger() << incomingBytes;
        TLogger() << "Incoming bytes: " << incomingBytes;
        TLogger() << "Outgoing bytes: " << outgoingBytes;
    }

    void msleep(int ms)
    {
        std::chrono::duration<double, std::milli> duration(ms);
        std::this_thread::sleep_for(duration);
    }

    std::string generateSessionId()
    {
        auto now = std::chrono::system_clock::now();
        auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

        return std::to_string(seconds);
    }

    void log(const std::string& msg)
    {
        TLogger() << msg;
    }

    void hexDump(const std::string& prefix, const std::string& s)
    {
        std::ostringstream ss;
        bool upper_case = false;

        for (std::string::size_type i = 0; i < s.length(); ++i)
        {
            ss << std::hex << std::setfill('0') << std::setw(2)
               << (upper_case ? std::uppercase : std::nouppercase) << (int) s[i];
        }

        std::cout << prefix << ": " << s << " => " << ss.str() << std::endl;
    }

    bool startWebSocketEchoServer(ix::WebSocketServer& server)
    {
        server.setOnClientMessageCallback(
            [&server](std::shared_ptr<ConnectionState> connectionState,
                      WebSocket& webSocket,
                      const ix::WebSocketMessagePtr& msg) {
                auto remoteIp = connectionState->getRemoteIp();
                if (msg->type == ix::WebSocketMessageType::Open)
                {
                    TLogger() << "New connection";
                    TLogger() << "Remote ip: " << remoteIp;
                    TLogger() << "Uri: " << msg->openInfo.uri;
                    TLogger() << "Headers:";
                    for (auto it : msg->openInfo.headers)
                    {
                        TLogger() << it.first << ": " << it.second;
                    }
                }
                else if (msg->type == ix::WebSocketMessageType::Close)
                {
                    TLogger() << "Closed connection";
                }
                else if (msg->type == ix::WebSocketMessageType::Message)
                {
                    for (auto&& client : server.getClients())
                    {
                        if (client.get() != &webSocket)
                        {
                            client->send(msg->str, msg->binary);
                        }
                    }
                }
            });

        auto res = server.listen();
        if (!res.first)
        {
            TLogger() << res.second;
            return false;
        }

        server.start();
        return true;
    }

    std::vector<uint8_t> load(const std::string& path)
    {
        std::vector<uint8_t> memblock;

        std::ifstream file(path);
        if (!file.is_open()) return memblock;

        file.seekg(0, file.end);
        std::streamoff size = file.tellg();
        file.seekg(0, file.beg);

        memblock.resize((size_t) size);
        file.read((char*) &memblock.front(), static_cast<std::streamsize>(size));

        return memblock;
    }

    std::string readAsString(const std::string& path)
    {
        auto vec = load(path);
        return std::string(vec.begin(), vec.end());
    }

    SocketTLSOptions makeClientTLSOptions()
    {
        SocketTLSOptions tlsOptionsClient;
        tlsOptionsClient.certFile = ".certs/trusted-client-crt.pem";
        tlsOptionsClient.keyFile = ".certs/trusted-client-key.pem";
        tlsOptionsClient.caFile = ".certs/trusted-ca-crt.pem";

        return tlsOptionsClient;
    }

    SocketTLSOptions makeServerTLSOptions(bool preferTLS)
    {
        // Start a fake sentry http server
        SocketTLSOptions tlsOptionsServer;
        tlsOptionsServer.certFile = ".certs/trusted-server-crt.pem";
        tlsOptionsServer.keyFile = ".certs/trusted-server-key.pem";
        tlsOptionsServer.caFile = ".certs/trusted-ca-crt.pem";

#if defined(IXWEBSOCKET_USE_MBED_TLS) || defined(IXWEBSOCKET_USE_OPEN_SSL)
        tlsOptionsServer.tls = preferTLS;
#else
        tlsOptionsServer.tls = false;
#endif
        return tlsOptionsServer;
    }

    std::string getHttpScheme()
    {
#if defined(IXWEBSOCKET_USE_MBED_TLS) || defined(IXWEBSOCKET_USE_OPEN_SSL)
        std::string scheme("https://");
#else
        std::string scheme("http://");
#endif
        return scheme;
    }

    std::string getWsScheme(bool preferTLS)
    {
        std::string scheme;
#if defined(IXWEBSOCKET_USE_MBED_TLS) || defined(IXWEBSOCKET_USE_OPEN_SSL)
        if (preferTLS)
        {
            scheme = "wss://";
        }
        else
        {
            scheme = "ws://";
        }
#else
        scheme = "ws://";
#endif
        return scheme;
    }

    snake::AppConfig makeSnakeServerConfig(int port, bool preferTLS)
    {
        snake::AppConfig appConfig;
        appConfig.port = port;
        appConfig.hostname = "127.0.0.1";
        appConfig.verbose = true;
        appConfig.redisPort = getFreePort();
        appConfig.redisPassword = "";
        appConfig.redisHosts.push_back("localhost"); // only one host supported now
        appConfig.socketTLSOptions = makeServerTLSOptions(preferTLS);

        std::string appsConfigPath("appsConfig.json");

        // Parse config file
        auto str = readAsString(appsConfigPath);
        if (str.empty())
        {
            std::cout << "Cannot read content of " << appsConfigPath << std::endl;
            return appConfig;
        }

        std::cout << str << std::endl;
        auto apps = nlohmann::json::parse(str);
        appConfig.apps = apps["apps"];

        // Display config on the terminal for debugging
        dumpConfig(appConfig);

        return appConfig;
    }

    std::string makeCobraEndpoint(int port, bool preferTLS)
    {
        std::stringstream ss;
        ss << getWsScheme(preferTLS) << "localhost:" << port;
        std::string endpoint = ss.str();

        return endpoint;
    }

    void runPublisher(const ix::CobraConfig& config, const std::string& channel)
    {
        ix::CobraMetricsPublisher cobraMetricsPublisher;
        cobraMetricsPublisher.configure(config, channel);
        cobraMetricsPublisher.setSession(uuid4());
        cobraMetricsPublisher.enable(true);

        Json::Value msg;
        msg["fps"] = 60;

        cobraMetricsPublisher.setGenericAttributes("game", "ody");

        // Wait a bit
        ix::msleep(500);

        // publish some messages
        cobraMetricsPublisher.push("sms_metric_A_id", msg); // (msg #1)
        cobraMetricsPublisher.push("sms_metric_B_id", msg); // (msg #2)
        ix::msleep(500);

        cobraMetricsPublisher.push("sms_metric_A_id", msg); // (msg #3)
        cobraMetricsPublisher.push("sms_metric_D_id", msg); // (msg #4)
        ix::msleep(500);

        cobraMetricsPublisher.push("sms_metric_A_id", msg); // (msg #4)
        cobraMetricsPublisher.push("sms_metric_F_id", msg); // (msg #5)
        ix::msleep(500);
    }
} // namespace ix
