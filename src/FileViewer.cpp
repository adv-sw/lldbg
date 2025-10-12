#include "FileViewer.hpp"
#include "Defer.hpp"
#include "StringBuffer.hpp"

// clang-format off
#include "imgui.h"
#include "imgui_internal.h"
// clang-format on


#ifndef UNUSED
#define UNUSED(...) (void)(0, ##__VA_ARGS__)
#endif


// We work from description bcoz that is available for resolved & unresolved breakpoints.
void Breakpoint_Info(lldb::SBBreakpoint bp, std::string &file,  size_t &file_line, size_t &bp_id)
{
   lldb::SBStream description;
   bp.GetDescription(description, false);
   std::string desc = description.GetData();
   // "SBBreakpoint: ..."

   auto pos = desc.find("id = ");
         
   if (pos != std::string::npos)
      desc.erase(0,pos+5);

   pos = desc.find(",");
    
   if (pos != std::string::npos)
   {
      std::string param = desc.substr(0, pos);
      desc.erase(0,pos);
      bp_id = std::stoul(param,nullptr,0);
   }



   pos = desc.find("file = '");
         
   if (pos != std::string::npos)
      desc.erase(0,pos+8);

   pos = desc.find("'");
    

   if (pos != std::string::npos)
   {
      file = desc.substr(0, pos);
      desc.erase(0,pos);
   }

   
   pos = desc.find("line = ");
         
   if (pos != std::string::npos)
      desc.erase(0,pos+7);

   pos = desc.find(",");
    
   if (pos != std::string::npos)
   {
      std::string param = desc.substr(0, pos);
      desc.erase(0,pos);
      file_line = std::stoul (param,nullptr,0);
   }
}


// TODO: Optimize this - pull out breakpoints this file, sort them by line.
int BP_Match(lldb::SBTarget target, const std::string &file, size_t file_line)
{
   auto num_bp = target.GetNumBreakpoints();

    std::string candidate_file; 
    size_t candidate_file_line, candidate_bp_id;

    for (uint32_t i = 0; i < num_bp; i++) 
    {
        lldb::SBBreakpoint bp = target.GetBreakpointAtIndex(i);
        Breakpoint_Info(bp, candidate_file, candidate_file_line, candidate_bp_id);

        if ((candidate_file_line == file_line) && (candidate_file == file))
        {
            lldb::SBBreakpointLocation location = bp.GetLocationAtIndex(0);

            return location.IsValid() ? 1 : 2;
        }
    }

    return 0;
}


TextEditor::Markers BP_Get(lldb::SBTarget target, const Stack_Frames &stack_frames, const std::string &file)
{
   TextEditor::Markers markers;

   auto num_bp = target.GetNumBreakpoints();

   std::string candidate_file; 
   size_t candidate_file_line, candidate_bp_id;

    for (uint32_t i = 0; i < num_bp; i++) 
    {
        lldb::SBBreakpoint bp = target.GetBreakpointAtIndex(i);
        Breakpoint_Info(bp, candidate_file, candidate_file_line, candidate_bp_id);

        if (candidate_file == file)
        {
            lldb::SBBreakpointLocation location = bp.GetLocationAtIndex(0);

            auto bp_state = location.IsValid() ? 1 : 2;

            markers.insert(TextEditor::Marker((int) candidate_file_line, bp_state == 1 ? "b" : "p")); // b = resolved, p = pending.
        }
    }

    // Visualize stack frame.
    uint32_t level = 0;
    for (auto it = stack_frames.begin(); it != stack_frames.end(); ++it, level++) 
    {
        auto frame = *it;
          
        if (frame)
        {
            auto fp = frame->file_handle.filepath();

            if (fp == file)
            {
               auto marker_it = markers.find(frame->line);

               std::string id("a");
               id += std::to_string(level);

               if (marker_it == markers.end())
                  markers.insert(TextEditor::Marker(frame->line, id)); // a = active.
               else
                  (*marker_it).second.append(id);
            }
        }
    }

    return markers;
}


std::optional<int> FileViewer::Render(lldb::SBTarget target, const Stack_Frames &stack_frames)
{
    ImGuiContext& g = *GImGui;
    auto &style = g.Style;
    //ImGuiWindow* window = g.CurrentWindow;

    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, style.Colors[ImGuiCol_TitleBg]);
    Defer(ImGui::PopStyleColor());
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, style.Colors[ImGuiCol_TitleBg]);
    Defer(ImGui::PopStyleColor());

    std::optional<int> clicked_line = {};

   if (!m_editor)
   {
      m_editor = new TextEditor;
      m_editor->SetReadOnly(true);
      m_editor->SetShowWhitespaces(false);
      m_editor->SetTabSize(3);
      m_editor->SetPalette(m_editor->GetDarkPalette());
      m_editor->mLeftMargin = 2;

      StringBuffer line_buffer;
      std::string src_buffer;
      for (size_t i = 0; i < m_lines.size(); i++) 
      {
            const size_t line_number = i + 1;
            line_buffer.format("    {}    {}\n", line_number, m_lines[i]); 
            src_buffer.append(line_buffer.data());
            line_buffer.clear(); // ready for the next line.
      }

      // TODO: Morph this to void SetTextLines(const std::vector<std::string>& aLines);

      m_editor->SetText(src_buffer.c_str());
   }

      m_editor->Render("dbg");

   // Consume events from editor ...
  	if ((m_editor->mLastClick != m_consumed_last_click) && (m_editor->mClickMode > 1))
   {
      clicked_line = m_editor->mState.mCursorPosition.mLine+1;
      m_consumed_last_click = m_editor->mLastClick;
   }

   auto bps = BP_Get(target, stack_frames, m_path);
   m_editor->SetBreakpoints(bps);
  
   return clicked_line;
}


void FileViewer::Optimize_Breakpoints(lldb::SBTarget target)
{
   UNUSED(target);
   // TODO :)
}


void FileViewer::Show(FileHandle handle)
{
    m_lines = handle.contents();
    
    // TODO: Should use FileHandle here
    m_path = handle.filepath();
}