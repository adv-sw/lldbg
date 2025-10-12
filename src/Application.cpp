// Optional sanitizer to simplify debugging of wasmtime client code.
#define LLDBG_FILTER_WASMTIME   1

// Silence low priorty warnings
#define _CRT_SECURE_NO_WARNINGS 1

#include "Application.h"
#include "Defer.hpp"
#include "Log.hpp"
#include "StringBuffer.hpp"
#include "fmt/format.h"

#if LLDBG_FILTER_WASMTIME
#define ENABLE_REGISTERS 0 
#else
#define ENABLE_REGISTERS 1
#endif

// On for program diagnostics
#define ENABLE_LOG 0 

// Requires implementing so off until done.
#define ENABLE_STDOUT 0 
#define ENABLE_STDERR 0 

#define SOURCE_FILE_SELECT_EXPRESSION "Source files {.cpp,.c,.rs,.h,.hpp}All files {.*}"

const float vertical_split_margin = 10.0f;

#ifdef _WIN32
#include <windows.h>
// TODO: Get these working with standard cmake system then can be removed.
#pragma comment(lib, "imgui.lib")
#pragma comment(lib, "Z:\\dev_public\\glew\\lib\\Release\\x64\\glew32s.lib")
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#endif


void BringWindowToTop();
void Breakpoint_Info(lldb::SBBreakpoint bp, std::string &file,  size_t &file_line, size_t &bp_id);

extern bool __debugger_live;

namespace fs = std::filesystem;

static std::map<std::string, std::string> s_debug_stream;



std::string Hex(size_t value)
{
   std::stringstream stream;
   stream << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
   return std::string (stream.str());
}


size_t Addr_FromHexString(const std::string &addr_str)
{
   return std::stoul(addr_str, nullptr, 16);
}

size_t Addr_FromIntString(const std::string &addr_str)
{
   return std::stoul(addr_str, nullptr, 10);
}


std::string Hex(const char *int_str)
{
   size_t addr = Addr_FromIntString(int_str);
   return Hex(addr);
}


inline void remove_last_path_component(std::string& path) {
   size_t pos = path.find_last_of("\\/");
   if (pos != std::string::npos) {
      path.resize(pos);
   }
}


// NOTE: This is updated once per frame and currently variables are never removed.
#define DEBUG_STREAM(x)                                \
    {                                                  \
        const std::string xkey = std::string(#x);      \
        auto it = s_debug_stream.find(xkey);           \
        const std::string xstr = fmt::format("{}", x); \
        if (it != s_debug_stream.end()) {              \
            it->second = xstr;                         \
        }                                              \
        else {                                         \
            s_debug_stream[xkey] = xstr;               \
        }                                              \
    }


static std::pair<fs::path, int> resolve_line(lldb::SBLineEntry line_entry)
{
    const char* filename = line_entry.GetFileSpec().GetFilename();
    const char* directory = line_entry.GetFileSpec().GetDirectory();

    if (!filename || !directory)
    {
        LOG(Error) << "Failed to read breakpoint location after thread halted.";
        return { fs::path(), -1 };
    }

    return { fs::path(directory) / fs::path(filename), line_entry.GetLine() };
}


static std::pair<fs::path, int> resolve_breakpoint(lldb::SBBreakpointLocation location)
{
    lldb::SBAddress address = location.GetAddress();
    return resolve_line(address.GetLineEntry());
}


static std::pair<bool, bool> process_is_finished(lldb::SBProcess& process)
{
    if (!process.IsValid())
        return { false, false };

    const lldb::StateType state = process.GetState();

    const bool exited = state == lldb::eStateExited;
    const bool failed = state == lldb::eStateCrashed;

    return {exited || failed, !failed};
}


static bool process_is_running(lldb::SBProcess& process)
{
    return process.IsValid() && process.GetState() == lldb::eStateRunning;
}


static bool process_is_stopped(lldb::SBProcess& process)
{
    const auto state = process.GetState();
    return process.IsValid() && (state == lldb::eStateStopped || state == lldb::eStateUnloaded);
}


static void stop_process(lldb::SBProcess& process)
{
    if (!process.IsValid()) 
    {
        LOG(Warning) << "Attempted to stop an invalid process.";
        return;
    }

    if (process_is_stopped(process)) 
    {
        LOG(Warning) << "Attempted to stop an already-stopped process.";
        return;
    }

    lldb::SBError err = process.Stop();
    
    if (err.Fail()) 
    {
        LOG(Error) << "Failed to stop the process, encountered the following error: "  << err.GetCString();
        return;
    }
}


static void continue_process(lldb::SBProcess& process)
{
    if (!process.IsValid()) 
    {
        LOG(Warning) << "Attempted to continue an invalid process.";
        return;
    }

    if (process_is_running(process)) 
    {
        LOG(Warning) << "Attempted to continue an already-running process.";
        return;
    }

    lldb::SBError err = process.Continue();
    
    if (err.Fail()) 
    {
        LOG(Error) << "Failed to continue the process, encountered the following error: " << err.GetCString();
        return;
    }
}


static void kill_process(lldb::SBProcess& process)
{
    if (!process.IsValid()) 
    {
        LOG(Warning) << "Attempted to kill an invalid process.";
        return;
    }

    if (process_is_finished(process).first) 
    {
        LOG(Warning) << "Attempted to kill an already-finished process.";
        return;
    }

    lldb::SBError err = process.Kill();

    if (err.Fail()) 
    {
        LOG(Error) << "Failed to kill the process, encountered the following error: \n\t" << err.GetCString();
        return;
    }
}


static std::string build_string(const char* cstr)
{
    return cstr ? std::string(cstr) : std::string();
}


static void glfw_error_callback(int error, const char* description)
{
    StringBuffer buffer;
    buffer.format("GLFW Error {}: {}\n", error, description);
    LOG(Error) << buffer.data();
}


Stack_Frame *Stack_Frame::create(lldb::SBFrame frame)
{
   lldb::SBFileSpec spec = frame.GetLineEntry().GetFileSpec();
   fs::path filename = fs::path(build_string(spec.GetFilename()));
   fs::path directory = fs::path(build_string(spec.GetDirectory()));

   if (!fs::exists(directory)) 
   {
      // LOG(Warning) << "Directory specified by lldb stack frame doesn't exist: " <<
      // directory; LOG(Warning) << "Filepath specified: " << filename;
      return nullptr;
   }

   auto _path = directory / filename;
   std::string path{ _path.u8string() };
        
   if (auto handle = FileHandle::create(path); handle.has_value()) 
   {
      return new Stack_Frame(*handle, (int)frame.GetLineEntry().GetLine(),
                        (int)frame.GetLineEntry().GetColumn(),
                        build_string(frame.GetDisplayFunctionName()));
   }
   else 
   {
      LOG(Warning) << "Filepath corresponding to lldb stack frame doesn't exist: " << directory / filename;
      return nullptr;
   }
}


#if 0
static bool FileTreeNode(const char* label)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;

    ImGuiID id = window->GetID(label);
    ImVec2 pos = window->DC.CursorPos;
    ImRect bb(pos, ImVec2(pos.x + ImGui::GetContentRegionAvail().x,
                          pos.y + g.FontSize + g.Style.FramePadding.y * 2));

    bool opened = ImGui::TreeNodeBehaviorIsOpen(id);
    bool hovered, held;

    if (ImGui::ButtonBehavior(bb, id, &hovered, &held, true))
        window->DC.StateStorage->SetInt(id, opened ? 0 : 1);

    if (hovered || held)
        window->DrawList->AddRectFilled(bb.Min, bb.Max,
            ImGui::GetColorU32(held ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered));

    // Icon, text
    float button_sz = g.FontSize + g.Style.FramePadding.y * 2;
    window->DrawList->AddRectFilled(pos, ImVec2(pos.x + button_sz, pos.y + button_sz),
                                    opened ? ImColor(51, 105, 173) : ImColor(42, 79, 130));

    const auto label_location = ImVec2(pos.x + button_sz + g.Style.ItemInnerSpacing.x, pos.y + g.Style.FramePadding.y);

    ImGui::RenderText(label_location, label);

    ImGui::ItemSize(bb, g.Style.FramePadding.y);
    ImGui::ItemAdd(bb, id);

    if (opened) 
       ImGui::TreePush(label);

    return opened;
}
#endif


static bool Splitter(const char* name, bool split_vertically, float thickness, float* size1,
                     float* size2, float min_size1, float min_size2, float splitter_long_axis_size)
{
    using namespace ImGui;
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    ImGuiID id = window->GetID(name);
    ImRect bb;
    bb.Min = window->DC.CursorPos + (split_vertically ? ImVec2(*size1, 0.0f) : ImVec2(0.0f, *size1));
    bb.Max = bb.Min + CalcItemSize(split_vertically ? ImVec2(thickness, splitter_long_axis_size)
                                                    : ImVec2(splitter_long_axis_size, thickness), 0.0f, 0.0f);

    return SplitterBehavior(bb, id, split_vertically ? ImGuiAxis_X : ImGuiAxis_Y, size1, size2,
                            min_size1, min_size2, 0.0f);
}


static std::optional<lldb::SBTarget> find_target(lldb::SBDebugger &debugger)
{
    if (debugger.GetNumTargets() > 0) 
    {
        auto target = debugger.GetSelectedTarget();
        
        if (!target.IsValid()) 
        {
            LOG(Warning) << "Selected target is invalid.";
            return {};
        }
        else
            return target;
    }

    return {};
}


