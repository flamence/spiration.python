/**
 * @file runtime.cpp
 * @brief Python 解释器生命周期与脚本导入。
 * @author 陈林锴
 */

#include "internal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(_WIN32) && defined(_MSC_VER)
#include <shlobj.h>
#define SPIRATION_PYTHON_HAVE_REGISTRY 1
#endif

namespace spiration {
namespace python {
namespace loader {

namespace {

/** @brief 由本加载器启动的解释器主线程状态。 */
PyThreadState* g_main_thread_state = nullptr;
/** @brief 解释器是否已就绪。 */
bool g_ready = false;
/** @brief 是否需要由本加载器负责收尾。 */
bool g_owned = false;

/** @brief 实际用于启动解释器的根目录；空表示交由 CPython 自行探测。 */
std::string g_home;

/** @brief 发现过程中已尝试的候选，用于失败诊断。 */
std::vector<std::string> g_attempts;

/** @brief 读取环境变量。 */
std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

/** @brief 去掉末尾的路径分隔符。 */
std::string trim_separators(std::string path) {
    while (path.size() > 1 && (path[path.size() - 1] == '/' || path[path.size() - 1] == '\\')) {
        path.erase(path.size() - 1);
    }
    return path;
}

/**
 * @brief 解释器根目录：仅看环境变量。
 * @note 环境变量是运维显式指定的，故优先于任何自动发现。
 */
std::string configured_home() {
    const std::string standard = environment("PYTHONHOME");
    if (!standard.empty()) return standard;
    return std::string();
}

/* -------------------------------------------------------------------------
 * 目标环境中的 Python 目录发现
 *
 * 装载器会被分发到与构建机不同的机器，因此不能依赖编译期记录的绝对路径。
 * 下面按优先级枚举**目标环境**中可能存在的解释器根目录，再用
 * `is_python_home` 校验，取第一个有效者。
 * ---------------------------------------------------------------------- */

/**
 * @brief 判断路径是否为目录。
 * @note 非 Windows 平台只做非空判断：候选表本身由平台相关的来源构成，
 *       真正的可用性仍由 `is_python_home` 里的库/标准库探测决定。
 */
bool is_directory(const std::string& path) {
#if defined(_WIN32)
    const DWORD attributes = ::GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    return !path.empty();
#endif
}

#if defined(_WIN32)

/** @brief 判断路径是否为（存在的）文件。 */
bool is_file(const std::string& path) {
    const DWORD attributes = ::GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

#endif /* _WIN32 */

/**
 * @brief 由 `3.11.9` 形式的版本号拼出解释器动态库文件名。
 * @return 无法解析时返回空串。
 */
std::string library_file_name(const std::string& version) {
    if (version.empty()) return std::string();

    unsigned major = 0;
    unsigned minor = 0;
    if (std::sscanf(version.c_str(), "%u.%u", &major, &minor) != 2) {
        return std::string();
    }

#if defined(_WIN32)
    /* 嵌入 API 必须链接版本化的 python3xx.dll（而非稳定 ABI 的 python3.dll）。 */
    return "python" + std::to_string(major) + std::to_string(minor) + ".dll";
#elif defined(__APPLE__)
    return "libpython" + std::to_string(major) + "." + std::to_string(minor) + ".dylib";
#else
    return "libpython" + std::to_string(major) + "." + std::to_string(minor) + ".so";
#endif
}

/**
 * @brief 校验候选目录是否为可用的解释器根目录。
 *
 * 判据是「解释器动态库」与「标准库」同时存在——只有库不足以启动解释器
 * （标准库缺失时 `Py_InitializeFromConfig` 会直接失败）。
 *
 * @note 非 Windows 平台以 `<root>/lib` 是否存在为准；该分支的候选来源
 *       较少，且失败时会回退到 CPython 自身的探测。
 */
bool is_python_home(const std::string& home) {
    if (home.empty() || !is_directory(home)) return false;

#if defined(_WIN32)
    const std::string library =
        library_file_name(SPIRATION_PYTHON_VERSION_STRING);
    if (library.empty() || !is_file(home + "\\" + library)) return false;
    return is_directory(home + "\\Lib") && is_file(home + "\\Lib\\os.py");
#else
    return is_directory(home + "/lib");
#endif
}

/** @brief 追加候选，去重并去掉末尾分隔符。 */
void add_candidate(std::vector<std::string>& out, const std::string& path) {
    const std::string cleaned = trim_separators(path);
    if (cleaned.empty()) return;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == cleaned) return;
    }
    out.push_back(cleaned);
}

/* ---- Windows：注册表 ---- */

#if defined(SPIRATION_PYTHON_HAVE_REGISTRY)

/** @brief 读取某个 `PythonCore` 版本键下的 `InstallPath`。 */
void add_registry_install_path(std::vector<std::string>& out, HKEY root,
                               const std::string& version_key) {
    std::string subkey = "SOFTWARE\\Python\\PythonCore\\" + version_key +
                         "\\InstallPath";
    HKEY key = nullptr;
    if (::RegOpenKeyExA(root, subkey.c_str(), 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return;
    }

    char buffer[MAX_PATH] = {0};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    const LONG status = ::RegQueryValueExA(key, nullptr, nullptr, &type,
                                           reinterpret_cast<LPBYTE>(buffer),
                                           &size);
    ::RegCloseKey(key);
    if (status == ERROR_SUCCESS && type == REG_SZ) {
        add_candidate(out, std::string(buffer));
    }
}

/** @brief 枚举 `SOFTWARE\Python\PythonCore` 下的全部版本（含 WOW64 视图）。 */
void add_registry_candidates(std::vector<std::string>& out) {
    const HKEY roots[] = {HKEY_CURRENT_USER};
    const REGSAM views[] = {KEY_READ | KEY_WOW64_64KEY, KEY_READ | KEY_WOW64_32KEY};

    for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); ++r) {
        for (size_t v = 0; v < sizeof(views) / sizeof(views[0]); ++v) {
            HKEY core = nullptr;
            if (::RegOpenKeyExA(roots[r], "SOFTWARE\\Python\\PythonCore", 0,
                                views[v], &core) != ERROR_SUCCESS) {
                continue;
            }

            for (DWORD index = 0;; ++index) {
                char name[128] = {0};
                DWORD name_size = sizeof(name);
                if (::RegEnumKeyExA(core, index, name, &name_size, nullptr,
                                    nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                    break;
                }
                add_registry_install_path(out, roots[r], name);
            }
            ::RegCloseKey(core);
        }
    }
}

#endif /* SPIRATION_PYTHON_HAVE_REGISTRY */

/* ---- Windows：语言无关的注册表探测 ---- */

#if defined(SPIRATION_PYTHON_HAVE_REGISTRY)

/** @brief 读取 `HKLM\SOFTWARE\Python\PyLauncher` 下各版本的 `InstallPath`。 */
void add_launcher_candidates(std::vector<std::string>& out) {
    const REGSAM views[] = {KEY_READ | KEY_WOW64_64KEY, KEY_READ | KEY_WOW64_32KEY};
    for (size_t v = 0; v < sizeof(views) / sizeof(views[0]); ++v) {
        HKEY key = nullptr;
        if (::RegOpenKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\Python\\PyLauncher", 0,
                            views[v], &key) != ERROR_SUCCESS) {
            continue;
        }

        for (DWORD index = 0;; ++index) {
            char name[128] = {0};
            DWORD name_size = sizeof(name);
            if (::RegEnumKeyExA(key, index, name, &name_size, nullptr,
                                nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                break;
            }

            std::string subkey = "SOFTWARE\\Python\\PyLauncher\\" + std::string(name) +
                                 "\\InstallPath";
            HKEY install = nullptr;
            if (::RegOpenKeyExA(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_READ,
                                &install) != ERROR_SUCCESS) {
                continue;
            }
            char buffer[MAX_PATH] = {0};
            DWORD size = sizeof(buffer);
            DWORD type = 0;
            if (::RegQueryValueExA(install, nullptr, nullptr, &type,
                                   reinterpret_cast<LPBYTE>(buffer),
                                   &size) == ERROR_SUCCESS &&
                type == REG_SZ) {
                add_candidate(out, std::string(buffer));
            }
            ::RegCloseKey(install);
        }
        ::RegCloseKey(key);
    }
}

#endif /* SPIRATION_PYTHON_HAVE_REGISTRY */

/** @brief 扫描某目录下的直接子目录，加入候选。 */
void add_child_directories(std::vector<std::string>& out,
                           const std::string& parent) {
#if defined(_WIN32)
    if (!is_directory(parent)) return;

    WIN32_FIND_DATAA entry;
    const std::string pattern = parent + "\\*";
    HANDLE handle = ::FindFirstFileA(pattern.c_str(), &entry);
    if (handle == INVALID_HANDLE_VALUE) return;

    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (entry.cFileName[0] == '.') continue;
        add_candidate(out, parent + "\\" + entry.cFileName);
    } while (::FindNextFileA(handle, &entry) != 0);

