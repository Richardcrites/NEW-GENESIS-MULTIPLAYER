#include "UI/ConnectScreen.h"
#include "SaveScanner/SaveScanner.h"
#include "Network/NetworkHost.h"
#include "Config.h"

#include <imgui.h>

#include <string>
#include <vector>
#include <cstring>

using namespace std::string_view_literals;

namespace Pulsar::UI
{
    static bool s_savesScanned = false;
    static std::vector<SaveScanner::SaveInfo> s_saves;
    static int  s_selectedSave = 0;
    static int  s_mode = 1; // 1=host, 0=client
    static char s_ipBuf[64] = "192.168.1.100";
    static int  s_port = 7777;
    static char s_statusMsg[256] = "Disconnected";

    static void ScanSavesOnce()
    {
        if (s_savesScanned) return;
        s_savesScanned = true;
        s_saves = SaveScanner::ScanSaves();
    }

    void DrawConnectScreen()
    {
        ScanSavesOnce();

        ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("PULSAR Co-op", nullptr, ImGuiWindowFlags_NoCollapse)) {
            ImGui::End();
            return;
        }

        // ─── Save List ───
        ImGui::Text("Your Saves:");
        ImGui::Separator();

        if (ImGui::BeginChild("SaveList", ImVec2(0, 180), true)) {
            for (int i = 0; i < static_cast<int>(s_saves.size()); i++) {
                auto& s = s_saves[i];

                // Format: "Name  Lv##  Location  Time"
                char label[256];
                snprintf(label, sizeof(label), "%-12s Lv%-3u  %-30s %s",
                    s.playerName.c_str(), s.playerLevel,
                    s.playerLocation.c_str(), s.playtime.c_str());

                if (ImGui::Selectable(label, s_selectedSave == i))
                    s_selectedSave = i;
            }
            if (s_saves.empty()) {
                ImGui::TextDisabled("No saves found");
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();

        // ─── Connection Settings ───
        ImGui::Text("Connection:");
        ImGui::Separator();

        // Mode toggle
        ImGui::RadioButton("Host", &s_mode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Client", &s_mode, 0);
        bool isHost = (s_mode == 1);

        // IP input (only for client)
        if (!isHost) {
            ImGui::SetNextItemWidth(200);
            ImGui::InputText("Host IP", s_ipBuf, sizeof(s_ipBuf));
        }

        // Port
        ImGui::SetNextItemWidth(100);
        ImGui::InputInt("Port", &s_port);
        if (s_port < 1024) s_port = 1024;
        if (s_port > 65535) s_port = 65535;

        ImGui::Spacing();

        // ─── Connect / Disconnect ───
        auto& net = Network::GetSingleton();
        bool isConnected = net.IsConnected();

        if (!isConnected) {
            if (ImGui::Button("Connect", ImVec2(120, 30))) {
                if (isHost) {
                    bool ok = net.StartHost(static_cast<uint16_t>(s_port));
                    snprintf(s_statusMsg, sizeof(s_statusMsg),
                        ok ? "Hosting on port %d" : "Host failed!", s_port);
                } else {
                    bool ok = net.Connect(s_ipBuf, static_cast<uint16_t>(s_port));
                    snprintf(s_statusMsg, sizeof(s_statusMsg),
                        ok ? "Connecting to %s:%d..." : "Connect failed!", s_ipBuf, s_port);
                }
            }
        } else {
            if (ImGui::Button("Disconnect", ImVec2(120, 30))) {
                net.Disconnect();
                snprintf(s_statusMsg, sizeof(s_statusMsg), "Disconnected");
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Refresh Saves", ImVec2(120, 30))) {
            s_savesScanned = false;
        }

        ImGui::Spacing();
        ImGui::Separator();

        // ─── Status ───
        if (isConnected) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Status: %s", s_statusMsg);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Status: %s", s_statusMsg);
        }

        ImGui::End();
    }
}