size_t Application::Breakpoint_Locate(lldb::SBTarget target, const char *file, uint32_t bp_line)
{
    auto num_bp = target.GetNumBreakpoints();

    std::string candidate_path; 
    size_t candidate_line=0, bp_id=0;

    for (uint32_t i = 0; i < num_bp; i++) 
    {
        lldb::SBBreakpoint bp = target.GetBreakpointAtIndex(i);
        Breakpoint_Info(bp, candidate_path, candidate_line, bp_id);

        if (candidate_line != bp_line)
            continue;
        
         if (!strcmp(file, candidate_path.c_str()))
           return bp_id;
   }

   return 0;
}


// Displays & interacts with source code windows.
static void SourceView_Draw(Application &app)
{
    bool closed_tab = false;

    app.m_open_files.for_each_open_file([&](FileHandle handle, bool is_focused) 
    {
        auto action = OpenFiles::Action::Nothing;

        // We programmatically set the focused tab if manual tab change requested
        // for example when the user clicks an entry in the stack trace or file explorer
        auto tab_flags = ImGuiTabItemFlags_None;

        if (app.ui.request_manual_tab_change && is_focused) 
{
            tab_flags = ImGuiTabItemFlags_SetSelected;
            app.file_viewer.Show(handle);
        }

        bool keep_tab_open = true;

        if (ImGui::BeginTabItem(handle.filename().c_str(), &keep_tab_open, tab_flags)) 
        {
            ImGui::BeginChild("FileContents");
            
            if (!app.ui.request_manual_tab_change && !is_focused) 
            {
                // User selected tab directly with mouse
                action = OpenFiles::Action::ChangeFocusTo;
                app.file_viewer.Show(handle);
            }

            auto target = find_target(app.m_debugger);

            if (target.has_value())
            {
               std::optional<int> clicked_line = app.file_viewer.Render(target.value(), app.m_frames);
            
               if (clicked_line.has_value()) 
               {
                   std::optional<FileHandle> focus_handle = app.m_open_files.focus();
                
                   if (focus_handle.has_value()) 
                   {
                       const fs::path filepath = focus_handle->filepath();
                       StringBuffer breakpoint_command;

                       size_t found_index = app.Breakpoint_Locate(target.value(), filepath.string().c_str(), *clicked_line);

                        if (found_index > 0)
                        {
                           const uint32_t max_bp_index = target->GetNumBreakpoints() - 1;

                           if (app.ui.viewed_breakpoint_index > max_bp_index) 
                              app.ui.viewed_breakpoint_index = max_bp_index;

                           breakpoint_command.format("breakpoint delete {}", found_index);
                        }
                        else
                        {
#if 0
                           // TODO: Optionally use API directly instead of command line.
                           lldb::SBBreakpoint bp = target.BreakpointCreateByLocation(m_path.c_str(), static_cast<uint32_t>(*clicked_line));
#else

                           breakpoint_command.format("breakpoint set --file {} --line {}",
                                                   filepath.string().c_str(), *clicked_line);
#endif
                        }

                        run_lldb_command(app, breakpoint_command.data(), true);

                   }
               }
            }

            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (!keep_tab_open) 
        {  
            // User closed tab with mouse
            closed_tab = true;
            action = OpenFiles::Action::Close;
        }

        return action;
    });

    app.ui.request_manual_tab_change = false;

    if (closed_tab && app.m_open_files.size() > 0) 
    {
        auto focus_handle = app.m_open_files.focus();
        
        if (!focus_handle.has_value()) 
        {
            LOG(Error) << "Invalid logic encountered when user requested tab close.";
        }
        else
            app.file_viewer.Show(*focus_handle);
    }
}


static void manually_open_and_or_focus_file(User_Interface &ui, OpenFiles &open_files,
                                            FileHandle handle)
{
    if (auto focus = open_files.focus(); focus.has_value() && (*focus == handle)) 
    {
        return;  // Already focused
    }

    open_files.open(handle);
    ui.request_manual_tab_change = true;
}


static void manually_open_and_or_focus_file(User_Interface &ui, OpenFiles &open_files, const std::string &path)
{
   try
   {
      auto fh = FileHandle::create(path.c_str());  

      if (fh.has_value())
      {
         manually_open_and_or_focus_file(ui, open_files, *fh);
         return;
      }
   }
   catch (...)
   {
   }

   LOG(Warning) << "Could not open : " << path;
}


static std::optional<lldb::SBProcess> find_process(lldb::SBDebugger &debugger)
{
    auto target = find_target(debugger);
    
    if (!target.has_value()) 
         return {};

    lldb::SBProcess process = target->GetProcess();

    if (process.IsValid())
        return process;

    return {};
}


static lldb::SBCommandReturnObject run_lldb_command(lldb::SBDebugger& debugger,
                                                    LLDBCommandLine& cmdline,
                                                    const lldb::SBListener& listener,
                                                    const char* command,
                                                    bool hide_from_history = false)
{
#if 0
    if ( auto unaliased_cmd = cmdline.expand_and_unalias_command(command); unaliased_cmd.has_value() )  
    {
        LOG(Debug) << "Unaliased command: " << *unaliased_cmd;
    }
#endif

    if (!strcmp(command, "quit"))
       exit(0);

    auto target_before = find_target(debugger);
    lldb::SBCommandReturnObject ret = cmdline.run_command(command, hide_from_history);
    auto target_after = find_target(debugger);

    const bool added_new_target = !target_before && target_after;
    const bool switched_target = target_before && target_after && (*target_before != *target_after);

    if (added_new_target || switched_target) 
    {
        constexpr auto target_listen_flags = lldb::SBTarget::eBroadcastBitBreakpointChanged | lldb::SBTarget::eBroadcastBitWatchpointChanged;
        target_after->GetBroadcaster().AddListener(listener, target_listen_flags);
    }

#if 0
    switch (ret.GetStatus()) 
    {
        case lldb::eReturnStatusInvalid:
            LOG(Debug) << "\t => eReturnStatusInvalid";
            break;
        case lldb::eReturnStatusSuccessFinishNoResult:
            LOG(Debug) << "\t => eReturnStatusSuccessFinishNoResult";
            break;
        case lldb::eReturnStatusSuccessFinishResult:
            LOG(Debug) << "\t => eReturnStatusSuccessFinishResult";
            break;
        case lldb::eReturnStatusSuccessContinuingNoResult:
            LOG(Debug) << "\t => eReturnStatusSuccessContinuingNoResult";
            break;
        case lldb::eReturnStatusSuccessContinuingResult:
            LOG(Debug) << "\t => eReturnStatusSuccessContinuingResult";
            break;
        case lldb::eReturnStatusStarted:
            LOG(Debug) << "\t => eReturnStatusStarted";
            break;
        case lldb::eReturnStatusFailed:
            LOG(Debug) << "\t => eReturnStatusFailed";
            break;
        case lldb::eReturnStatusQuit:
            LOG(Debug) << "\t => eReturnStatusQuit";
            break;
        default:
            LOG(Debug) << "unknown lldb command return status encountered.";
            break;
    }
#endif

    return ret;
}


lldb::SBCommandReturnObject run_lldb_command(Application &app, const char* command, bool hide_from_log)
{
    return run_lldb_command(app.m_debugger, app.m_cmd, app.listener, command, hide_from_log);
}


#include "./widget/file/ImGuiFileDialog/ImGuiFileDialog.h"

static uint32_t Widget_File_Selector(std::string &dest, bool activate, const char *key_id, const char *pattern, const char *hint, const std::string &start_path)
{ 
  auto fdialog = ImGuiFileDialog::Instance();
   
  if (activate)
  {
     // Configure
     ImGui::SetNextWindowPos(ImVec2(50, 50));
     ImGui::SetNextWindowSize(ImVec2(1000 * 0.75f, 700));

     fdialog->OpenDialog(key_id, hint, pattern, start_path.c_str());
  }

  uint32_t result = 0;

  if (fdialog->Display(key_id)) 
  {
    // Confirmed ...
    result = 0x02;

    if (fdialog->IsOk())
    {
       dest = ImGuiFileDialog::Instance()->GetFilePathName(false);
       result |= 0x01;
    }

    // Close
    fdialog->Close();
  }
   
  return result;
}


void Text_Centered(const std::string &text, uint32_t column_width) 
{
    auto textWidth = ImGui::CalcTextSize(text.c_str()).x;

    ImGui::SetCursorPosX((column_width - textWidth) * 0.5f);
    ImGui::Text(text.c_str());
}


static void Target_Select(Application &app, LLDBCommandLine &cmdline,
                          const lldb::SBListener &listener, const User_Interface &ui)
{
    auto target = find_target(app.m_debugger);
    
    if (target.has_value()) 
    {
        Text_Centered("Selected Configuration", (uint32_t) app.ui.vertical_split_1_position);
        ImGui::Text("");
       
        lldb::SBFileSpec fs = target->GetExecutable();
        StringBuffer target_description;
        const char* target_directory = fs.GetDirectory();
        const char* target_filename = fs.GetFilename();

        if (target_directory && target_filename)
            target_description.format("{}/{}", target_directory, target_filename);
        else
            target_description.format("Invalid target.");

        ImGui::TextUnformatted(target_description.data());

        lldb::SBLaunchInfo linfo = target.value().GetLaunchInfo();
        auto count = linfo.GetNumArguments();
        std::string target_params;
        for (uint32_t i=0;i < count; i++)
        {
            if (i != 0)
               target_params += " ";

            target_params += linfo.GetArgumentAtIndex(i);
        }

        ImGui::TextUnformatted(target_params.data());
        ImGui::Text("");
    }

    auto process = find_process(app.m_debugger);

#if 0    
    if (process.has_value()) 
    {
        StringBuffer process_description;
        const char* process_state = lldb::SBDebugger::StateAsCString(process->GetState());
        process_description.format("Process State: {}", process_state);
        ImGui::TextUnformatted(process_description.data());
    }
    else if (target.has_value()) 
    {
        ImGui::TextUnformatted("Process State: Unlaunched");
    }
#endif


    if (!target.has_value()) 
    {
       ImGui::TextUnformatted(""); 
       Text_Centered("Launch Configurations", (uint32_t) app.ui.vertical_split_1_position); 
       ImGui::TextUnformatted("");

       uint32_t entry_id = 0;

       for (auto i = app.m_launch_configs.begin(); i != app.m_launch_configs.end(); ++i, entry_id++)
       {
           auto config = *i;

           ImGui::Separator();

           std::string id(config->cmd);

           if (ImGui::Button(id.c_str()))
           {
               config->change_target_active = true;
           }

           if (config->change_target_active)
           {
                std::string start_path = config->cmd;
                static std::string selected_path = "";

                auto result = Widget_File_Selector(selected_path, config->change_target_active, "target", ".exe", "Change target", start_path);

                if (result & 0x02)
                {
                   config->change_target_active = false;
               
                   if (result & 0x01)
                        config->cmd = selected_path;
                }
            }

          
            char input_buf[2048];
            strncpy(input_buf, config->args.c_str(), ARRAY_SIZE(input_buf));
            
            char widget_id_str[32];
            sprintf(widget_id_str, "##%d", entry_id);

            ImGui::PushItemWidth(float(app.ui.vertical_split_1_position - 10));
            if (ImGui::InputText(widget_id_str, input_buf, 2048, ImGuiInputTextFlags_EnterReturnsTrue, nullptr)) 
            {
                config->args = input_buf;
            }
            ImGui::PopItemWidth();

            sprintf(widget_id_str, "Load##%d", entry_id);

            if (ImGui::Button(widget_id_str, ImVec2(float(ui.vertical_split_1_position * 0.65), 0)))
            {
               std::string target_id("target create -- "); target_id += config->cmd;
               run_lldb_command(app.m_debugger, cmdline, listener, target_id.c_str());
 
#if 1
               std::string args = "settings set target.run-args "; args += config->args;
               run_lldb_command(app.m_debugger, cmdline, listener, args.c_str());
#else
               target = find_target(app.m_debugger);
               const char *argv[2] = { lt->args.c_str(), nullptr } ;
               lldb::SBLaunchInfo linfo = target.value().GetLaunchInfo();
               linfo.SetArguments(argv, false);
#endif

               // Required for Mac. 
               // Set as default this way Windows/Linux, but set anyway just to be sure :)
               run_lldb_command(app.m_debugger, cmdline, listener, "settings set plugin.jit-loader.gdb.enable on", true);


               for (auto j=config->m_open_files.begin(); j != config->m_open_files.end(); ++j)
                  manually_open_and_or_focus_file(app.ui, app.m_open_files, (*j).c_str());

               config->m_current = true;
            }

            ImGui::SameLine();
            sprintf(widget_id_str, "Delete##%d", entry_id);
            if (ImGui::Button(widget_id_str))
            {
                app.m_launch_configs.remove(config);
                delete config;
                break; // Draw correctly next cycle.
            }

            ImGui::Separator();
            ImGui::Text("");
       }
   
       static bool target_select_active = false;
       ImGui::Text("");  ImGui::Text("");
       if (ImGui::Button("Add Target"))
       {
           target_select_active = true;
       }

       static std::string start_path = "";
       static std::string selected_path = "";

       auto result = Widget_File_Selector(selected_path, target_select_active, "target", ".exe", "Select target", start_path);

       if (result & 0x02)
       {
          target_select_active = false;
           
          if (result & 0x01)
          {
               auto t = new Launch_Target;

               t->cmd = selected_path;
               t->args = "";

               app.m_launch_configs.push_back(t);
          }
       }
    }
      
    else if (!process.has_value())
    {
        if (ImGui::Button("Launch >", ImVec2(float(app.ui.vertical_split_1_position - 5), 0)))
        {
            run_lldb_command(app.m_debugger, cmdline, listener, "run");
        }
    }

    else if (process_is_stopped(*process))
    {
        ImGui::Text(""); ImGui::Separator(); ImGui::Text("");

        if (ImGui::Button("Continue >"))
        {
            continue_process(*process);
            //run_lldb_command(app.m_debugger, cmdline, listener, "continue");
        }
        ImGui::SameLine();
        
        if (ImGui::Button("Step Over"))
        {
#if 1
            run_lldb_command(app.m_debugger, cmdline, listener, "n");
#else
            // TODO: No idea why this approach doesn't work.
            const uint32_t nthreads = process->GetNumThreads();
            if (ui.viewed_thread_index < nthreads) 
            {
                lldb::SBThread th = process->GetThreadAtIndex(ui.viewed_thread_index);
                th.StepOver();
            }
#endif
        }
        
        if (ImGui::Button("Step Into")) 
        {
#if 1
            run_lldb_command(app.m_debugger, cmdline, listener, "s");
#else
            // TODO: No idea why this approach doesn't work.
            const uint32_t nthreads = process->GetNumThreads();
            if (ui.viewed_thread_index < nthreads) 
            {
                lldb::SBThread th = process->GetThreadAtIndex(ui.viewed_thread_index);
                th.StepInto();
            }
#endif
        }

        ImGui::SameLine();

        if (ImGui::Button("Step Out")) 
        {
             run_lldb_command(app.m_debugger, cmdline, listener, "finish");
        }
    }
    else if (process_is_running(*process)) 
    {
        if (ImGui::Button("Stop")) 
        {
            stop_process(*process);
        }
    }
    else if (const auto [finished, _] = process_is_finished(*process); finished) 
    {
        if (ImGui::Button("Restart")) 
        {
            run_lldb_command(app.m_debugger, cmdline, listener, "run");
        }
    }
    else 
    {
        LOG(Error) << "Unknown/Invalid session state encountered!";
    }
}


static void draw_file_viewer(Application &app)
{
    ImGui::BeginChild("FileViewer", ImVec2(app.ui.vertical_split_2_position - app.ui.vertical_split_1_position - vertical_split_margin, float(app.ui.m_column_2_height)));

    if (ImGui::BeginTabBar("##FileViewerTabs", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_NoTooltip)) 
    {
        Defer(ImGui::EndTabBar());

        if (app.m_open_files.size() == 0)
        {
            // No files open so display title / credits page.

            if (ImGui::BeginTabItem("Welcome")) 
            {
                Defer(ImGui::EndTabItem());
                ImGui::TextUnformatted("");
                ImGui::TextUnformatted("LLDBG is an ImGUI based visual front end for the lldb debugger.");
                ImGui::TextUnformatted(""); ImGui::TextUnformatted("");
                ImGui::TextUnformatted("v1.01 including wasmtime support by Advance Software.");
                ImGui::TextUnformatted("");
                ImGui::TextUnformatted("Alpha by Zac Meadows.");
                ImGui::TextUnformatted("");
                ImGui::TextUnformatted("Thanks to all contributors.");
                ImGui::TextUnformatted("");
       

                auto script_lang = app.m_debugger.GetScriptLanguage();

                switch (script_lang) {
                case lldb::ScriptLanguage::eScriptLanguagePython:
                   ImGui::TextUnformatted("Python scripting is available in LLDB.");
                   break;
                case lldb::ScriptLanguage::eScriptLanguageLua:
                   ImGui::TextUnformatted("Lua scripting is available in LLDB.");
                   break;
                case  lldb::ScriptLanguage::eScriptLanguageNone:
                   ImGui::TextUnformatted("No scripting language available in LLDB.");
                   break;
                default:
                   ImGui::TextUnformatted("Unexpected scripting language is active: ");
                   break;
                }

                ImGui::TextUnformatted("");
            }
        }
        else
            SourceView_Draw(app);
    }

    ImGui::EndChild();
}


static void draw_console(Application &app)
{
    ImGui::BeginChild("LogConsole", ImVec2(app.ui.vertical_split_2_position - app.ui.vertical_split_1_position - vertical_split_margin,
                             float(app.ui.console_height - 2 * ImGui::GetFrameHeightWithSpacing())));

    if (ImGui::BeginTabBar("##ConsoleLogTabs", ImGuiTabBarFlags_None)) 
    {
        if (ImGui::BeginTabItem("Command")) 
        {
            ImGui::BeginChild("ConsoleEntries");

            for (const CommandLineEntry &entry : app.m_cmd.get_history()) 
            {
                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "> %s", entry.input.c_str());
                
                if (!entry.succeeded) 
                {
                    ImGui::TextColored(ImVec4(0.8f, 0.0f, 0.0f, 1.0f), "Error: %s is not a valid command.", entry.input.c_str());
                    continue;
                }

                if (entry.output.size() > 0)
                    ImGui::TextUnformatted(entry.output.c_str());
            }

            // Later in this method we scroll to the bottom of the command history if
            // a command was run last frame so that the user can immediately see the output.
            const bool should_auto_scroll_command_window = app.ui.ran_command_last_frame || app.ui.window_resized_last_frame;

            ImGuiInputTextCallback command_input_callback = nullptr;

            /*[](ImGuiTextEditCallbackData*) -> int 
            {
                return 0;  // TODO: Scroll command line history with up/down arrows
            };*/

            const ImGuiInputTextFlags command_input_flags = ImGuiInputTextFlags_EnterReturnsTrue;

            // Keep console input focused unless user is doing something else
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                !ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked(0)) 
            {
                ImGui::SetKeyboardFocusHere(0);
            }

            const float intensity = 0.8f;
            ImGui::TextColored(ImVec4(intensity, intensity, intensity, 1.0f), "(lldb)"); 
            ImGui::SameLine();

            static char input_buf[2048];
            if (ImGui::InputText("", input_buf, 2048, command_input_flags,
                                 command_input_callback)) 
            {
                run_lldb_command(app, input_buf);
                memset(input_buf, 0, sizeof(input_buf));
                input_buf[0] = '\0';
                app.ui.ran_command_last_frame = true;
            }

            // Always keep keyboard input focused on the lldb console input box unless some other disrupting action is occuring.
            if (ImGui::IsItemHovered() ||  (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootWindow) &&
                 !ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked(0))) 
            {
                ImGui::SetKeyboardFocusHere(-1);  // Auto focus previous widget
            }

            if (should_auto_scroll_command_window) 
            {
                ImGui::SetScrollHereY(1.0f);
                app.ui.ran_command_last_frame = false;
            }

            ImGui::EndChild();

            ImGui::EndTabItem();
        }

