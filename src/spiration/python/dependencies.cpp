/**
 * @file dependencies.cpp
 * @brief 脚本拓展的 PyPI 依赖管理。
 * @author 陈林锴
 *
 * 拓展可在清单中声明 Python 依赖（PEP 508 规格，必须带版本约束）：
 *
 * ```json
 * {
 *   "python": {
 *     "requirements": ["openai>=3.37,<4"]
 *   }
 * }
 * ```
 *
 * 加载器在**导入脚本之前**保证这些依赖可用：
 *
 * 1. 依赖安装在拓展私有数据目录的 `python/site-packages` 下
 *    （`host->data_path("python")`，即应用数据目录内、可写且随版本保留）；
 * 2. 首次装载或需求变化时，用**同一套 CPython** 执行
 *    `python -m pip install --target <site-packages> -r <requirements>`；
 * 3. 安装成功后把解释器版本与需求写入 `python/state.txt`，下次装载直接复用；
 * 4. 安装期间释放 GIL，子进程隐藏窗口，超时可配置。
 *
 * 设计约束：
 * - 只接受“包名 + 版本约束 (+ 可选 extras/环境标记)”的普通规格，拒绝以 `-`
 *   开头的 pip 选项、URL、本地路径，避免清单变成任意命令/文件注入；
 * - 必须带版本约束，与项目的依赖引入规范一致；
 * - 安装失败即拒绝装载拓展，绝不带着缺失依赖继续导入脚本。
 *
 * @par 可用的环境变量
 * - `SPIRATION_PYTHON_DISABLE_PIP`：非空且非 `0` 时跳过自动安装，假定依赖已由
 *   部署方预置（离线/打包场景）；加载器仍会注入依赖目录。
 * - `SPIRATION_PYTHON_PIP_TIMEOUT`：pip 超时秒数，缺省 300。
 */

