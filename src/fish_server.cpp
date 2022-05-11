//
// Alternate fish main loop, for using interactively by another process.
/*
Copyright (C) 2005-2008 Axel Liljencrantz

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/
#include <ext/alloc_traits.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "builtin.h"
#include "common.h"
#include "env.h"
#include "event.h"
#include "fallback.h"  // IWYU pragma: keep
#include "flog.h"
#include "future_feature_flags.h"
#include "history.h"
#include "io.h"
#include "maybe.h"
#include "parse_constants.h"
#include "parse_tree.h"
#include "parse_util.h"
#include "parser.h"
#include "path.h"
#include "proc.h"
#include "signal.h"
#include "wchar.h"
#include "wcstringutil.h"
#include "wutil.h"  // IWYU pragma: keep


static bool has_suffix(const std::string &path, const char *suffix, bool ignore_case) {
    size_t pathlen = path.size(), suffixlen = std::strlen(suffix);
    return pathlen >= suffixlen &&
           !(ignore_case ? strcasecmp : std::strcmp)(path.c_str() + pathlen - suffixlen, suffix);
}

/// Modifies the given path by calling realpath. Returns true if realpath succeeded, false
/// otherwise.
static bool get_realpath(std::string &path) {
    char buff[PATH_MAX], *ptr;
    if ((ptr = realpath(path.c_str(), buff))) {
        path = ptr;
    }
    return ptr != nullptr;
}

static struct config_paths_t determine_config_directory_paths(const char *argv0) {
    struct config_paths_t paths;
    bool done = false;
    std::string exec_path = get_executable_path(argv0);
    if (get_realpath(exec_path)) {
        FLOGF(config, L"exec_path: '%s', argv[0]: '%s'", exec_path.c_str(), argv0);
        // TODO: we should determine program_name from argv0 somewhere in this file

#ifdef CMAKE_BINARY_DIR
        // Detect if we're running right out of the CMAKE build directory
        if (string_prefixes_string(CMAKE_BINARY_DIR, exec_path.c_str())) {
            FLOGF(config,
                  "Running out of build directory, using paths relative to CMAKE_SOURCE_DIR:\n %s",
                  CMAKE_SOURCE_DIR);

            done = true;
            paths.data = wcstring{L"" CMAKE_SOURCE_DIR} + L"/share";
            paths.sysconf = wcstring{L"" CMAKE_SOURCE_DIR} + L"/etc";
            paths.doc = wcstring{L"" CMAKE_SOURCE_DIR} + L"/user_doc/html";
            paths.bin = wcstring{L"" CMAKE_BINARY_DIR};
        }
#endif

        if (!done) {
            // The next check is that we are in a reloctable directory tree
            const char *installed_suffix = "/bin/fish";
            const char *just_a_fish = "/fish";
            const char *suffix = nullptr;

            if (has_suffix(exec_path, installed_suffix, false)) {
                suffix = installed_suffix;
            } else if (has_suffix(exec_path, just_a_fish, false)) {
                FLOGF(config, L"'fish' not in a 'bin/', trying paths relative to source tree");
                suffix = just_a_fish;
            }

            if (suffix) {
                bool seems_installed = (suffix == installed_suffix);

                wcstring base_path = str2wcstring(exec_path);
                base_path.resize(base_path.size() - std::strlen(suffix));

                paths.data = base_path + (seems_installed ? L"/share/fish" : L"/share");
                paths.sysconf = base_path + (seems_installed ? L"/etc/fish" : L"/etc");
                paths.doc = base_path + (seems_installed ? L"/share/doc/fish" : L"/user_doc/html");
                paths.bin = base_path + (seems_installed ? L"/bin" : L"");

                // Check only that the data and sysconf directories exist. Handle the doc
                // directories separately.
                struct stat buf;
                if (0 == wstat(paths.data, &buf) && 0 == wstat(paths.sysconf, &buf)) {
                    // The docs dir may not exist; in that case fall back to the compiled in path.
                    if (0 != wstat(paths.doc, &buf)) {
                        paths.doc = L"" DOCDIR;
                    }
                    done = true;
                }
            }
        }
    }

    if (!done) {
        // Fall back to what got compiled in.
        FLOGF(config, L"Using compiled in paths:");
        paths.data = L"" DATADIR "/fish";
        paths.sysconf = L"" SYSCONFDIR "/fish";
        paths.doc = L"" DOCDIR;
        paths.bin = L"" BINDIR;
    }

    FLOGF(config,
          L"determine_config_directory_paths() results:\npaths.data: %ls\npaths.sysconf: "
          L"%ls\npaths.doc: %ls\npaths.bin: %ls",
          paths.data.c_str(), paths.sysconf.c_str(), paths.doc.c_str(), paths.bin.c_str());
    return paths;
}

// Source the file config.fish in the given directory.
static void source_config_in_directory(parser_t &parser, const wcstring &dir) {
    // If the config.fish file doesn't exist or isn't readable silently return. Fish versions up
    // thru 2.2.0 would instead try to source the file with stderr redirected to /dev/null to deal
    // with that possibility.
    //
    // This introduces a race condition since the readability of the file can change between this
    // test and the execution of the 'source' command. However, that is not a security problem in
    // this context so we ignore it.
    const wcstring config_pathname = dir + L"/config.fish";
    const wcstring escaped_dir = escape_string(dir, ESCAPE_ALL);
    const wcstring escaped_pathname = escaped_dir + L"/config.fish";
    if (waccess(config_pathname, R_OK) != 0) {
        FLOGF(config, L"not sourcing %ls (not readable or does not exist)",
              escaped_pathname.c_str());
        return;
    }
    FLOGF(config, L"sourcing %ls", escaped_pathname.c_str());

    const wcstring cmd = L"builtin source " + escaped_pathname;
    set_is_within_fish_initialization(true);
    parser.eval(cmd, io_chain_t());
    set_is_within_fish_initialization(false);
}

/// Parse init files. exec_path is the path of fish executable as determined by argv[0].
static void read_init(parser_t &parser, const struct config_paths_t &paths) {
    source_config_in_directory(parser, paths.data);
    source_config_in_directory(parser, paths.sysconf);

    // We need to get the configuration directory before we can source the user configuration file.
    // If path_get_config returns false then we have no configuration directory and no custom config
    // to load.
    wcstring config_dir;
    if (path_get_config(config_dir)) {
        source_config_in_directory(parser, config_dir);
    }
}

int try_open(int fd, const std::string& path, int flags) {
    if (fd > 2) {
        close(fd);
    }
    if (path.empty()) {
        return -1;
    }
    return open(path.c_str(), flags | O_CLOEXEC);
}

// Copied from escape_string_pcre2, changing escaped chars. Obviously super insecure
// The actual RFC is at https://www.ietf.org/rfc/rfc4627.txt (Section 2.5)
wcstring escape_string_json(const wcstring& in) {
    wcstring out;
    out.reserve(in.size() * 1.3);  // a wild guess
    wchar_t hex_buf[7]; // \uXXXX is 6 chars, plus NUL

    for (auto c : in) {
        switch (c) {
            case L'\b':
                out.append(L"\\b");
                break;
            case L'\r':
                out.append(L"\\r");
                break;
            case L'\n':
                out.append(L"\\n");
                break;
            case L'\t':
                out.append(L"\\t");
                break;
            case L'\f':
                out.append(L"\\f");
                break;
            case L'\\':
            case L'"':
                out.push_back('\\');
                /* FALLTHROUGH */
            default:
                if (c < ' ') {
                    swprintf(hex_buf, 7, L"\\u%4x", c);
                    out.append(hex_buf);
                } else {
                    out.push_back(c);
                }
        }
    }

    return out;
}