#if ENABLE_LOG
        if (ImGui::BeginTabItem("Log")) 
        {
            ImGui::BeginChild("LogEntries");
            Logger::get_instance()->for_each_message([](const LogMessage &entry) -> void 
            {
                const char* msg = entry.message.c_str();
                
                switch (entry.level) 
                {
                    case LogLevel::Verbose: 
                    {
                        ImGui::TextColored(ImVec4(78.f / 255.f, 78.f / 255.f, 78.f / 255.f, 255.f),
                                           "[VERBOSE]");
                        break;
                    }
      
                    case LogLevel::Debug: 
                    {
                        ImGui::TextColored(ImVec4(52.f / 255.f, 56.f / 255.f, 176.f / 255.f, 255.f / 255.f),
                            "[DEBUG]");
                        break;
                    }
                    case LogLevel::Info: 
                    {
                        ImGui::TextColored(ImVec4(225.f / 255.f, 225.f / 255.f, 225.f / 255.f, 255.f / 255.f),
                            "[INFO]");
                        break;
                    }
                    case LogLevel::Warning: 
                    {
                        ImGui::TextColored(ImVec4(216.f / 255.f, 129.f / 255.f, 42.f / 255.f, 255.f / 255.f),
                            "[WARNING]");
                        break;
                    }
                    case LogLevel::Error: 
                    {
                        ImGui::TextColored(ImVec4(212.f / 255.f, 67.f / 255.f, 67.f / 255.f, 255.f / 255.f),
                            "[ERROR]");
                        break;
                    }
                }
                ImGui::SameLine();
                ImGui::TextWrapped("%s", msg);
            });

            static size_t last_seen_messages = 0;

            const size_t seen_messages = Logger::get_instance()->message_count();
            
            if (seen_messages > last_seen_messages) 
            {
                last_seen_messages = seen_messages;
                ImGui::SetScrollHereY(1.0f);
            }

            ImGui::EndChild();
            ImGui::EndTabItem();
        }