    ::FindClose(handle);
#else
    (void)out;
    (void)parent;
#endif
}

#if defined(_WIN32)

/** @brief 从 `PATH` 中找出解释器所在目录，并推导其根目录。 */
void add_path_candidates(std::vector<std::string>& out) {
    const std::string path = environment("PATH");
    if (path.empty()) return;

    size_t begin = 0;
    while (begin <= path.size()) {
        const size_t end = path.find(';', begin);
        const std::string entry =
            path.substr(begin, end == std::string::npos ? std::string::npos
                                                        : end - begin);
        begin = (end == std::string::npos) ? path.size() + 1 : end + 1;
        if (entry.empty()) continue;

        const std::string cleaned = trim_separators(entry);
        add_candidate(out, cleaned);

        /* PATH 常指向 `<root>`、`<root>\Scripts` 或 `<root>\bin`，统一归一到根目录。 */
        static const char* const suffixes[] = {"\\Scripts", "\\bin"};
        for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
            const size_t length = std::strlen(suffixes[i]);
            if (cleaned.size() > length &&
                cleaned.compare(cleaned.size() - length, length, suffixes[i]) == 0) {
                add_candidate(out, cleaned.substr(0, cleaned.size() - length));
            }
        }
    }
}

/**
 * @brief 枚举目标环境中的候选解释器根目录，按优先级排列。
 *
 * 顺序（高 → 低）：
 * 1. 加载器自身所在目录 —— 便于随拓展一起分发解释器运行时；
 * 2. 注册表中注册的解释器（`PythonCore`、`PyLauncher`）；
 * 3. 各版本管理器与常见安装位置的固定路径；
 * 4. 构建期记录的根目录（开发机上最准，分发后最不可靠）；
 * 5. `PATH` 中的条目。
 */
std::vector<std::string> environment_candidates() {
    std::vector<std::string> out;

    /* 1. 加载器自身所在目录，及其同级 runtime/（随拓展分发的运行时）。 */
    char module[MAX_PATH] = {0};
    const DWORD length =
        ::GetModuleFileNameA(nullptr, module, static_cast<DWORD>(sizeof(module)));
    if (length > 0 && length < sizeof(module)) {
        std::string here(module);
        const size_t slash = here.find_last_of("\\/");
        if (slash != std::string::npos) {
            here.erase(slash);
            add_candidate(out, here);

            /* 加载器位于 <拓展>/bin 时，解释器可能被放在 <拓展>/runtime。 */
            std::string root = here;
            const size_t parent = root.find_last_of("\\/");
            if (parent != std::string::npos) {
                root.erase(parent);
                add_candidate(out, root + "\\runtime");
            }
        }
    }

    /* 2. 注册表。 */
#if defined(SPIRATION_PYTHON_HAVE_REGISTRY)
    add_registry_candidates(out);
    add_launcher_candidates(out);
#endif

    /* 3. 固定路径。 */
    const std::string local_appdata = environment("LOCALAPPDATA");
    const std::string program_files = environment("ProgramFiles");
    const std::string program_files_x86 = environment("ProgramFiles(x86)");

    if (!local_appdata.empty()) {
        /* PyManager 把各版本装在 %LOCALAPPDATA%\Python\pythoncore-*。 */
        add_child_directories(out, local_appdata + "\\Python");

        const std::string version = SPIRATION_PYTHON_VERSION_STRING;
        add_candidate(out, local_appdata + "\\Programs\\Python\\Python" + version);
        add_candidate(out, local_appdata + "\\Programs\\Python\\Python" + version +
                               "-32");
    }
    if (!program_files.empty()) {
        add_candidate(out, program_files + "\\Python" +
                               SPIRATION_PYTHON_VERSION_STRING);
    }
    if (!program_files_x86.empty()) {
        add_candidate(out, program_files_x86 + "\\Python" +
                               SPIRATION_PYTHON_VERSION_STRING);
    }
    add_candidate(out, "C:\\Python" SPIRATION_PYTHON_VERSION_COMPACT);
    add_candidate(out, "C:\\ProgramData\\chocolatey\\lib\\python3\\tools");

    /* 4. 构建期记录的默认根目录：开发机上最准，分发后最不可靠，故靠后。 */
#ifdef SPIRATION_PYTHON_DEFAULT_HOME
    add_candidate(out, SPIRATION_PYTHON_DEFAULT_HOME);
#endif

    /* 5. PATH。 */
    add_path_candidates(out);

    return out;
}

#else /* !_WIN32 */

/**
 * @brief 枚举目标环境中的候选解释器根目录。
 *
 * 非 Windows 平台把定位工作交给 CPython 自身（它读取编译期 prefix、`PATH`
 * 与 `sysconfig`，比这里硬编码前缀更可靠），此处只保留构建期记录的根目录。
 */
std::vector<std::string> environment_candidates() {
    std::vector<std::string> out;
#ifdef SPIRATION_PYTHON_DEFAULT_HOME
    add_candidate(out, SPIRATION_PYTHON_DEFAULT_HOME);
#endif
    return out;
}

#endif /* _WIN32 */
/** @brief 记录一次候选尝试。 */
void note_attempt(const std::string& source, const std::string& path,
                  const char* reason) {
    g_attempts.push_back(source + ": " + path + " (" + reason + ")");
}

/**
 * @brief 在目标环境中寻找可用的解释器根目录。
 * @param source 输出：命中候选的来源描述。
 * @return 根目录；未找到返回空串。
 */
std::string find_python_home(std::string* source) {
    g_attempts.clear();

    const std::vector<std::string> candidates = environment_candidates();
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (is_python_home(candidates[i])) {
            if (source != nullptr) *source = "target environment";
            return candidates[i];
        }
        note_attempt("target", candidates[i], "not a python home");
    }
    return std::string();
}

