#define _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING 1

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "Application.h"
#include "Defer.hpp"
#include "FileSystem.hpp"
#include "Log.hpp"
#include "StringBuffer.hpp"
#include "Timer.hpp"
#include "cxxopts.hpp"
#include "fmt/format.h"
#include "imgui.h"
#include "lldb/API/LLDB.h"


bool __debugger_live = false;

void zap_debugger()
{
   if (__debugger_live)
   {
      lldb::SBDebugger::Terminate();
      __debugger_live = false;
   }
}


int main(int argc, char** argv)
{
    // TODO: Add version number to description here
    cxxopts::Options options("lldbg", "LLVM Debugger GUI.");
    options.positional_help("");

    // TODO: Warn on mutually exclusive arguments, for example attach vs. launch file.
    // clang-format off
    options.add_options()
        ("n,attach-name", "Attach to a process with the given name.", cxxopts::value<std::string>())
        ("p,attach-pid", "Attach to a process with the given pid.", cxxopts::value<std::string>())
        ("w,wait-for", "Wait for a process with the given pid or name to launch before attaching.", cxxopts::value<std::string>())
        ("f,file", "Use <filename> as the program to be debugged.", cxxopts::value<std::string>())
        ("S,source-before-file", "Read and execute the lldb  commands  in the given file, before any file has been loaded.", cxxopts::value<std::string>())
        ("s,source", "Read and execute the lldb commands in the given file, after any file has been loaded.", cxxopts::value<std::string>())
        ("workdir", "Specify base directory of file explorer tree", cxxopts::value<std::string>())
        ("h,help", "Print out usage information.")
        ("positional", "Positional arguments: these are the arguments that are entered without an option", cxxopts::value<std::vector<std::string>>())
        ;
    // clang-format on

    options.parse_positional({"file", "positional"});

    auto result = options.parse(argc, argv);

    if (result.count("help")) 
    {
        std::cout << options.help() << std::endl;
        return EXIT_SUCCESS;
    }

    auto lldb_error = lldb::SBDebugger::InitializeWithErrorHandling();

    if (lldb_error.Fail()) 
    {
        const char* lldb_error_cstr = lldb_error.GetCString();
        std::cerr << (lldb_error_cstr ? lldb_error_cstr : "Unknown LLDB error!");
        std::cerr << "Failed to initialize LLDB, exiting...";
        return EXIT_FAILURE;
    }

    __debugger_live = true;
    Defer(zap_debugger());

    auto ui = User_Interface::init();
    if (!ui.has_value()) 
    {
        LOG(Error) << "Failed to initialize graphics/UI.\n Exiting...";
        return EXIT_FAILURE;
    }

    std::optional<fs::path> workdir = {};
    if (result.count("workdir")) 
    {
        fs::path workdir_request = fs::path(result["workdir"].as<std::string>());
        if (fs::exists(workdir_request) && fs::is_directory(workdir_request)) 
        {
            workdir = workdir_request;
        }
    }

    Application app(*ui, workdir);

    if (result.count("source-before-file")) 
    {
        const std::string source_path = result["source-before-file"].as<std::string>();
        auto handle = FileHandle::create(source_path);

        if (handle.has_value()) 
        {
            for (const std::string& line : handle->contents()) 
            {
                auto ret = run_lldb_command(app, line.c_str());
            }
            LOG(Verbose) << "Successfully executed commands in source file: " << source_path;
        }
        else 
        {
            LOG(Error) << "Invalid filepath passed to source-before-file argument: " << source_path;
        }
    }

    if (result.count("file")) 
    {
        // TODO: Detect and open main file of specified executable
        StringBuffer target_set_cmd;
        target_set_cmd.format("file {}", result["file"].as<std::string>());
        run_lldb_command(app, target_set_cmd.data());

        if (result.count("positional")) 
        {
            StringBuffer argset_command;
            argset_command.format_("settings set target.run-args ");
            for (const auto& arg : result["positional"].as<std::vector<std::string>>()) 
            {
                argset_command.format_("{} ", arg);
            }

            argset_command.format_("{}", '\0');
            run_lldb_command(app, argset_command.data());
        }
    }

    if (result.count("source")) 
    {
        const std::string source_path = result["source"].as<std::string>();
        auto handle = FileHandle::create(source_path);

        if (handle.has_value()) 
        {
            for (const std::string& line : handle->contents()) 
            {
                auto ret = run_lldb_command(app, line.c_str());
            }
            LOG(Verbose) << "Successfully executed commands in source file: " << source_path;
        }
        else 
        {
            LOG(Error) << "Invalid filepath passed to --source argument: " << source_path;
        }
    }

    return main_loop(app);
}