#endif

        // TODO: these quantities need to be reset whenever the target is reset or process is
        // re-launched
        static size_t last_stdout_size = 0;
        static size_t last_stderr_size = 0;

#if ENABLE_STDOUT
        if (ImGui::BeginTabItem("stdout")) 
        {
            ImGui::BeginChild("StdOUTEntries");

            ImGui::TextUnformatted(app._stdout.get());
            if (app._stdout.size() > last_stdout_size) 
            {
                ImGui::SetScrollHereY(1.0f);
            }

            last_stdout_size = app._stdout.size();

            ImGui::EndChild();
            ImGui::EndTabItem();
        }
#endif

#if ENABLE_STDERR
        if (ImGui::BeginTabItem("stderr")) 
        {
            ImGui::BeginChild("StdERREntries");
            ImGui::TextUnformatted(app._stderr.get());
            if (app._stderr.size() > last_stderr_size) 
            {
                ImGui::SetScrollHereY(1.0f);
            }

            last_stderr_size = app._stderr.size();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
#endif

        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}



static void draw_threads(std::list<Thread*> &threads, User_Interface &ui, float stack_height, float pos_x)
{
    ImGui::BeginChild("#ThreadsChild", ImVec2(pos_x, stack_height));

    Defer(ImGui::EndChild());

    // TODO: Be consistent about whether or not to use Defer.
    // TODO: Add columns with stop reason and other potential information.
    if (ImGui::BeginTabBar("#ThreadsTabs", ImGuiTabBarFlags_None))
    {
        Defer(ImGui::EndTabBar());

        if (ImGui::BeginTabItem("Threads")) 
        {
            Defer(ImGui::EndTabItem());
            
            //if (process.has_value() && process_is_stopped(*process)) 
            {
                StringBuffer thread_label;
                for (uint32_t i=0; i< threads.size(); i++)

                for (auto it = threads.begin(); it != threads.end(); it++)
                {
                    auto th = *it;

                    if (th->m_id != i)
                      continue;
                    
                    std::string thread_name = th->m_name;

#if LLDBG_FILTER_WASMTIME
                    // Skip clutter from suspended threads we don't care about.
                    if (strstr(thread_name.c_str(), "NtWait") || strstr(thread_name.c_str(), "NtDelay"))
                       continue;

                    // Tidy up default thread name.

                    size_t pos = thread_name.find("JIT");

                    if (pos != std::string::npos)
                    {
                        uint32_t match = 0;
                        thread_name.erase(0,pos);
                        pos = thread_name.find('`');
                        if (pos != std::string::npos)
                        {
                           thread_name.erase(0, pos+1);
                           match++;
                        }
                        
                        pos = thread_name.find(" at \x1b");

                        if (pos != std::string::npos)
                        {
                           thread_name = thread_name.substr(0, pos);
                           match++;
                        }

                        if (match ==2)
                           thread_name = "root";
                        else
                           thread_name = th->m_name; // Revert - unexpected format.
                    }
#endif
 
                    thread_label.format("{}", thread_name.c_str());

                    if (ImGui::Selectable(thread_label.data(), i == ui.viewed_thread_index)) 
                        ui.viewed_thread_index = i;

                    thread_label.clear();
                }
            }
        }
    }
}