/** @brief 把尝试记录追加到错误文本，便于定位环境问题。 */
std::string with_attempts(const std::string& error) {
    if (g_attempts.empty()) return error;

    std::string text = error;
    text += "\nsearched:";
    /* 只保留前若干条，避免日志被长 PATH 淹没。 */
    const size_t limit = g_attempts.size() < 8 ? g_attempts.size() : 8;
    for (size_t i = 0; i < limit; ++i) {
        text += "\n  - " + g_attempts[i];
    }
    if (g_attempts.size() > limit) {
        text += "\n  - ... (" + std::to_string(g_attempts.size() - limit) +
                " more)";
    }
    return text;
}

/** @brief 尝试以指定根目录启动解释器。 */
bool try_start(const std::string& home, std::string* error) {
    PyConfig config;
    PyConfig_InitPythonConfig(&config);

    /* 宿主 argv 属于 Spiration，不能交给 Python 解析。 */
    config.parse_argv = 0;
    /* 信号处理交由宿主，避免解释器抢占宿主的事件源。 */
    config.install_signal_handlers = 0;

    if (!home.empty()) {
        PyStatus set = PyConfig_SetBytesString(&config, &config.home, home.c_str());
        if (PyStatus_Exception(set)) {
            if (error != nullptr && set.err_msg != nullptr) *error = set.err_msg;
            PyConfig_Clear(&config);
            return false;
        }
    }

    const PyStatus status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) {
        if (error != nullptr) {
            *error = status.err_msg != nullptr ? status.err_msg
                                               : "Py_InitializeFromConfig failed";
        }
        return false;
    }
    return true;
}

