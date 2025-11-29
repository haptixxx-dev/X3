#include <IconsFontAwesome6.h>
#include <implot.h>
#include "ProfilerPanel.h"

namespace Laura 
{

    void ProfilerPanel::OnImGuiRender() {
        EditorTheme& theme = m_EditorState->temp.editorTheme;

        if (!m_EditorState->temp.isProfilerPanelOpen) {
            return;
        }


        if (!m_Profiler->globalTimerSet) {
            LOG_ENGINE_WARN("Unable to render Profiler Panel - No Global Timer Set");
            if (m_EditorState->temp.isInRuntimeSimulation) {
                ImGui::EndDisabled();
            }
            return;
        }

        auto t = m_Profiler->timer("ProfilerPanel");

        ImPlot::PushColormap(ImPlotColormap_Deep);
        theme.PushColor(ImGuiCol_WindowBg, EditorCol_Background3);
        ImGui::Begin(ICON_FA_STOPWATCH " Profiler", &m_EditorState->temp.isProfilerPanelOpen);
        if (m_EditorState->temp.isInRuntimeSimulation) {
            ImGui::BeginDisabled();
        }

        const char* playLabel = (m_Profiler->isPaused) ? ICON_FA_PLAY : ICON_FA_PAUSE;
        theme.PushColor(ImGuiCol_Button, EditorCol_Primary1, 0.0f);
        if (ImGui::Button(playLabel)) {
            m_Profiler->isPaused = !m_Profiler->isPaused;
        }
        theme.PopColor();
        ImGui::SameLine();
        const ScrollingBuffer& buff = m_Profiler->getGlobalBuffer();
        if (!buff.empty()) {
            double Global_ms = buff.data()[buff.size() - 1];
            int Global_FPS = static_cast<int>(1000.0 / Global_ms); // FPS = 1/ms
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d FPS [%.1f ms]", Global_FPS, Global_ms);
            ImGui::SameLine();

            if (ImGui::Button("Clear " ICON_FA_REPEAT))
                m_Profiler->clear();
            ImGui::SameLine();
        }

        static ImPlotAxisFlags shouldAutoFit = ImPlotAxisFlags_AutoFit;
        ImGui::CheckboxFlags("Autofit", &shouldAutoFit, ImPlotAxisFlags_AutoFit);

        static int colorIndex = 0;
        static std::unordered_map<std::string, plotLineStyle> plotLineStyleMap;
        const ImPlotSubplotFlags SubPlot_Flags = ImPlotSubplotFlags_ShareItems | ImPlotSubplotFlags_NoTitle;
        theme.PushColor(ImGuiCol_FrameBg, EditorCol_Background3);
        if (ImPlot::BeginSubplots("##SUBPLOTS", 1, 1, ImVec2(-1, -1), SubPlot_Flags, 0)) { // Using subplot to allow chart scale independently from legend
            ImPlot::SetupLegend(ImPlotLocation_South | ImPlotLocation_West, 
                    ImPlotLegendFlags_Sort | ImPlotLegendFlags_Horizontal);
            ImPlot::BeginPlot("##LinePlot", ImVec2(-1, -1), ImPlotFlags_NoMouseText);
            ImPlot::SetupAxes(nullptr, nullptr, shouldAutoFit, shouldAutoFit | ImPlotAxisFlags_Opposite);
            ImPlot::SetupAxisFormat(ImAxis_Y1, "%gms");
            ImPlot::PushStyleVar(ImPlotStyleVar_FillAlpha, 0.05f);
            
            for (const auto& p : m_Profiler->data()) {
                const std::string label = p.first;
                const ScrollingBuffer& buff = p.second;

                auto it = plotLineStyleMap.find(label);
                if (it == plotLineStyleMap.end())
                    it = plotLineStyleMap.emplace(label, plotLineStyle(ImPlot::GetColormapColor(colorIndex), colorIndex++, 1.0f)).first;

                auto& lineStyle = it->second;
                ImPlot::SetNextLineStyle(lineStyle.color, lineStyle.thickness);
                ImPlot::PlotLine<double>(label.c_str(), buff.data(), buff.size());
                ImPlot::PlotShaded<double>(label.c_str(), buff.data(), buff.size());

                if (ImPlot::IsLegendEntryHovered(label.c_str())) {
                    ImGui::SetTooltip("Open %s Options", label.c_str());

                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        ImGui::OpenPopup(label.c_str());
                    }
                }

                if (ImGui::BeginPopup(label.c_str())) {
                    ImGui::Text("%s Options", label.c_str());
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::ColorPicker4("##clrpckr", (float*)&lineStyle.color, ImGuiColorEditFlags_NoInputs);
                    ImGui::SliderFloat("##thcknss", &lineStyle.thickness, 1.0f, 10.0f, "Thickness %.1f");
                    ImGui::SameLine();
                    if (ImGui::Button(ICON_FA_ROTATE)) {
                        lineStyle.color = ImPlot::GetColormapColor(lineStyle.colormapID);
                        lineStyle.thickness = 1.0f;
                    }
                    ImGui::EndPopup();
                }
            }
            ImPlot::PopStyleVar();
            ImPlot::EndPlot();
            ImPlot::EndSubplots();
        }
        theme.PopColor(); // frameBg
        
        if (m_EditorState->temp.isInRuntimeSimulation) {
            ImGui::EndDisabled();
        }
        
        ImGui::End();
        ImPlot::PopColormap();
        theme.PopColor(); // windowBg
    }
}