static void draw_stack_trace(std::list<Stack_Frame *> &stack_frame, User_Interface &ui, OpenFiles &open_files,
                             float stack_height)
{
    ImGui::BeginChild("#StackTraceChild", ImVec2(0, stack_height));

    if (ImGui::BeginTabBar("##StackTraceTabs", ImGuiTabBarFlags_None)) 
    {
        if (ImGui::BeginTabItem("Stack Trace")) 
        {
            //if (process.has_value() && process_is_stopped(*process)) 
            {
                ImGui::Columns(3, "##StackTraceColumns");
                ImGui::Separator();
                ImGui::Text("FUNCTION");
                ImGui::NextColumn();
                ImGui::Text("FILE");
                ImGui::NextColumn();
                ImGui::Text("LINE");
                ImGui::NextColumn();
                ImGui::Separator();

                uint32_t i = 0;
                
                for (auto it = stack_frame.begin(); it != stack_frame.end(); ++it, ++i) 
                {
                    auto frame = *it;

                    if (!frame) 
                       continue;

#if LLDBG_FILTER_WASMTIME
                    // Filter runtime bootstrap entrypoints from the visualization to keep it simple (tm).
                    if (!strcmp(frame->function_name.c_str(), "wasmtime_setjmp"))
                       continue;
#endif

                    if (ImGui::Selectable(frame->function_name.c_str(), i == ui.viewed_frame_index, ImGuiSelectableFlags_SpanAllColumns)) 
                    {
                        manually_open_and_or_focus_file(ui, open_files, frame->file_handle);
                        ui.viewed_frame_index = i;
                    }

                    ImGui::NextColumn();

                    ImGui::TextUnformatted(frame->file_handle.filename().c_str());
                    ImGui::NextColumn();

                    StringBuffer linebuf;
                    linebuf.format("{}", (int)frame->line);
                    ImGui::TextUnformatted(linebuf.data());
                    ImGui::NextColumn();
                }
                ImGui::Columns(1);
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::EndChild();
}



#if LLDBG_FILTER_WASMTIME

void Virtual_Address_Eval(lldb::SBValue &dest, lldb::SBFrame frame, const std::string this_type, size_t virtual_addr)
{
   std::stringstream addr_as_hex;
   addr_as_hex << "0x" << std::hex << virtual_addr;

   std::string cmd_to_eval = "*(";
   cmd_to_eval += this_type;
   cmd_to_eval += "*)";
   cmd_to_eval += "resolve_vmctx_memory("; 
   cmd_to_eval += addr_as_hex.str(); 
   cmd_to_eval += ")";

   dest = frame.EvaluateExpression(cmd_to_eval.c_str());
}


bool GetVirtualAddr(size_t &virtual_addr, lldb::SBValue param)
{
    const char *param_err = param.GetError().GetCString();
   
    if (param_err)
    {
         // Extract
         if (sscanf(param_err, "read memory from %zx failed", &virtual_addr))
            return true;
    }

    return false;
}


size_t Virtual_Address_Register(lldb::SBFrame frame, Virtual_Values &entries, lldb::SBValue param)
{
    size_t virtual_addr = 0;
    if (GetVirtualAddr(virtual_addr, param))
    {
         // Virtual resolve.
         lldb::SBValue result;
         const char* param_type = param.GetDisplayTypeName();
         Virtual_Address_Eval(result, frame, param_type, virtual_addr);

         Virtual_Value entry;
         entry.m_value = result;
         auto value_ptr = result.GetValue();

         if (value_ptr)
            entry.m_value_str = value_ptr;

         entry.m_virtual_addr = virtual_addr;
         entries.push_back(entry);
         return virtual_addr;
    }

    return 0;
}


void eval_wasmtime(lldb::SBFrame frame, Virtual_Values &entries, lldb::SBValue param)
{
    // Globals with "invalid" virtual addresses fail directly.
    // The following implements our workaround to resolve them.
    bool parsed = Virtual_Address_Register(frame, entries, param) != 0;

    bool global_ptr = false;

    if (!parsed)
    {
       const char* param_type = param.GetDisplayTypeName();

       if (!strncmp(param_type, "WebAssemblyPtrWrapper<", 22))
       {
            const char *param_name = param.GetName();

            lldb::SBValue result;

            size_t virtual_addr=0;
            bool invalid = !param.MightHaveChildren();

            // Grab virtual address
            lldb::SBValue param_vaddr = param.GetChildAtIndex(0);
            const char *virtual_addr_str = param_vaddr.GetValue();
   
            if (virtual_addr_str)
               virtual_addr = Addr_FromIntString(virtual_addr_str);
            else
            {
               // Global/static pointers currently 'fail' like this providing the address of the pointer, which we then
               // decode twice - first get the pointer itself, then dereference it.

               if (GetVirtualAddr(virtual_addr, param_vaddr))
                  global_ptr = true;  // globals give us address where they are, not their value. 
            }


            if (!invalid && param_name)
            {
               // Filter: NULL this.
               // At the end of a constructor, virtual_addr is sometimes 0 - likely out of scope. We filter out here.
               if (virtual_addr == 0)
                  return;

               // Extract type
               std::string this_type = param_type + 22;
               this_type.pop_back();

               if (global_ptr)
                  this_type.push_back('*');

               // Resolve virtual address.
               Virtual_Address_Eval(result, frame, this_type, virtual_addr);

               Virtual_Value entry;
                  
               // We store value itself so its hierarchy can be explored in display window.         
               entry.m_value = result;
               auto result_value = result.GetValue();
                  
               if (result_value)
                  entry.m_value_str = result_value;
               else
               {
                  const char *result_err = result.GetError().GetCString();
                  
                  if (result_err)
                     LOG(Error) << "Invalid expression:\n"  << result_err;
               }

               entry.m_virtual_addr = virtual_addr;
               entries.push_back(entry);
               parsed = true;
            }
       }
    }

    for (uint32_t i = 0; i < param.GetNumChildren(100); i++) 
    {
        eval_wasmtime(frame, entries, param.GetChildAtIndex(i));
    }
}


#endif // LLDBG_FILTER_WASMTIME



Virtual_Value *Find(Virtual_Values &evaluated, size_t virtual_addr)
{
   for(auto i = evaluated.begin(); i != evaluated.end(); ++i)
   {
         auto &entry = *i;

         if (entry.m_virtual_addr == virtual_addr)
            return &entry;
   }

   return nullptr;
}


void Display_Known_Type(lldb::SBValue &param)
{
    const char *param_name = param.GetName();
    const char *param_type = param.GetDisplayTypeName();


   ImGui::TextUnformatted(param_name);
   ImGui::NextColumn();
   ImGui::TextUnformatted(param_type);
   ImGui::NextColumn();

   std::string formatted_value;
   int index = 0;
   for (;;)
   {
      auto field = param.GetChildAtIndex(index);
      index++;

      if (!field.IsValid())
         break;

      if (!formatted_value.empty())
         formatted_value.append("\t");

      formatted_value.append(field.GetValue());
   }

   if (!formatted_value.empty())
      ImGui::TextUnformatted(formatted_value.c_str());
   else
      ImGui::TextUnformatted("...");
}


static void draw_var_recursive(Virtual_Values &evaluated, lldb::SBValue param)
{
    const char *param_name = param.GetName();
    const char *param_type = param.GetDisplayTypeName();
    const char *param_value = nullptr;

    bool wrapped_wasm_ptr = false;
    bool wrapped_global_wasm_ptr = false;
    Virtual_Value *virtual_value = nullptr;

#if LLDBG_FILTER_WASMTIME

    if (param_name && !strcmp(param_name, "__vmctx"))
       return; // Skip internals.

    size_t virtual_addr = 0;
    bool is_virtual_addr = GetVirtualAddr(virtual_addr, param);

    // Lookup resolved value.
    if (is_virtual_addr)
    {
       virtual_value = Find(evaluated, virtual_addr);
       param = virtual_value ? virtual_value->m_value : param;
    }

    wrapped_wasm_ptr = param_type && !strncmp(param_type, "WebAssemblyPtrWrapper<", 22);

    std::string tmp;

    if (wrapped_wasm_ptr)
    {
        if (!param.MightHaveChildren())
            return; // invalid - ignore.

         // Grab wasm pointer ..
        param = param.GetChildAtIndex(0);
        param_value = param.GetValue();

        if (param_value)
        {
            virtual_addr = Addr_FromIntString(param_value);
            is_virtual_addr = virtual_addr != 0;
        }
        else
        {
            // Possible global wrapped ptr ...

            is_virtual_addr = GetVirtualAddr(virtual_addr, param);

            // Lookup resolved value.
            if (is_virtual_addr)
            {
                virtual_value = Find(evaluated, virtual_addr);
                param = virtual_value ? virtual_value->m_value : param;
                param_value = param.GetValue();
                wrapped_global_wasm_ptr = true;
            }
        }

        if (wrapped_global_wasm_ptr && param_value)
            virtual_addr = Addr_FromHexString(param_value + 2); // skip past 0x

        // At the end of a constructor, we sometimes receive a null this. 
        // We currently ignore. Might be possible to fix up by remembering value from previous call.
        if ( !strcmp(param_name, "this") || !strcmp(param_name, "self") )
        {
            if (virtual_addr == 0)
               return;
        }

        // Demangle type.
        tmp = param_type+22; tmp.pop_back();
        tmp.append(" *");
        param_type = tmp.c_str();
    }

#else
    const bool is_virtual_addr = false;
#endif

    if (!param_type || !param_name)
        return;

    StringBuffer child_node_label;
    child_node_label.format("{}##Children_{}", param_name, param.GetID());

    bool is_null_virtual_ptr = wrapped_wasm_ptr && (virtual_addr == 0);
    bool descend = wrapped_wasm_ptr && (virtual_addr != 0);
    descend |= (param.MightHaveChildren() && !is_null_virtual_ptr);

    if (descend)  // Structure or pointer.
    {
       const char *node_id = child_node_label.data() ? child_node_label.data() : "value";

#if 1
      // TODO: Matrix :)

       bool known_type = !strcmp(param_type, "Vector2") || !strcmp(param_type, "Vector3") || !strcmp(param_type, "Vector4");

       if (known_type)
       {
           Display_Known_Type(param);
       }
#endif
        else if (ImGui::TreeNode(node_id)) 
        {

#if LLDBG_FILTER_WASMTIME
            bool is_inspection_child = false;

            if (is_virtual_addr)
            { 
               virtual_value = Find(evaluated, virtual_addr);
               param = virtual_value ? virtual_value->m_value : param;
               is_inspection_child = wrapped_global_wasm_ptr;
            }
#endif

            ImGui::NextColumn();
            ImGui::TextUnformatted(param_type);
            ImGui::NextColumn();

#if LLDBG_FILTER_WASMTIME

            if (is_virtual_addr)
            {
              std::string result;

               if (wrapped_wasm_ptr)
               {
                   if (wrapped_global_wasm_ptr && param_value)
                      virtual_addr = Addr_FromHexString(param_value+2); // skip past 0x

                   result = virtual_addr == 0 ? "null" : Hex(virtual_addr);
               }
               else
                  result = param_value;
            
               ImGui::TextUnformatted(result.c_str());
            }
#endif // LLDBG_FILTER_WASMTIME
 
            ImGui::NextColumn();

            if (is_inspection_child)
                draw_var_recursive(evaluated, param);
            else
            {
                if (is_virtual_addr && (virtual_addr != 0) || !is_virtual_addr)
                {
                    for (uint32_t i = 0; i < param.GetNumChildren(100); i++)
                       draw_var_recursive(evaluated, param.GetChildAtIndex(i));
                }
            }

            ImGui::TreePop();
        }
        else 
        {
            ImGui::NextColumn();
            ImGui::TextUnformatted(param_type);
            ImGui::NextColumn();
   
#if LLDBG_FILTER_WASMTIME
            if (is_virtual_addr)
            {
               std::string result;
               result = virtual_addr == 0 ? "null" : Hex(virtual_addr);
               ImGui::TextUnformatted(result.c_str());
            }
            else
#endif // LLDBG_FILTER_WASMTIME
               ImGui::TextUnformatted("...");

            ImGui::NextColumn();
        }
    }
    else 
    {
        if ( (param_name[0] == '$') || !strncmp(param_name, "*$", 2) )
            ImGui::TextUnformatted("");  // Skip lldb automatic variable names - means we've fully dereferenced - looks better blank.
        else
            ImGui::TextUnformatted(param_name);

        ImGui::NextColumn();
        ImGui::TextUnformatted(param_type);
        ImGui::NextColumn();

        if (!virtual_value)
        {
            param_value = param.GetValue();   
         }
        
         if (!param_value)
            ImGui::TextUnformatted(virtual_value ? virtual_value->m_value_str.c_str() : "?");
         else 
         {
             ImGui::TextUnformatted(is_null_virtual_ptr ? "null" : param_value);
         }
       
        ImGui::NextColumn();
    }
}


void Application::Display_Variables_Registers(float stack_height)
{
    std::optional<lldb::SBProcess> process = find_process(m_debugger);
    auto target = find_target(m_debugger);

    ImGui::BeginChild("#Watch", ImVec2(0, stack_height));

    if (ImGui::BeginTabBar("##WatchTabs", ImGuiTabBarFlags_None)) 
    {

        if (ImGui::BeginTabItem("Locals")) 
        {
            if (process.has_value() && process_is_stopped(*process)) 
            {
                ImGui::Columns(3, "##LocalsColumns");
                ImGui::Separator();
                ImGui::Text("NAME");
                ImGui::NextColumn();
                ImGui::Text("TYPE");
                ImGui::NextColumn();
                ImGui::Text("VALUE");
                ImGui::NextColumn();
                ImGui::Separator();

                // TODO: Select entire row like in stack trace
                for (uint32_t i = 0; i < m_locals.GetSize(); i++) 
                {
                   draw_var_recursive(m_evaluated_locals, m_locals.GetValueAtIndex(i));
                }

                ImGui::Columns(1);
            }

            ImGui::EndTabItem();
        }


        if (ImGui::BeginTabItem("Globals")) 
        {
            if (process.has_value() && process_is_stopped(*process)) 
            {
                ImGui::Columns(3, "##GlobalsColumns");
                ImGui::Separator();
                ImGui::Text("NAME");
                ImGui::NextColumn();
                ImGui::Text("TYPE");
                ImGui::NextColumn();
                ImGui::Text("VALUE");
                ImGui::NextColumn();
                ImGui::Separator();

                // TODO: Select entire row like in stack trace
                for (uint32_t i = 0; i < m_globals.GetSize(); i++) 
                {
                   draw_var_recursive(m_evaluated_globals, m_globals.GetValueAtIndex(i));
                }

                ImGui::Columns(1);
            }

            ImGui::EndTabItem();
        }

#if ENABLE_REGISTERS
        if (ImGui::BeginTabItem("Registers")) 
        {
            if (process.has_value() && process_is_stopped(*process)) 
            {
                lldb::SBThread viewed_thread = process->GetThreadAtIndex(ui.viewed_thread_index);
                lldb::SBFrame frame = viewed_thread.GetFrameAtIndex(ui.viewed_frame_index);
                
                if (viewed_thread.IsValid() && frame.IsValid()) 
                {
                    lldb::SBValueList register_collections = frame.GetRegisters();

                    for (uint32_t i = 0; i < register_collections.GetSize(); i++) 
                    {
                        lldb::SBValue regcol = register_collections.GetValueAtIndex(i);

                        const char* collection_name = regcol.GetName();

                        if (!collection_name) 
                        {
                            LOG(Warning) << "Skipping over invalid/un-named register collection.";
                            continue;
                        }

                        StringBuffer reg_coll_name;
                        reg_coll_name.format("{}##RegisterCollection", collection_name);
                        
                        if (ImGui::TreeNode(reg_coll_name.data())) 
                        {
                            for (uint32_t i = 0; i < regcol.GetNumChildren(); i++) 
                            {
                                lldb::SBValue reg = regcol.GetChildAtIndex(i);
                                const char* reg_name = reg.GetName();
                                const char* reg_value = reg.GetValue();

                                if (!reg_name || !reg_value) {
                                    LOG(Warning) << "Skipping invalid register.";
                                    continue;
                                }

                                ImGui::Text("%s = %s", reg_name, reg_value);
                            }

                            ImGui::TreePop();
                        }
                    }
                }
            }
            
            ImGui::EndTabItem();
        }
#endif // ENABLE_REGISTERS

        ImGui::EndTabBar();
    }

    ImGui::EndChild();
}


static void draw_breakpoints_and_watchpoints(User_Interface &ui, OpenFiles& open_files,
                                             std::optional<lldb::SBTarget> target,
                                             float stack_height)
{
    ImGui::BeginChild("#BreakWatchPointChild", ImVec2(0, stack_height));

    if (ImGui::BeginTabBar("##BreakWatchPointTabs", ImGuiTabBarFlags_None)) 
    {
        Defer(ImGui::EndTabBar());

        if (ImGui::BeginTabItem("Breakpoints")) 
        {
            Defer(ImGui::EndTabItem());

            if (target.has_value()) 
            {
                ImGui::Columns(2);
                ImGui::Separator();
                ImGui::Text("FILE");
                ImGui::NextColumn();
                ImGui::Text("LINE");
                ImGui::NextColumn();
                ImGui::Separator();
                Defer(ImGui::Columns(1));

                const uint32_t nbreakpoints = target->GetNumBreakpoints();

                if (ui.viewed_breakpoint_index >= nbreakpoints) 
                    ui.viewed_breakpoint_index = nbreakpoints - 1;

                for (uint32_t i = 0; i < nbreakpoints; i++) 
                {
                    lldb::SBBreakpoint breakpoint = target->GetBreakpointAtIndex(i);

                    // Required for pending breakpoints.
                    std::string file;  
                    size_t pending_line_id=0, pending_bp_id=0;
   
                    // For resolved breakpoints.
                    lldb::SBBreakpointLocation location;
                    lldb::SBAddress address;
                    lldb::SBLineEntry line_entry;

                    if (!breakpoint.IsValid()) 
                        continue;

                     if (breakpoint.GetNumLocations() > 0)
                     {
                        location = breakpoint.GetLocationAtIndex(0);

                        if (location.IsValid()) 
                        {
                           address = location.GetAddress();

                           if (!address.IsValid()) 
                           {
                              LOG(Error) << "Invalid breakpoint address encountered.";
                              continue;
                           }

                           line_entry = address.GetLineEntry();

                           if (!line_entry.IsValid()) 
                           {
                              LOG(Error) << "Invalid line entry encountered.";
                              continue;
                           }
                        }
                    }
                    else
                    {
                        Breakpoint_Info(breakpoint, file, pending_line_id, pending_bp_id);
                    }

                     std::string filename, directory;

                     if (pending_bp_id > 0)
                     {
                         auto path = fs::path(file);
                         filename = path.filename().string();
                         directory = path.parent_path().string();
                     }
                     else
                     {
                       auto fs = line_entry.GetFileSpec();
      
                       filename  = fs.GetFilename();
                       directory = fs.GetDirectory();
                     }

                    if (filename.empty() || directory.empty())
                    {
                        LOG(Error) << "Breakpoint found with invalid path - ignored.";
                        continue;
                    }

                    if (ImGui::Selectable(filename.c_str(), i == ui.viewed_breakpoint_index, ImGuiSelectableFlags_SpanAllColumns)) 
                    {
                        fs::path breakpoint_filepath = fs::path(directory) / fs::path(filename);
                        manually_open_and_or_focus_file(ui, open_files, breakpoint_filepath.string().c_str());
                        ui.viewed_breakpoint_index = i;
                    }

                    ImGui::NextColumn();

                    StringBuffer line_buf;

                    if (pending_bp_id > 0)
                       line_buf.format("{}", pending_line_id);
                    else
                       line_buf.format("{}", line_entry.GetLine());

                    ImGui::TextUnformatted(line_buf.data());
                    ImGui::NextColumn();
                }
            }
        }

#if 0
        if (ImGui::BeginTabItem("Watchpoints")) 
        {
            Defer(ImGui::EndTabItem());

            // TODO: Implement watch points
            for (int i = 0; i < 4; i++) 
            {
                StringBuffer label;
                label.format("Watch {}", i);

                if (ImGui::Selectable(label.data(), i == 0)) 
                {
                    // TODO
                }
            }
        }
#endif

    }
    ImGui::EndChild();
}

#if 0
static void draw_diagnostics_popup(User_Interface &ui)
{
    ImGui::SetNextWindowPos(ImVec2(ui.window_width / 2.f, ui.window_height / 2.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 200), ImGuiCond_FirstUseEver);

    ImGui::PushFont(ui.font);

    if (ImGui::Begin("Diagnostics", 0)) 
    {
        for (const auto& [xkey, xstr] : s_debug_stream) 
        {
            StringBuffer debug_line;
            debug_line.format("{} : {}", xkey, xstr);
            ImGui::TextUnformatted(debug_line.data());
        }
    }

    ImGui::PopFont();

    ImGui::End();
}
#endif


__attribute__((flatten)) static void draw(Application &app)
{
    auto& ui = app.ui;
    auto& open_files = app.m_open_files;

    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ui.window_width, ui.window_height), ImGuiCond_Always);

    static constexpr auto main_window_flags =
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoTitleBar;

    ImGui::Begin("lldbg", 0, main_window_flags);
    ImGui::PushFont(ui.font);

    {
        Splitter("##vertical_split_1", true, 3.0f, &ui.vertical_split_1_position, &ui.vertical_split_1_max,
                 0.1f * ui.window_width, 0.1f * ui.window_width, ui.window_height);

        Splitter("##vertical_split_2", true, 3.0f, &ui.vertical_split_2_position, &ui.vertical_split_2_max,
                 ui.vertical_split_2_min, 0.1f * ui.window_width, ui.window_height);


        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.0f, 0.05f, 1.0f));

        ImGui::BeginChild("ControlBarAndFileBrowser", ImVec2(ui.vertical_split_1_position, 0));

        Target_Select(app, app.m_cmd, app.listener, app.ui);

         auto target = find_target(app.m_debugger);

         if (target.has_value()) 
         {
             ImGui::Text("");
             ImGui::Separator();
             ImGui::Text("");

             static std::string start_path;
             std::string selected_path;
             static bool open_active = false;

             ImGui::Text("");

             if (ui.vertical_split_1_position < 280)
             {
               ImGui::Text("Open source files to");
               ImGui::Text("set breakpoints.");
             }
             else
               ImGui::Text("Open source files to set breakpoints.");

             ImGui::Text("");

             if (ImGui::Button("Open")) 
                open_active = true;


             if (app.m_open_files.size() > 0)
             {
                 ImGui::Text("");
                 ImGui::Text("");
                 ImGui::Text("Help");
                 ImGui::Text("");

                 if (ui.vertical_split_1_position < 280)
                     ImGui::Text("Double click in source to place & remove breakpoints.");
                 else
                 {
                      ImGui::Text("Double click in source to");
                      ImGui::Text("place & remove breakpoints.");
                 }
             }


             auto result = Widget_File_Selector(selected_path, open_active, "source", SOURCE_FILE_SELECT_EXPRESSION, "Select source", start_path);

             if (result & 0x02)
             {
                open_active = false;
           
                if (result & 0x01)
                {
                   manually_open_and_or_focus_file(app.ui, app.m_open_files, selected_path);
                   start_path = open_active;
                }
             }
        }

        ImGui::EndChild();
        
        ImGui::PopStyleColor();
        ImGui::EndGroup();
    }

    ImGui::SameLine();


    {
        Splitter("##S2", false, 3.0f, &ui.m_column_2_height, &ui.console_height,
                 0.1f * ui.window_height, 0.1f * ui.window_height, ui.vertical_split_2_position - ui.vertical_split_1_position - vertical_split_margin);

        ImGui::BeginGroup();
        draw_file_viewer(app);
        ImGui::Spacing();
        draw_console(app);
        ImGui::EndGroup();
    }

    ImGui::SameLine();





    {
        ImGui::BeginGroup();

        // TODO: Let locals tab have all the expanded space
        const float stack_height = (ui.window_height - 2 * ImGui::GetFrameHeightWithSpacing()) / 4;

        draw_threads(app.m_threads, ui, stack_height, ui.vertical_split_2_position);
        draw_stack_trace(app.m_frames, ui, open_files, stack_height);
        app.Display_Variables_Registers(stack_height);
        draw_breakpoints_and_watchpoints(ui, open_files, find_target(app.m_debugger), stack_height);

        ImGui::EndGroup();
    }

    ImGui::PopFont();
    ImGui::End();