/** @brief 取 `sys.path` 并插入一项。 */
void prepend_sys_path(const std::string& path) {
    PyObject* sys_path = PySys_GetObject("path"); /* 借用引用 */
    if (sys_path == nullptr || !PyList_Check(sys_path)) return;

    PyObject* item = PyUnicode_FromString(path.c_str());
    if (item == nullptr) {
        PyErr_Clear();
        return;
    }
    /* 已存在则不重复插入，避免重复导入路径堆积。 */
    if (PySequence_Contains(sys_path, item) == 1) {
        Py_DECREF(item);
        return;
    }
    PyList_Insert(sys_path, 0, item);
    Py_DECREF(item);
}

} // namespace

std::string to_string(spiration_str_view text) {
    return text.data != nullptr ? std::string(text.data, text.length)
                                : std::string();
}

spiration_str_view to_view(const std::string& text) {
    spiration_str_view out;
    out.data = text.empty() ? nullptr : text.data();
    out.length = text.size();
    return out;
}

bool interpreter_ready() { return g_ready; }

std::string python_home() { return g_home; }

bool initialize_interpreter(std::string* error) {
    if (Py_IsInitialized()) {
        /* 宿主（或另一个加载器）已启动解释器：复用而不接管其生命周期。 */
        g_ready = true;
        g_owned = false;
        return true;
    }

    std::string failure;

    /* 1. 环境变量显式指定：失败即报错，不静默改用别的解释器。 */
    const std::string specified = configured_home();
    if (!specified.empty()) {
        if (try_start(specified, &failure)) {
            g_main_thread_state = PyEval_SaveThread();
            g_home = specified;
            g_ready = true;
            g_owned = true;
            return true;
        }
        if (error != nullptr) {
            *error = "SPIRATION_PYTHON_HOME/PYTHONHOME is not usable: " +
                     failure;
        }
        return false;
    }

    /* 2. 在目标环境中寻找解释器根目录。 */
    std::string source;
    const std::string found = find_python_home(&source);
    if (!found.empty()) {
        if (try_start(found, &failure)) {
            g_main_thread_state = PyEval_SaveThread();
            g_home = found;
            g_ready = true;
            g_owned = true;
            return true;
        }
        if (error != nullptr) {
            *error = "failed to start the interpreter at " + found + " (" +
                     source + "): " + failure;
        }
        return false;
    }

    /* 3. 交给 CPython 自行探测（PATH、注册表、编译期 prefix 等）。 */
    std::string probe_failure;
    if (try_start(std::string(), &probe_failure)) {
        g_main_thread_state = PyEval_SaveThread();
        g_home.clear();
        g_ready = true;
        g_owned = true;
        return true;
    }

    if (error != nullptr) {
        *error = with_attempts(
            "no usable python installation found; automatic probe also failed: " +
            probe_failure);
    }
    return false;
}

