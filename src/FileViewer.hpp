#pragma once

#define LLDBG_ADVANCED_EDITOR 1

#include <optional>
#include <string>
#include <vector>

#include "FileSystem.hpp"

#if LLDBG_ADVANCED_EDITOR
#include ".\widget\ImGuiColorTextEdit\TextEditor.h"
#endif

typedef std::pair<std::string, uint32_t> Breakpoint;
typedef std::list<Breakpoint> Breakpoints;

// Caches required information from lldb::SBFrame
struct Stack_Frame 
{
    FileHandle file_handle;
    std::string function_name;
    int line;
    int column;

    Stack_Frame(FileHandle _file_handle, int _line, int _column, std::string&& _function_name)
        : file_handle(_file_handle),
          function_name(std::move(_function_name)),
          line(_line),
          column(_column)
    {
    }

public:
    static Stack_Frame * create(lldb::SBFrame frame);   
};

typedef std::list<Stack_Frame *> Stack_Frames;


class FileViewer
{
public:

#if LLDBG_ADVANCED_EDITOR
    FileViewer()  { m_editor = nullptr; m_consumed_last_click = -1.0f; }
    ~FileViewer() { if (m_editor) delete m_editor; }
#endif

    void Show(FileHandle handle);
    std::optional<int> Render(lldb::SBTarget target, const Stack_Frames &stack_frames);
    void Optimize_Breakpoints(lldb::SBTarget target);

    inline void set_highlight_line(int line)
    {
        m_highlighted_line = line;
        m_highlight_line_needs_focus = true;
    }

    inline void unset_highlight_line()
    {
        m_highlighted_line = {};
        m_highlight_line_needs_focus = false;
    }

    std::string m_path;

private:
    std::vector<std::string> m_lines;

    std::optional<int> m_highlighted_line = {};
    bool m_highlight_line_needs_focus = false;

#if LLDBG_ADVANCED_EDITOR
    float m_consumed_last_click;
    TextEditor *m_editor;
#endif
};