//void server_send_msg(bool done, int exit, const wcstring& dir) {
//    // Write directly to stdout, foregoing the FILE* (We could also disable buffering on stdout)
//    dprintf(STDOUT_FILENO, "{\"Done\": %s, \"Exit\": %d, \"Dir\": \"%ls\"}\n",
//            done ? "true" : "false", exit, escape_string_json(dir).c_str());
//}

int server_read_loop(parser_t &parser) {
    // TODO: Don't use iostream. See wcstringutil for internal versions of string functions
    // In particular, wcstring_tok() looks similar
    const std::string whitespace = " \t\n\r";
    int in_fd = 0, out_fd = 1, err_fd = 2;
    // Set up dummy redirections for now. We'll overwrite the vector when we run the "stdio" method.
    // TODO: Doesn't apply in the preexec/postexec events :(
    io_chain_t redirections {
        std::make_shared<io_fd_t>(STDIN_FILENO, STDERR_FILENO),
        std::make_shared<io_fd_t>(STDOUT_FILENO, STDERR_FILENO),
        std::make_shared<io_fd_t>(STDERR_FILENO, STDERR_FILENO)
    };
    // Things that still get sent to stderr:
    // - Dynamic parse errors like "unknown command". Could probably still look them up?
    // - time command (timer.cpp#L208)

    std::string message;
    // Read entire message until NUL
    while (std::getline(std::cin, message, '\0').good()) {
        std::string::size_type pos = 0;
        // Move ahead to first token
        pos = message.find_first_not_of(whitespace, pos);
        // Returns an empty string if there are no more tokens.
        auto next_token = [&] () -> std::string {
            if (pos == std::string::npos) {
                return {};
            }
            auto start = pos;
            pos = message.find_first_of(whitespace, pos);
            auto len = (pos == std::string::npos) ? std::string::npos : pos-start;
            // Move ahead to next token
            if (pos != std::string::npos) {
                pos = message.find_first_not_of(whitespace, pos);
            }
            return message.substr(start, len);
        };

        std::string method = next_token();

        if (method == "stdio") {
            std::string in, out, err;
            in = next_token();
            out = next_token();
            err = next_token();
            in_fd = try_open(in_fd, in, O_RDONLY);
            out_fd = try_open(out_fd, out, O_WRONLY);
            err_fd = try_open(err_fd, err, O_WRONLY);
            redirections[0] = std::make_shared<io_fd_t>(STDIN_FILENO, in_fd);
            redirections[1] = std::make_shared<io_fd_t>(STDOUT_FILENO, out_fd);
            redirections[2] = std::make_shared<io_fd_t>(STDERR_FILENO, err_fd);
        } else if (method == "run") {
            // TODO: read_i() does some history manipulation that we need
            std::string cmdstr;
            if (pos != std::string::npos) {
                cmdstr = message.substr(pos);
            }

            parse_error_list_t errors;
            auto src = parse_source(str2wcstring(cmdstr), parse_flag_none, &errors);
            if (src != nullptr && parse_util_detect_errors(src->ast, src->src, &errors)) {
                src = nullptr;
            }
            // TODO: Use src==nullptr and parser.get_backtrace?
            if (!errors.empty()) {
                // TODO: return error message directly along with Done: true
                dprintf(STDOUT_FILENO, "{\"Done\": true, \"Exit\": %d}\n", STATUS_ILLEGAL_CMD);
                fprintf(stderr, "Parse errors exist: %s\n", cmdstr.c_str());
                for (const auto& e : errors) {
                    fprintf(stderr, "error: %ls\n", e.text.c_str());
                }
                continue;
            }

            // TODO: Cancel eval on SIGINT (Could still have a long-running loop: `for true; end`)
            //       Or do something crazy like run the parser in its own thread?

            event_fire_generic(parser, L"fish_preexec");
            auto result = parser.eval(src, redirections);
            job_reap(parser, true);
            // TODO: Find out what new jobs were created

            bool exit = parser.libdata().exit_current_script;
            parser.libdata().exit_current_script = false;

            int status = result.status.status_value();
            // TODO: Reset status before next command? ("set" doesn't seem to update status)
            event_fire_generic(parser, L"fish_postexec");

            // Could also call builtin_pwd
            wcstring dir;
            if (auto tmp = parser.vars().get(L"PWD")) {
                dir = tmp->as_string();
            }
            // Write directly to stdout, foregoing the FILE* (We could also disable buffering on stdout)
            dprintf(STDOUT_FILENO, "{\"Done\": true, \"Exit\": %d, \"Dir\": \"%ls\"}\n",
                    status, escape_string_json(dir).c_str());

            if (exit) {
                return 0;
            }
        } else if (method == "exit") {
            return 0;
        } else {
            fprintf(stderr, "Unknown method: %s\n", method.c_str());
        }
    }

    return 1;
}