void finalize_interpreter() {
    if (!g_owned || !Py_IsInitialized()) return;

    PyEval_RestoreThread(g_main_thread_state);
    g_main_thread_state = nullptr;
    Py_FinalizeEx();
    g_owned = false;
    g_ready = false;
    g_home.clear();
}

void add_script_paths(const std::string& directory) {
    if (directory.empty()) return;

    /* 顺序敏感：后插入者更靠前，故按“由外到内”追加。 */
    prepend_sys_path(directory + "/site-packages");
    prepend_sys_path(directory + "/lib");
    prepend_sys_path(directory + "/python");
    prepend_sys_path(directory);
}

PyObject* import_script_module(script_state* script) {
    PyObject* util = PyImport_ImportModule("importlib.util");
    if (util == nullptr) return nullptr;

    PyObject* spec = PyObject_CallMethod(util, "spec_from_file_location", "ss",
                                         script->module_key.c_str(),
                                         script->entry.c_str());
    if (spec == nullptr) {
        Py_DECREF(util);
        return nullptr;
    }

    PyObject* module = PyObject_CallMethod(util, "module_from_spec", "O", spec);
    if (module == nullptr) {
        Py_DECREF(spec);
        Py_DECREF(util);
        return nullptr;
    }

    /* 先登记到 sys.modules，使脚本内的相对导入与循环导入正常工作。 */
    PyObject* sys_modules = PyImport_GetModuleDict(); /* 借用引用 */
    if (sys_modules != nullptr) {
        PyObject* key = PyUnicode_FromString(script->module_key.c_str());
        if (key != nullptr) {
            PyDict_SetItem(sys_modules, key, module);
            Py_DECREF(key);
        }
    }

    PyObject* loader = PyObject_GetAttrString(spec, "loader");
    PyObject* exec_module =
        loader != nullptr ? PyObject_GetAttrString(loader, "exec_module") : nullptr;
    PyObject* outcome = nullptr;
    if (exec_module != nullptr) {
        outcome = PyObject_CallFunctionObjArgs(exec_module, module, NULL);
    }
    const bool ok = outcome != nullptr;

    Py_XDECREF(outcome);
    Py_XDECREF(exec_module);
    Py_XDECREF(loader);
    Py_DECREF(util);
    Py_DECREF(spec);

    if (!ok) {
        /* 导入失败：从 sys.modules 撤回，避免留下半成品模块。 */
        if (sys_modules != nullptr) {
            PyObject* key = PyUnicode_FromString(script->module_key.c_str());
            if (key != nullptr) {
                PyDict_DelItem(sys_modules, key);
                Py_DECREF(key);
            } else {
                PyErr_Clear();
            }
        }
        Py_DECREF(module);
        return nullptr;
    }

    return module;
}

