#pragma once

// INF_PATCH
#pragma warning(disable: 4251)

#include <lldb/API/LLDB.h>

#include <cassert>
#include <iostream>

#include "FPSTimer.hpp"
#include "FileSystem.hpp"
#include "FileViewer.hpp"
#include "LLDBCommandLine.hpp"
#include "Log.hpp"
#include "StreamBuffer.hpp"

// clang-format off
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl2.h"
// clang-format on

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <list>

struct User_Interface 
{
    uint32_t viewed_thread_index = 0;
    uint32_t viewed_frame_index = 0;
    uint32_t viewed_breakpoint_index = 0;

    float window_width = -1.f;   // in pixels
    float window_height = -1.f;  // in pixels

    float vertical_split_1_position = -1.f;
    float vertical_split_1_max = -1.f;

    float vertical_split_2_position = -1.f;
    float vertical_split_2_max = -1.f;
    float vertical_split_2_min = -1.f;

    float m_column_1_width = -1.f;
    float m_column_2_width = -1.f;
    float m_column_2_height = -1.f;
    float console_height = -1.f;

    bool request_manual_tab_change = false;
    bool ran_command_last_frame = false;
    bool window_resized_last_frame = false;

    size_t frames_rendered = 0;

    ImFont* font = nullptr;
    GLFWwindow* window = nullptr;

    static std::optional<User_Interface> init(void);

private:
    User_Interface() = default;
};


// Caches values that need to be evaluated to resolve.
typedef struct 
{
   size_t m_virtual_addr;
   std::string   m_value_str;
   lldb::SBValue m_value;
} Virtual_Value;

typedef std::list<Virtual_Value> Virtual_Values;



struct Thread
{
   Thread(uint32_t id, const char *name) { m_id = id; m_name = name ? strdup(name) : nullptr; } 
   ~Thread() { if (m_name) free(m_name); }

   uint32_t m_id;
   char *m_name;
};


class Launch_Target
{
public:
   Launch_Target() { change_target_active = false; m_current = false; }
   std::string cmd;
   std::string args;
   std::list<std::string> m_open_files;
   bool change_target_active;
   bool m_current;
};


struct Application 
{
    Application(const User_Interface &, std::optional<fs::path>);
    ~Application();

    Application() = delete;
    Application(const Application &) = delete;
    Application& operator=(const Application &) = delete;
    Application& operator=(Application &&) = delete;
    void Handle_Events();
    void State_Sync(std::optional<lldb::SBProcess> process, lldb::SBThread &th);
    void Config_Save();
    void Config_Load();
    uint32_t Breakpoint_Locate(lldb::SBTarget target, const char *file, uint32_t line);

    lldb::SBValueList m_locals;
    lldb::SBValueList m_globals;
    lldb::SBDebugger m_debugger;
    lldb::SBListener listener;
    LLDBCommandLine m_cmd;
    StreamBuffer _stdout;
    StreamBuffer _stderr;

    OpenFiles m_open_files;
    std::unique_ptr<FileBrowserNode> m_file_browser;
    User_Interface ui;
    FileViewer file_viewer;
    FPSTimer fps_timer;

    Virtual_Values m_evaluated_locals;
    Virtual_Values m_evaluated_globals;

   // Breakpoints m_pending_breakpoints;

    Stack_Frames m_frames;
    std::list<Thread*>       m_threads;

    std::list<Launch_Target *> m_launch_configs;

   void Display_Variables_Registers(float stack_height);
};

int main_loop(Application& app);

lldb::SBCommandReturnObject run_lldb_command(Application& app, const char* command,
                                             bool hide_from_history = false);