#include "internal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace spiration {
namespace python {
namespace loader {

namespace {

/** @brief 依赖目录在拓展数据目录下的相对路径。 */
const char* const kDependencyDirectory = "python";
/** @brief 站点包目录名。 */
const char* const kSitePackages = "site-packages";
/** @brief 依赖状态文件（解释器版本 + 需求列表）。 */
const char* const kStateFile = "state.txt";
/** @brief 传给 pip 的需求文件。 */
const char* const kRequirementsFile = "requirements.txt";
/** @brief pip 输出日志。 */
const char* const kPipLogFile = "pip.log";
/** @brief 默认 pip 超时秒数。 */
const int kDefaultPipTimeoutSeconds = 300;
/** @brief 错误信息中保留的 pip 输出字节数。 */
const size_t kPipLogTailBytes = 4000;
/** @brief 单条需求的长度上限。 */
const size_t kMaxRequirementLength = 512;

/* -------------------------------------------------------------------------
 * 基础工具
 * ---------------------------------------------------------------------- */

/** @brief 读取环境变量。 */
std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

/** @brief 环境变量是否为真（非空且不为 "0"）。 */
bool environment_flag(const char* name) {
    const std::string value = environment(name);
    return !value.empty() && value != "0";
}

/** @brief 拼接路径（保留调用方分隔符风格）。 */
std::string join(const std::string& base, const std::string& leaf) {
    if (base.empty()) return leaf;
    if (leaf.empty()) return base;
    const char last = base[base.size() - 1];
    if (last == '/' || last == '\\') return base + leaf;
#if defined(_WIN32)
    return base + "\\" + leaf;
#else
    return base + "/" + leaf;
#endif
}

/** @brief 读取文本文件尾部（用于展示 pip 失败原因）。 */
bool read_file_tail(const std::string& path, size_t max_bytes,
                    std::string* out) {
    out->clear();
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;

    if (std::fseek(file, 0, SEEK_END) == 0) {
        const long length = std::ftell(file);
        if (length > 0) {
            long start = static_cast<long>(max_bytes);
            if (start > length) start = length;
            if (std::fseek(file, length - start, SEEK_SET) == 0) {
                std::string text(static_cast<size_t>(start), '\0');
                const size_t read =
                    std::fread(&text[0], 1, text.size(), file);
                text.resize(read);
                *out = std::move(text);
            }
        }
    }
    std::fclose(file);
    return true;
}

/* -------------------------------------------------------------------------
 * 需求清单解析（需持有 GIL）
 * ---------------------------------------------------------------------- */

/** @brief 判断规格是否包含版本约束操作符。 */
bool has_version_constraint(const std::string& spec) {
    static const char* const operators[] = {"===", "==", "~=", "!=",
                                            ">=",  "<=", ">",  "<"};
    for (size_t i = 0; i < sizeof(operators) / sizeof(operators[0]); ++i) {
        if (spec.find(operators[i]) != std::string::npos) return true;
    }
    return false;
}

/**
 * @brief 校验一条依赖规格。
 * @return 合法返回 `true`；否则返回 `false` 并写入原因。
 */
bool validate_requirement(const std::string& spec, std::string* error) {
    if (spec.empty()) {
        *error = "empty requirement";
        return false;
    }
    if (spec.size() > kMaxRequirementLength) {
        *error = "requirement is too long: " + spec.substr(0, 64) + "...";
        return false;
    }
    if (spec[0] == '-' || spec.find("://") != std::string::npos ||
        spec.find("file:") != std::string::npos ||
        spec.find("--") != std::string::npos) {
        *error = "requirement must be a plain PEP 508 specifier, got: " + spec;
        return false;
    }
    if (spec[0] == '.' || spec[0] == '/' || spec[0] == '\\' ||
        spec.find("..") != std::string::npos) {
        *error = "requirement must not reference a local path: " + spec;
        return false;
    }
    if (spec.find('\n') != std::string::npos ||
        spec.find('\r') != std::string::npos) {
        *error = "requirement must not contain newlines";
        return false;
    }
    if (!has_version_constraint(spec)) {
        *error = "requirement must pin a version, for example "
                 "\"openai>=3.37,<4\": " +
                 spec;
        return false;
    }
    return true;
}

/**
 * @brief 从清单原文读取 `python.requirements`（需持有 GIL）。
 * @param script 脚本状态。
 * @param out    输出需求列表；清单未声明时为空。
 * @param error  失败原因。
 */
bool read_requirements(script_state* script, std::vector<std::string>* out,
                       std::string* error) {
    out->clear();
    if (script == nullptr) return true;

    PyObject* json = PyImport_ImportModule("json");
    if (json == nullptr) {
        PyErr_Clear();
        *error = "cannot import json to parse the extension manifest";
        return false;
    }
    PyObject* document = PyObject_CallMethod(
        json, "loads", "s#", script->manifest.data(),
        static_cast<Py_ssize_t>(script->manifest.size()));
    Py_DECREF(json);
    if (document == nullptr) {
        report_python_error(script, "manifest");
        *error = "extension manifest is not valid JSON";
        return false;
    }
    if (!PyDict_Check(document)) {
        Py_DECREF(document);
        *error = "extension manifest must be a JSON object";
        return false;
    }

    PyObject* python = PyDict_GetItemString(document, "python"); /* 借用引用 */
    if (python == nullptr || python == Py_None) {
        Py_DECREF(document);
        return true;
    }
    if (!PyDict_Check(python)) {
        Py_DECREF(document);
        *error = "\"python\" in the manifest must be an object";
        return false;
    }

    PyObject* requirements =
        PyDict_GetItemString(python, "requirements"); /* 借用引用 */
    if (requirements == nullptr || requirements == Py_None) {
        Py_DECREF(document);
        return true;
    }

    PyObject* sequence =
        PySequence_Fast(requirements, "\"python.requirements\" must be a list");
    if (sequence == nullptr) {
        PyErr_Clear();
        Py_DECREF(document);
        *error = "\"python.requirements\" must be a list";
        return false;
    }

    const Py_ssize_t count = PySequence_Fast_GET_SIZE(sequence);
    for (Py_ssize_t i = 0; i < count; ++i) {
        PyObject* item = PySequence_Fast_GET_ITEM(sequence, i); /* 借用引用 */
        const char* text = item != nullptr ? PyUnicode_AsUTF8(item) : nullptr;
        if (text == nullptr) {
            PyErr_Clear();
            Py_DECREF(sequence);
            Py_DECREF(document);
            *error =
                "\"python.requirements\" must contain strings";
            return false;
        }
        std::string spec(text);
        std::string reason;
        if (!validate_requirement(spec, &reason)) {
            Py_DECREF(sequence);
            Py_DECREF(document);
            *error = reason;
            return false;
        }
        out->push_back(std::move(spec));
    }

    Py_DECREF(sequence);
    Py_DECREF(document);
    return true;
}

/** @brief 序列化为 pip 需求文件内容。 */
std::string requirements_text(const std::vector<std::string>& requirements) {
    std::string text;
    for (size_t i = 0; i < requirements.size(); ++i) {
        text += requirements[i];
        text += '\n';
    }
    return text;
}

/**
 * @brief 依赖状态文本：解释器版本、架构与需求全相同才视为已安装。
 */
std::string dependency_state(const std::vector<std::string>& requirements) {
    std::string text = "python ";
    text += SPIRATION_PYTHON_VERSION_STRING;
    text += " (";
    text += std::to_string(sizeof(void*) * 8);
    text += "-bit)\n";
    text += requirements_text(requirements);
    return text;
}

/* -------------------------------------------------------------------------
 * Python 辅助（以下函数均需持有 GIL）
 * ---------------------------------------------------------------------- */

/** @brief 读取 UTF-8 文本文件；失败返回 `false`。 */
bool read_text_file(const std::string& path, std::string* out) {
    out->clear();

    PyObject* builtins = PyImport_ImportModule("builtins");
    if (builtins == nullptr) {
        PyErr_Clear();
        return false;
    }
    PyObject* open_fn = PyObject_GetAttrString(builtins, "open");
    Py_DECREF(builtins);
    if (open_fn == nullptr) {
        PyErr_Clear();
        return false;
    }

    PyObject* file = PyObject_CallFunction(open_fn, "ss", path.c_str(), "r");
    Py_DECREF(open_fn);
    if (file == nullptr) {
        PyErr_Clear();
        return false;
    }

    PyObject* text = PyObject_CallMethod(file, "read", nullptr);
    PyObject* closed = PyObject_CallMethod(file, "close", nullptr);
    Py_XDECREF(closed);
    Py_DECREF(file);
    if (text == nullptr) {
        PyErr_Clear();
        return false;
    }

    Py_ssize_t size = 0;
    const char* utf8 = PyUnicode_AsUTF8AndSize(text, &size);
    if (utf8 != nullptr) out->assign(utf8, static_cast<size_t>(size));
    Py_DECREF(text);
    if (utf8 == nullptr) {
        PyErr_Clear();
        return false;
    }
    return true;
}

/** @brief 写入 UTF-8 文本文件（自动创建父目录）；失败返回 `false`。 */
bool write_text_file(const std::string& directory, const std::string& path,
                     const std::string& text) {
    PyObject* os = PyImport_ImportModule("os");
    if (os == nullptr) {
        PyErr_Clear();
        return false;
    }
    PyObject* makedirs = PyObject_GetAttrString(os, "makedirs");
    Py_DECREF(os);
    if (makedirs == nullptr) {
        PyErr_Clear();
        return false;
    }
    PyObject* arguments = Py_BuildValue("(s)", directory.c_str());
    PyObject* keywords = Py_BuildValue("{s:O}", "exist_ok", Py_True);
    PyObject* result = PyObject_Call(makedirs, arguments, keywords);
    Py_DECREF(makedirs);
    Py_DECREF(arguments);
    Py_DECREF(keywords);
    if (result == nullptr) {
        PyErr_Clear();
        return false;
    }
    Py_DECREF(result);

    PyObject* builtins = PyImport_ImportModule("builtins");
    if (builtins == nullptr) {
        PyErr_Clear();
        return false;
    }
    PyObject* open_fn = PyObject_GetAttrString(builtins, "open");
    Py_DECREF(builtins);
    if (open_fn == nullptr) {
        PyErr_Clear();
        return false;
    }
    PyObject* open_arguments = Py_BuildValue("(ss)", path.c_str(), "w");
    PyObject* open_keywords = Py_BuildValue("{s:s}", "encoding", "utf-8");
    PyObject* file = PyObject_Call(open_fn, open_arguments, open_keywords);
    Py_DECREF(open_fn);
    Py_DECREF(open_arguments);
    Py_DECREF(open_keywords);
    if (file == nullptr) {
        PyErr_Clear();
        return false;
    }

    PyObject* written = PyObject_CallMethod(
        file, "write", "s#", text.data(), static_cast<Py_ssize_t>(text.size()));
    PyObject* closed = PyObject_CallMethod(file, "close", nullptr);
    Py_XDECREF(closed);
    Py_DECREF(file);
    if (written == nullptr) {
        PyErr_Clear();
        return false;
    }
    Py_DECREF(written);
    return true;
}

/** @brief 判断路径是否为目录。 */
bool path_is_directory(const std::string& path) {
    PyObject* os_path = PyImport_ImportModule("os.path");
    if (os_path == nullptr) {
        PyErr_Clear();
        return false;
    }
    PyObject* result =
        PyObject_CallMethod(os_path, "isdir", "s", path.c_str());
    Py_DECREF(os_path);
    if (result == nullptr) {
        PyErr_Clear();
        return false;
    }
    const int truth = PyObject_IsTrue(result);
    Py_DECREF(result);
    return truth == 1;
}

/** @brief 判断路径是否为存在的文件。 */
bool path_is_file(const std::string& path) {
    PyObject* os_path = PyImport_ImportModule("os.path");
    if (os_path == nullptr) {
        PyErr_Clear();
        return false;
    }
    PyObject* result =
        PyObject_CallMethod(os_path, "isfile", "s", path.c_str());
    Py_DECREF(os_path);
    if (result == nullptr) {
        PyErr_Clear();
        return false;
    }
    const int truth = PyObject_IsTrue(result);
    Py_DECREF(result);
    return truth == 1;
}

/** @brief 取 Python 字符串属性（失败返回空串）。 */
std::string attribute_text(PyObject* object, const char* name) {
    PyObject* value = PyObject_GetAttrString(object, name);
    if (value == nullptr) {
        PyErr_Clear();
        return std::string();
    }
    std::string text;
    if (PyUnicode_Check(value)) {
        Py_ssize_t size = 0;
        const char* utf8 = PyUnicode_AsUTF8AndSize(value, &size);
        if (utf8 != nullptr) text.assign(utf8, static_cast<size_t>(size));
        PyErr_Clear();
    }
    Py_DECREF(value);
    return text;
}

/**
 * @brief 定位用于执行 pip 的解释器可执行文件（需持有 GIL）。
 *
 * 优先使用构建/运行时确定的根目录，其次 `sys.prefix`，最后 `sys.executable`。
 */
bool python_program(script_state* script, std::string* out,
                    std::string* error) {
    std::vector<std::string> candidates;

    const std::string home = python_home();
    if (!home.empty()) {
#if defined(_WIN32)
        candidates.push_back(join(home, "python.exe"));
#else
        candidates.push_back(join(join(home, "bin"), "python3"));
        candidates.push_back(join(join(home, "bin"), "python"));
#endif
    }

    PyObject* sys = PyImport_ImportModule("sys");
    if (sys != nullptr) {
        const std::string prefix = attribute_text(sys, "prefix");
        if (!prefix.empty()) {
#if defined(_WIN32)
            candidates.push_back(join(prefix, "python.exe"));
#else
            candidates.push_back(join(join(prefix, "bin"), "python3"));
            candidates.push_back(join(join(prefix, "bin"), "python"));
#endif
        }
        const std::string executable = attribute_text(sys, "executable");
        if (!executable.empty()) candidates.push_back(executable);
        Py_DECREF(sys);
    } else {
        PyErr_Clear();
    }

    for (size_t i = 0; i < candidates.size(); ++i) {
        if (path_is_file(candidates[i])) {
            *out = candidates[i];
            return true;
        }
    }

    std::string searched;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (i != 0) searched += ", ";
        searched += candidates[i].empty() ? "(empty)" : candidates[i];
    }
    *error = "cannot locate the python executable used by the loader" +
             (searched.empty() ? std::string() : "; tried: " + searched);
    if (script != nullptr) {
        log_to(&script->desc, SPIRATION_LOG_ERROR, *error, "python/pip");
    }
    return false;
}

/* -------------------------------------------------------------------------
 * 子进程
 * ---------------------------------------------------------------------- */

#if defined(_WIN32)

/** @brief UTF-8 → UTF-16。 */
std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          &out[0], size);
    return out;
}