void report_python_error(script_state* script, const char* context) {
    PyObject* type = nullptr;
    PyObject* value = nullptr;
    PyObject* traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback);
    if (type == nullptr && value == nullptr) return;
    PyErr_NormalizeException(&type, &value, &traceback);

    std::string text;
    PyObject* traceback_module = PyImport_ImportModule("traceback");
    if (traceback_module != nullptr) {
        PyObject* lines = PyObject_CallMethod(
            traceback_module, "format_exception", "OOO",
            type != nullptr ? type : Py_None,
            value != nullptr ? value : Py_None,
            traceback != nullptr ? traceback : Py_None);
        if (lines != nullptr) {
            PyObject* separator = PyUnicode_FromString("");
            PyObject* joined = separator != nullptr
                                   ? PyUnicode_Join(separator, lines)
                                   : nullptr;
            if (joined != nullptr) {
                const char* utf8 = PyUnicode_AsUTF8(joined);
                if (utf8 != nullptr) text = utf8;
                Py_DECREF(joined);
            }
            Py_XDECREF(separator);
            Py_DECREF(lines);
        } else {
            PyErr_Clear();
        }
        Py_DECREF(traceback_module);
    } else {
        PyErr_Clear();
    }

    if (text.empty() && value != nullptr) {
        PyObject* repr = PyObject_Str(value);
        if (repr != nullptr) {
            const char* utf8 = PyUnicode_AsUTF8(repr);
            if (utf8 != nullptr) text = utf8;
            Py_DECREF(repr);
        } else {
            PyErr_Clear();
        }
    }

    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(traceback);

    if (text.empty()) text = "unknown python error";

    const spiration_host_api* host = script->desc.host;
    if (host != nullptr && host->log != nullptr) {
        host->log(&script->desc, SPIRATION_LOG_ERROR, to_view(text),
                  to_view(std::string("python/") + (context ? context : "")));
    }
}

void report_callback_error(script_state* script, const char* context) {
    report_python_error(script, context);
}

} // namespace loader
} // namespace python
} // namespace spiration