#if 0
    draw_diagnostics_popup(ui);
#endif
}


void Application::State_Sync(std::optional<lldb::SBProcess> _process, lldb::SBThread &viewed_thread)
{
   auto process = _process.value();

   lldb::SBFrame frame = viewed_thread.GetFrameAtIndex(ui.viewed_frame_index);

   m_locals  = frame.GetVariables(true, true, false, true);
   m_globals = frame.GetVariables(false, false, true, true);

#if LLDBG_FILTER_WASMTIME

   m_evaluated_locals.clear();
   m_evaluated_globals.clear();

   // Note: Currently noisy : Sends the following to console - don't care. Works. Want it to shut up :)
   // "warning: `this' is not accessible (substituting 0). Couldn't load 'this' because its value couldn't be evaluated"

   for (uint32_t i = 0; i < m_globals.GetSize(); i++) 
   {
        eval_wasmtime(frame, m_evaluated_globals, m_globals.GetValueAtIndex(i));
   }

   for (uint32_t i = 0; i < m_locals.GetSize(); i++) 
   {
        eval_wasmtime(frame, m_evaluated_locals, m_locals.GetValueAtIndex(i));
   }

#endif

   const uint32_t nframes = viewed_thread.GetNumFrames();

   if (ui.viewed_frame_index >= nframes) 
      ui.viewed_frame_index = nframes - 1;
 
   // Clear previous.
   while (!m_frames.empty())  
   {
      auto s = *m_frames.begin();
      if (s) delete s;
      m_frames.pop_front();
   }

   for (uint32_t i = 0; i < viewed_thread.GetNumFrames(); i++) 
   {
      auto new_frame = Stack_Frame::create(viewed_thread.GetFrameAtIndex(i));
      m_frames.push_back(new_frame);
   }


   // Clear previous.
   while (!m_threads.empty())  
   {
      auto t = *m_threads.begin();
      if (t) delete t;
      m_threads.pop_front();
   }


   const uint32_t nthreads = process.GetNumThreads();
                
   for (uint32_t i = 0; i < nthreads; i++) 
   {
      lldb::SBThread th = process.GetThreadAtIndex(i);
      lldb::SBStream desc;
      th.GetDescription(desc);
      m_threads.push_back(new Thread(th.GetIndexID(), desc.GetData()));
   }
}