/** @brief 按 Windows 命令行规则为一个参数加引号。 */
std::wstring quote_argument(const std::wstring& argument) {
    if (argument.find_first_of(L" \t\"") == std::wstring::npos) return argument;
    std::wstring quoted = L"\"";
    for (size_t i = 0; i < argument.size(); ++i) {
        if (argument[i] == L'"') quoted += L'\\';
        quoted += argument[i];
    }
    quoted += L"\"";
    return quoted;
}

/**
 * @brief 隐藏窗口运行子进程，stdout/stderr 重定向到日志文件。
 * @return 成功启动返回 `true`（`exit_code` 有效）；超时在 `error` 中说明。
 */
bool run_process(const std::string& program,
                 const std::vector<std::string>& arguments,
                 const std::string& log_path, int timeout_seconds,
                 int* exit_code, std::string* error) {
    std::wstring command = quote_argument(utf8_to_wide(program));
    for (size_t i = 0; i < arguments.size(); ++i) {
        command += L' ';
        command += quote_argument(utf8_to_wide(arguments[i]));
    }
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');

    SECURITY_ATTRIBUTES attributes;
    std::memset(&attributes, 0, sizeof(attributes));
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE log_handle = ::CreateFileW(
        utf8_to_wide(log_path).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log_handle == INVALID_HANDLE_VALUE) {
        *error = "cannot create the pip log file: " + log_path;
        return false;
    }
    HANDLE input_handle =
        ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup;
    std::memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log_handle;
    startup.hStdError = log_handle;
    startup.hStdInput =
        input_handle == INVALID_HANDLE_VALUE ? nullptr : input_handle;

    PROCESS_INFORMATION process;
    std::memset(&process, 0, sizeof(process));
    const BOOL created = ::CreateProcessW(
        utf8_to_wide(program).c_str(), command_line.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);

    ::CloseHandle(log_handle);
    if (input_handle != INVALID_HANDLE_VALUE) ::CloseHandle(input_handle);

    if (!created) {
        *error = "cannot start pip (" + program + "), error " +
                 std::to_string(static_cast<int>(::GetLastError()));
        return false;
    }

    const DWORD timeout_ms =
        timeout_seconds > 0
            ? static_cast<DWORD>(timeout_seconds) * 1000u
            : INFINITE;
    const DWORD wait_result = ::WaitForSingleObject(process.hProcess, timeout_ms);
    if (wait_result == WAIT_TIMEOUT) {
        ::TerminateProcess(process.hProcess, 1);
        ::WaitForSingleObject(process.hProcess, 5000);
        *error = "pip timed out after " + std::to_string(timeout_seconds) + "s";
    }

    DWORD code = 1;
    ::GetExitCodeProcess(process.hProcess, &code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    *exit_code = static_cast<int>(code);
    return true;
}

#else /* !_WIN32 */

/**
 * @brief 隐藏窗口运行子进程（POSIX: fork/exec，stdout/stderr 重定向到日志）。
 * @return 成功启动返回 `true`（`exit_code` 有效）；超时在 `error` 中说明。
 */
bool run_process(const std::string& program,
                 const std::vector<std::string>& arguments,
                 const std::string& log_path, int timeout_seconds,
                 int* exit_code, std::string* error) {
    const pid_t pid = fork();
    if (pid < 0) {
        *error = "fork failed while starting pip";
        return false;
    }
    if (pid == 0) {
        const int log = open(log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC,
                             static_cast<mode_t>(0644));
        const int input = open("/dev/null", O_RDONLY);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
            close(log);
        }
        if (input >= 0) {
            dup2(input, STDIN_FILENO);
            close(input);
        }

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(program.c_str()));
        for (size_t i = 0; i < arguments.size(); ++i) {
            argv.push_back(const_cast<char*>(arguments[i].c_str()));
        }
        argv.push_back(nullptr);
        execv(program.c_str(), argv.data());
        _exit(127);
    }

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(timeout_seconds > 0 ? timeout_seconds : 0);
    for (;;) {
        int status = 0;
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            if (WIFEXITED(status)) {
                *exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                *exit_code = 128 + WTERMSIG(status);
            } else {
                *exit_code = -1;
            }
            return true;
        }
        if (done < 0) {
            *error = "waitpid failed while waiting for pip";
            return false;
        }
        if (timeout_seconds > 0 &&
            std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            *error = "pip timed out after " + std::to_string(timeout_seconds) + "s";
            *exit_code = -1;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

#endif /* _WIN32 */

/** @brief 取拓展数据目录下的依赖根目录。 */
bool dependency_root(script_state* script, std::string* out,
                     std::string* error) {
    const spiration_host_api* host =
        script != nullptr ? script->desc.host : nullptr;
    if (host == nullptr || host->data_path == nullptr) {
        *error = "host does not provide data_path";
        return false;
    }
    const std::string relative = kDependencyDirectory;
    const std::string root =
        to_string(host->data_path(&script->desc, to_view(relative)));
    if (root.empty()) {
        *error = "cannot resolve the extension data directory";
        return false;
    }
    *out = root;
    return true;
}

/** @brief 清理环境变量读取到的超时值。 */
int pip_timeout_seconds() {
    int timeout = kDefaultPipTimeoutSeconds;
    const std::string configured = environment("SPIRATION_PYTHON_PIP_TIMEOUT");
    if (!configured.empty()) {
        const int parsed = std::atoi(configured.c_str());
        if (parsed > 0) timeout = parsed;
    }
    return timeout;
}

} // namespace