// Just a demo. The main function is generally the same, but I ripped out some actual shell features!
// Including:
// - All command line options
// - Features that depend on fish or env vars, like $fish_features and $FISH_DEBUG
int main(int argc, char **argv) {
    int res = 1;
    UNUSED(argc);

    program_name = L"fish";
    set_main_thread();
    setup_fork_guards();
    signal_unblock_all();
    setlocale(LC_ALL, "");

    const char *dummy_argv[2] = {"fish", nullptr};
    if (!argv[0]) {
        argv = const_cast<char **>(dummy_argv);  //!OCLINT(parameter reassignment)
        argc = 1;                                //!OCLINT(parameter reassignment)
    }


    // Apply our options.
    mark_login();
    set_interactive_session(true);

    // Only save (and therefore restore) the fg process group if we are interactive. See issues
    // #197 and #1002.
    if (is_interactive_session()) {
        save_term_foreground_process_group();
    }

    struct config_paths_t paths;
    paths = determine_config_directory_paths(argv[0]);
    env_init(&paths);

    proc_init();
    builtin_init();
    misc_init();

    parser_t &parser = parser_t::principal_parser();

    read_init(parser, paths);

    parser.libdata().is_interactive = true;
    // Not really confident this works. SIGINTs still seem to end the program.
    signal_set_handlers(true);
    // ---------------------------------------
    // THIS IS THE MAIN INTERACTIVE LOOP
    res = server_read_loop(parser);
    // ---------------------------------------
    event_fire_generic(parser, L"fish_exit");

    int exit_status = res ? STATUS_CMD_UNKNOWN : parser.get_last_status();

    event_fire(parser,
               proc_create_event(L"PROCESS_EXIT", event_type_t::exit, getpid(), exit_status));

    // Trigger any exit handlers.
    wcstring_list_t event_args = {to_string(exit_status)};
    event_fire_generic(parser, L"fish_exit", &event_args);

    history_save_all();

    // We don't need to use the last exit status from the parser.
    exit_without_destructors(0);
    return EXIT_FAILURE;  // above line should always exit
}