void Application::Handle_Events()
{
    lldb::SBEvent event;

    while (true) 
    {
        const bool event_found = listener.GetNextEvent(event);
        if (!event_found) break;

        if (!event.IsValid()) 
        {
            LOG(Warning) << "Invalid event found.";
            continue;
        }

        lldb::SBStream event_description;
        event.GetDescription(event_description);
        //LOG(Verbose) << "Event Description => " << event_description.GetData();

        auto target = find_target(m_debugger);
        auto process = find_process(m_debugger);

        if (target.has_value() && event.BroadcasterMatchesRef(target->GetBroadcaster())) 
        {
            //LOG(Debug) << "Found target event.";
            file_viewer.Optimize_Breakpoints(*target);
        }
       
        if (process.has_value() && event.BroadcasterMatchesRef(process->GetBroadcaster())) 
        {
            const lldb::StateType new_state = lldb::SBProcess::GetStateFromEvent(event);
            const char* state_descr = lldb::SBDebugger::StateAsCString(new_state);

#if 1
            if (state_descr) 
            {
                LOG(Debug) << "Found process event with new state : " << state_descr;
            }
#endif

            // For now we find the first (if any) stopped thread and construct a StopInfo.
            if (new_state == lldb::eStateStopped) 
            {
                BringWindowToTop();

                const uint32_t nthreads = process->GetNumThreads();
                
                for (uint32_t i = 0; i < nthreads; i++) 
                {
                    lldb::SBThread th = process->GetThreadAtIndex(i);
                    auto sr = th.GetStopReason();
                    
                    switch (sr) 
                    {
                        case lldb::eStopReasonException:
                        {
                            LOG(Debug) << "Unexpected exception.";
                            break;
                        }

                        case lldb::eStopReasonBreakpoint: 
                        {
                            // https://lldb.llvm.org/cpp_reference/classlldb_1_1SBThread.html#af284261156e100f8d63704162f19ba76


                            assert(th.GetStopReasonDataCount(); >= 2);
                            lldb::break_id_t breakpoint_id = (lldb::break_id_t) th.GetStopReasonDataAtIndex(0);
                            lldb::SBBreakpoint breakpoint =
                                target->FindBreakpointByID(breakpoint_id);

                            lldb::break_id_t location_id = (lldb::break_id_t) th.GetStopReasonDataAtIndex(1);
                            lldb::SBBreakpointLocation location =
                                breakpoint.FindLocationByID(location_id);

                            const auto [filepath, linum] = resolve_breakpoint(location);
                            manually_open_and_or_focus_file(ui, m_open_files, filepath.string());
                            file_viewer.set_highlight_line(linum);

                            State_Sync(process, th); // Grab locals, etc.

                            break;
                        }

                        case lldb::eStopReasonPlanComplete: 
                        {
                            //auto dc = th.GetStopReasonDataCount();
                            auto f = th.GetSelectedFrame();
                            auto le = f.GetLineEntry();

                            const auto [filepath, linum] = resolve_line(le);
                            manually_open_and_or_focus_file(ui, m_open_files, filepath.string());
                            file_viewer.set_highlight_line(linum);

                            State_Sync(process, th); // Grab locals, etc.

                            break;
                        }

                        default:
                            continue;
                    }
                }
            }
            else if (new_state == lldb::eStateRunning) 
            {
                file_viewer.unset_highlight_line();
            }
        }
        else 
        {
            // TODO: Print event description
            LOG(Debug) << "Found non-target/process event.";
        }
    }
}


static void tick(Application &app)
{
    app.Handle_Events();

    User_Interface &ui = app.ui;
    DEBUG_STREAM(ui.window_width);
    DEBUG_STREAM(ui.window_height);
    DEBUG_STREAM(ui.vertical_split_1_position);
    DEBUG_STREAM(ui.vertical_split_1_max);
    DEBUG_STREAM(ui.m_column_2_height);
    DEBUG_STREAM(ui.console_height);
    DEBUG_STREAM(app.fps_timer.current_fps());

    draw(app);
}


static void update_window_dimensions(User_Interface &ui)
{
    int new_width = -1;
    int new_height = -1;

    glfwGetFramebufferSize(ui.window, &new_width, &new_height);
     
    // Handle case of minimizing parent native window ...
    if ((new_width == 0) || (new_height == 0))
    {
       new_width = -1;
       new_height = -1;
       return;
    }

    ui.window_resized_last_frame = new_width != ui.window_width || new_height != ui.window_height;

    if (ui.window_resized_last_frame) 
    {
        // Rescale the size of the invididual panels to account for window resize
        ui.vertical_split_1_position *= new_width / ui.window_width;
        ui.vertical_split_1_max   *= new_width / ui.window_width;
        ui.m_column_2_height *= new_height / ui.window_height;
        ui.console_height *= new_height / ui.window_height;

        ui.window_width  = (float) new_width;
        ui.window_height = (float) new_height;
    }
}


bool Parse_Element_End(std::string &line)
{
   bool element_complete = false;
   
   if (line.back() == '>')
       line.pop_back();

   if (line.back() == '/')
   {
      element_complete = true;
      line.pop_back();
   }

   if (line.back() == '\"')
   {
      line.pop_back();
   }

   return element_complete;
}



#if defined _WIN32 || defined _WIN64