bool ensure_dependencies(script_state* script, std::string* error) {
    if (script == nullptr) return true;
    if (script->desc.host == nullptr) return true;

    /* 调用方允许不关心失败原因；统一下沉到一个本地缓冲区。 */
    std::string ignored_error;
    if (error == nullptr) error = &ignored_error;

    std::vector<std::string> requirements;
    {
        gil_guard gil;
        script_scope scope(script);
        if (!read_requirements(script, &requirements, error)) return false;
    }
    if (requirements.empty()) return true;

    std::string root;
    if (!dependency_root(script, &root, error)) return false;
    const std::string site_packages = join(root, kSitePackages);
    const std::string state_path = join(root, kStateFile);
    const std::string requirements_path = join(root, kRequirementsFile);
    const std::string pip_log_path = join(root, kPipLogFile);
    script->dependency_path = site_packages;

    const std::string desired = dependency_state(requirements);
    {
        gil_guard gil;
        std::string current;
        if (read_text_file(state_path, &current) && current == desired &&
            path_is_directory(site_packages)) {
            log_to(&script->desc, SPIRATION_LOG_DEBUG,
                   "python dependencies are up to date: " + site_packages,
                   "python/pip");
            return true;
        }
    }

    if (environment_flag("SPIRATION_PYTHON_DISABLE_PIP")) {
        log_to(&script->desc, SPIRATION_LOG_WARNING,
               "SPIRATION_PYTHON_DISABLE_PIP is set; expecting dependencies to "
               "be preinstalled at " +
                   site_packages,
               "python/pip");
        return true;
    }

    std::string program;
    {
        gil_guard gil;
        script_scope scope(script);
        if (!python_program(script, &program, error)) return false;
        if (!write_text_file(root, requirements_path,
                             requirements_text(requirements))) {
            *error = "cannot write the pip requirements file: " +
                     requirements_path;
            return false;
        }
    }

    std::vector<std::string> arguments;
    arguments.push_back("-m");
    arguments.push_back("pip");
    arguments.push_back("install");
    arguments.push_back("--disable-pip-version-check");
    arguments.push_back("--no-input");
    arguments.push_back("--upgrade");
    arguments.push_back("--no-warn-script-location");
    arguments.push_back("--target");
    arguments.push_back(site_packages);
    arguments.push_back("--requirement");
    arguments.push_back(requirements_path);

    const int timeout_seconds = pip_timeout_seconds();
    log_to(&script->desc, SPIRATION_LOG_INFO,
           "installing " + std::to_string(requirements.size()) +
               " Python dependenc(ies) for " + script->id + " (timeout " +
               std::to_string(timeout_seconds) + "s): " +
               requirements_text(requirements),
           "python/pip");

    int exit_code = -1;
    std::string run_error;
    const bool started = run_process(program, arguments, pip_log_path,
                                     timeout_seconds, &exit_code, &run_error);

    std::string output;
    read_file_tail(pip_log_path, kPipLogTailBytes, &output);
    const std::string output_tail =
        output.empty() ? std::string() : ":\n" + output;

    if (!started) {
        *error = "failed to start pip: " + run_error + output_tail;
        log_to(&script->desc, SPIRATION_LOG_ERROR, *error, "python/pip");
        return false;
    }
    if (!run_error.empty()) {
        *error = run_error + output_tail;
        log_to(&script->desc, SPIRATION_LOG_ERROR, *error, "python/pip");
        return false;
    }
    if (exit_code != 0) {
        *error = "pip install failed (exit " + std::to_string(exit_code) +
                 ")" + output_tail;
        log_to(&script->desc, SPIRATION_LOG_ERROR, *error, "python/pip");
        return false;
    }

    {
        gil_guard gil;
        if (!write_text_file(root, state_path, desired)) {
            *error = "cannot record the dependency state: " + state_path;
            return false;
        }
    }

    log_to(&script->desc, SPIRATION_LOG_INFO,
           "python dependencies installed for " + script->id + " at " +
               site_packages,
           "python/pip");
    return true;
}

void add_dependency_path(script_state* script) {
    if (script == nullptr || script->dependency_path.empty()) return;
    add_sys_path(script->dependency_path);
}

} // namespace loader
} // namespace python
} // namespace spiration