static bool Config_GetPath(std::string &dest)
{
   bool aLocal = true;

   HKEY key;
   LPCWSTR keyName =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders";
   DWORD res = ::RegOpenKeyExW(HKEY_CURRENT_USER, keyName, 0, KEY_READ,
      &key);
   if (res != ERROR_SUCCESS) {
      dest.clear();
      return false;
   }

   DWORD type=0, size=0;

   res = RegQueryValueExA(key, (aLocal ? "Local AppData" : "AppData"),
      nullptr, &type, nullptr, &size);

   // The call to RegQueryValueEx must succeed, the type must be REG_SZ, the
   // buffer size must not equal 0
   if (res != ERROR_SUCCESS || type != REG_SZ || size == 0) {
      ::RegCloseKey(key);
      dest.clear();
      return false;
   }

   char *value = new char[size + 1];

   res = RegQueryValueExA(key, (aLocal ? "Local AppData" : "AppData"),
      nullptr, nullptr, (LPBYTE) value, &size);

   ::RegCloseKey(key);

   if (res == ERROR_SUCCESS)
   {
      value[size] = '\0';
      dest.assign(value);
      dest += "\\lldbg.ini";
      return true;
   }
   else
   {
      dest.clear();
      return false;
   }
}
#else
static bool Config_GetPath(std::string &dest, bool aLocal)
{
   dest.assign(getenv("HOME"));
   dest += "/lldbg.ini";
}
#endif


void Application::Config_Load()
{
   std::string config_path;
   Config_GetPath(config_path);

   std::ifstream infile(config_path.c_str());

   std::string line; std::string cmd; std::string args;

   while (std::getline(infile, line))
   {
      if (line.compare(0, 8, "<target "))
         continue;  // super basic xml parser, just grab element we care about.

      line.erase(0, 8);

      auto t = new Launch_Target;

      if (!line.compare(0, 5, "cmd=\""))
      {
         line.erase(0, 5);
         auto pos = line.find("\" args=\"");

         if (pos == std::string::npos)
            continue; // can't parse.

         cmd = line.substr(0, pos);
         line.erase(0, pos+1);
      }

      bool element_complete = false;

      if (!line.compare(0, 7, " args=\""))
      {
         line.erase(0, 7);

         element_complete = Parse_Element_End(line);

         args = line;
      }

      // there's more ...
      if (!element_complete)
      {
         while (std::getline(infile, line))
         {
              if (!line.compare(0, 5, "</>"))
              {
                  element_complete = true;
                  break;
              }

              if (!line.compare(0, 10, "<file id=\""))
              {
                  line.erase(0, 10);
                  element_complete = Parse_Element_End(line);
                  assert(element_complete); // complex file elements not currently supported.
                  t->m_open_files.push_back(line);
              }
         }
      }

      if (!cmd.empty())
      {
         t->cmd = cmd;
         t->args = args;
         m_launch_configs.push_back(t);
      }
   }
}


void Application::Config_Save()
{
   std::string config_path;
   Config_GetPath(config_path);;

   FILE *fp = fopen(config_path.c_str(), "w");
   fprintf(fp, "<?xml version=\"1.0\"?>\n<lldbg version=\"0.1\">\n");

   // Save launch configs
   for (auto i = m_launch_configs.begin(); i != m_launch_configs.end(); ++i)
   {
      auto lt = *i;
      fprintf(fp, "<target cmd=\"%s\" args=\"%s\">\n", lt->cmd.c_str(), lt->args.c_str());

      if (lt->m_current)
      {
         for (auto j = m_open_files.m_files.begin(); j != m_open_files.m_files.end(); ++j)
         {
             FileHandle fh = *j;
             const fs::path filename = fh.filepath(); // wide string
             fprintf(fp, "<file id=\"%S\"/>\n", filename.c_str());
         }
      }
      else
      {
         for (auto j = lt->m_open_files.begin(); j != lt->m_open_files.end(); ++j)
         {
            auto filename = *j;
            fprintf(fp, "<file id=\"%s\"/>\n", filename.c_str());
         }
      }
      fprintf(fp, "</>\n");
   }

   fprintf(fp, "</lldbg>\n");
   fclose(fp);
}


std::optional<User_Interface> User_Interface::init(void)
{
    User_Interface ui;

    glfwSetErrorCallback(glfw_error_callback);

    if (glfwInit() != GLFW_TRUE) 
        return {};

#ifdef __APPLE__
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif

#ifdef WIN32
    RECT rect { 0,0,0,0 };
    SystemParametersInfo(SPI_GETWORKAREA, 0, &rect, 0);
    const uint32_t window_width  = rect.right;
    const uint32_t window_height = rect.bottom;
#else
    // TODO: Choose initial window resolution based on display resolution
    const uint32_t window_width  = 1920;
    const uint32_t window_height = 1080;
#endif
     
    ui.window = glfwCreateWindow(window_width-20, window_height, "lldbg", nullptr, nullptr);

    if (!ui.window) 
    {
        glfwTerminate();
        return {};
    }

#ifdef WIN32
    glfwMaximizeWindow(ui.window);
#endif

    ui.window_width  = (float)  window_width;
    ui.window_height = (float) window_height;

    ui.m_column_1_width  = ui.window_width * 0.15f;
    ui.m_column_2_width  = ui.window_width * 0.6f;
    ui.m_column_2_height = ui.window_height * 0.6f;
    ui.console_height    = ui.window_height * 0.4f;

    ui.vertical_split_1_position = ui.m_column_1_width;
    ui.vertical_split_1_max      = ui.vertical_split_1_position + ui.m_column_1_width * 0.5f;

    ui.vertical_split_2_position  = ui.m_column_1_width + ui.m_column_2_width;
    ui.vertical_split_2_max       = (ui.window_width - ui.vertical_split_2_position) * 0.95f;
    ui.vertical_split_2_min        = ui.vertical_split_1_position + 0.1f * window_width;


    glfwMakeContextCurrent(ui.window);
    glfwSwapInterval(0);  // Disable vsync

    const GLenum err = glewInit();
   
    if (err != GLEW_OK) 
    {
        fprintf(stderr, "GLEW Error: %s\n", glewGetErrorString(err));
        return {};
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable keyboard controls

    // Customize style here :

    io.Fonts->AddFontDefault();
    static const std::string font_path = fmt::format("{}/ttf/Hack-Regular.ttf", LLDBG_ASSETS_DIR);
    ui.font = io.Fonts->AddFontFromFileTTF(font_path.c_str(), 15.0f);

    ImGui::StyleColorsDark();

#if 0
    ImGuiStyle& style = ImGui::GetStyle();

    // Fast mode - all rounding off.
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.TabRounding = 0.0f;
#endif

    ImGui_ImplGlfw_InitForOpenGL(ui.window, true);
    ImGui_ImplOpenGL3_Init();

    return ui;
}


Application::Application(const User_Interface &ui_, std::optional<fs::path> work_dir)
    : m_debugger(lldb::SBDebugger::Create()),
      listener(m_debugger.GetListener()),
      m_cmd(m_debugger),
      _stdout(StreamBuffer::StreamSource::StdOut),
      _stderr(StreamBuffer::StreamSource::StdErr),
      m_file_browser(FileBrowserNode::create(work_dir)),
      ui(ui_)
{
    Config_Load();
}


Application::~Application()
{
    if (auto process = find_process(m_debugger); process.has_value() && process->IsValid()) 
    {
        LOG(Warning) << "Found active process while closing application.";
        kill_process(*process);
    }

    if (auto target = find_target(m_debugger); target.has_value() && target->IsValid()) 
    {
        LOG(Warning) << "Found active target while closing application.";
        m_debugger.DeleteTarget(*target);
    }

    if (m_debugger.IsValid()) 
    {
        m_debugger.Clear();
        lldb::SBDebugger::Destroy(m_debugger);
        __debugger_live = false;
    }
    else 
    {
        LOG(Warning) << "Found invalid lldb::SBDebugger while closing application.";
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(ui.window);
    glfwTerminate();
}


int main_loop(Application &app)
{
    while (!glfwWindowShouldClose(app.ui.window)) 
    {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        tick(app);

        ImGui::Render();

        glViewport(0, 0, (GLsizei) app.ui.window_width, (GLsizei) app.ui.window_height);
        static const ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glFinish();

        glfwSwapBuffers(app.ui.window);

        // Poll standard streams for updates every so often.

        if (app.ui.frames_rendered % 10 == 0) 
        {
            if (auto process = find_process(app.m_debugger); process.has_value()) 
            {
                app._stdout.update(*process);
                app._stderr.update(*process);
            }

             auto target = find_target(app.m_debugger);
             static bool __custom_config_initialized = false;

             // Inject any custom target available commands here ...

             if (!__custom_config_initialized && target.has_value())
             {
                  // Drop in custom command pp used to inspect web assembly pointers.
                  run_lldb_command(app.m_debugger, app.m_cmd, app.listener, "command regex pp 's/(.+)/p __vmctx->set(),%1/'");
                  __custom_config_initialized = true;
             }

        }

        update_window_dimensions(app.ui);

        app.fps_timer.wait_for_frame_duration(int(1.75 * 16666));
        app.fps_timer.frame_end();
        app.ui.frames_rendered++;
    }

    app.Config_Save();
   
    return EXIT_SUCCESS;
}


#ifdef _WIN32
HWND GetNativeWindow()
{
   ImGuiViewport* viewport = ImGui::GetMainViewport();
   return (HWND)viewport->PlatformHandleRaw;
}
#endif


void BringWindowToTop()
{
#ifdef _WIN32
   HWND hwnd = GetNativeWindow();

   // TODO: May need AllowSetForegroundWindow(lldbg_process_id) in debuggee somehow.
   ::SetForegroundWindow(hwnd);
#endif
}
