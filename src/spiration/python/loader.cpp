/**
 * @file loader.cpp
 * @brief Python 加载器拓展：入口、脚本生命周期与加载器接口。
 * @author 陈林锴
 */

#include "internal.h"

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace spiration {
namespace python {
namespace loader {

namespace {

/* -------------------------------------------------------------------------
 * 当前脚本与全局登记
 * ---------------------------------------------------------------------- */

/** @brief 当前线程正在执行的脚本。 */
thread_local script_state* g_current_script = nullptr;

/** @brief 注入模块 `spiration`（全局共享，随解释器存在）。 */
PyObject* g_injected = nullptr;

/** @brief 尚未释放的脚本数量，用于避免过早收尾解释器。 */
int g_live_scripts = 0;

/** @brief 服务登记：条目指针 → 条目。 */
std::map<void*, service_entry*>& services() {
    static std::map<void*, service_entry*> table;
    return table;
}

/**
 * @brief 回调坟场：脚本卸载后仍需保留的对象。
 * @note 宿主可能长期持有菜单项与控件回调（例如用户点击由脚本创建的按钮，
 *       而脚本已卸载），故这类对象不能随脚本一起释放，统一搬运到此，
 *       等加载器自身卸载时再回收。
 */
std::vector<listener_ref*>& listener_graveyard() {
    static std::vector<listener_ref*> value;
    return value;
}

/** @brief 服务条目坟场。 */
std::vector<service_entry*>& service_graveyard() {
    static std::vector<service_entry*> value;
    return value;
}

/* -------------------------------------------------------------------------
 * 小工具
 * ---------------------------------------------------------------------- */

/** @brief 记录加载器自身产生的日志。 */
void log_with(const spiration_extension_desc* self, spiration_log_level level,
              const std::string& message, const char* tag) {
    if (self == nullptr || self->host == nullptr || self->host->log == nullptr) {
        return;
    }
    self->host->log(self, level, to_view(message), to_view(std::string(tag)));
}

/** @brief 判断路径是否以指定后缀结尾。 */
bool ends_with(const std::string& text, const char* suffix) {
    const size_t length = std::strlen(suffix);
    if (text.size() < length) return false;
    return text.compare(text.size() - length, length, suffix) == 0;
}

/** @brief 读取脚本模块的字符串属性。 */
std::string attribute_text(script_state* script, const char* name) {
    if (script == nullptr || script->module == nullptr) return std::string();

    PyObject* value = PyObject_GetAttrString(script->module, name);
    if (value == nullptr) {
        PyErr_Clear();
        return std::string();
    }
    std::string text;
    if (PyUnicode_Check(value)) {
        Py_ssize_t size = 0;
        const char* utf8 = PyUnicode_AsUTF8AndSize(value, &size);
        if (utf8 != nullptr) {
            text.assign(utf8, static_cast<size_t>(size));
        } else {
            PyErr_Clear();
        }
    }
    Py_DECREF(value);
    return text;
}

/**
 * @brief 调用脚本中的可选函数。
 * @param script 脚本状态。
 * @param name   函数名。
 * @param called 输出：是否找到并调用了该函数。
 * @return 新引用；未找到或调用失败返回 `nullptr`（失败时已设置 Python 异常）。
 */
PyObject* call_script_function(script_state* script, const char* name,
                               bool* called) {
    if (called != nullptr) *called = false;
    if (script == nullptr || script->module == nullptr) return nullptr;

    PyObject* function = PyObject_GetAttrString(script->module, name);
    if (function == nullptr) {
        PyErr_Clear();
        return nullptr;
    }
    if (!PyCallable_Check(function)) {
        Py_DECREF(function);
        return nullptr;
    }
    if (called != nullptr) *called = true;

    PyObject* result = PyObject_CallObject(function, nullptr);
    Py_DECREF(function);
    return result;
}

/**
 * @brief 判断路径是否为已存在的文件。
 * @note 需持有 GIL。走 `os.path.isfile` 而非 C 运行时的 `stat`，
 *       以便正确处理 UTF-8 路径。
 */
bool file_exists(const std::string& path) {
    PyObject* module = PyImport_ImportModule("os.path");
    if (module == nullptr) {
        PyErr_Clear();
        return false;
    }
    PyObject* result = PyObject_CallMethod(module, "isfile", "s", path.c_str());
    Py_DECREF(module);
    if (result == nullptr) {
        PyErr_Clear();
        return false;
    }
    const int truth = PyObject_IsTrue(result);
    Py_DECREF(result);
    return truth == 1;
}

/**
 * @brief 判断路径是否为目录。
 * @note 需持有 GIL。
 */
bool is_directory(const std::string& path) {
    PyObject* module = PyImport_ImportModule("os.path");
    if (module == nullptr) {
        PyErr_Clear();
        return false;
    }
    PyObject* result = PyObject_CallMethod(module, "isdir", "s", path.c_str());
    Py_DECREF(module);
    if (result == nullptr) {
        PyErr_Clear();
        return false;
    }
    const int truth = PyObject_IsTrue(result);
    Py_DECREF(result);
    return truth == 1;
}

/**
 * @brief 列出目录下的直接子目录（只保留目录）。
 * @note 需持有 GIL。
 */
std::vector<std::string> child_directories(const std::string& root) {
    std::vector<std::string> out;

    PyObject* os_module = PyImport_ImportModule("os");
    if (os_module == nullptr) {
        PyErr_Clear();
        return out;
    }
    PyObject* entries =
        PyObject_CallMethod(os_module, "listdir", "s", root.c_str());
    Py_DECREF(os_module);
    if (entries == nullptr) {
        PyErr_Clear();
        return out;
    }

    const Py_ssize_t count = PyList_Size(entries);
    for (Py_ssize_t i = 0; i < count; ++i) {
        PyObject* item = PyList_GetItem(entries, i); /* 借用引用 */
        if (item == nullptr || !PyUnicode_Check(item)) continue;

        const char* utf8 = PyUnicode_AsUTF8(item);
        if (utf8 == nullptr) {
            PyErr_Clear();
            continue;
        }
        const std::string candidate = root + "/" + utf8;
        if (is_directory(candidate)) out.push_back(candidate);
    }
    Py_DECREF(entries);
    return out;
}

/**
 * @brief 说明入口路径回退到了哪里。
 * @note 需持有 GIL；作为 Python 警告发出，因而会随宿主日志一并可见。
 */
void log_fallback(const std::string& requested, const std::string& resolved) {
    PyErr_WarnFormat(PyExc_UserWarning, 1,
                     "extension entry not found at '%s'; using '%s' instead "
                     "(the extension directory should be named after the "
                     "extension id)",
                     requested.c_str(), resolved.c_str());
    PyErr_Clear();
}

/**
 * @brief 向上取父目录。
 * @return 已到根时返回空串。
 */
std::string parent_of(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    if (cut == std::string::npos) return std::string();
    if (cut == 0) return path.substr(0, 1); /* 根目录 */
    /* 处理 "C:" 这类仅剩盘符的情形。 */
    if (cut == 2 && path[1] == ':') return path.substr(0, 3);
    return path.substr(0, cut);
}

/**
 * @brief 取文件所在目录。
 * @return 不含分隔符时返回空串。
 */
std::string directory_of(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    if (cut == std::string::npos) return std::string();
    if (cut == 0) return path.substr(0, 1);
    return path.substr(0, cut);
}

/**
 * @brief 解析入口脚本的实际路径，必要时回退到清单所在的真实目录。
 *
 * 宿主把拓展目录拼成 ``<数据目录>/<拓展 ID>``，所以当**目录名与 ID 不一致**
 * （手工安装时最常见，例如装进 ``<数据目录>/selftest`` 而 ID 是
 * ``com.example.selftest``）时，宿主给出的路径下的父目录就是数据目录，而真实
 * 拓展目录是它的**兄弟**目录——因此回退时要在父目录的**子目录**里找同名文件，
 * 而不是顺着已知路径向上找（那条路径本身并不存在，向上找永远找不到）。
 *
 * @param entry     宿主给出的入口绝对路径。
 * @param directory 宿主给出的拓展目录。
 * @return 可用的绝对路径；找不到唯一匹配时返回 ``entry`` 原值。
 * @note 需持有 GIL。
 */
std::string resolve_entry_path(const std::string& entry,
                               const std::string& directory) {
    if (entry.empty()) return entry;
    /* 常规情况：宿主给的路径正确。 */
    if (file_exists(entry)) return entry;

    const size_t slash = entry.find_last_of("/\\");
    const std::string name =
        slash == std::string::npos ? entry : entry.substr(slash + 1);
    if (name.empty()) return entry;

    const std::string root = parent_of(directory);
    if (root.empty()) return entry;

    /* 在数据目录的直接子目录里找同名文件；多个候选时宁可放弃，
       避免把本拓展解析到别的拓展身上。 */
    const std::vector<std::string> children = child_directories(root);
    std::string found;
    for (size_t i = 0; i < children.size(); ++i) {
        const std::string candidate = children[i] + "/" + name;
        if (!file_exists(candidate)) continue;
        if (!found.empty()) return entry; /* 不唯一 */
        found = candidate;
    }

    if (!found.empty()) log_fallback(entry, found);
    return found.empty() ? entry : found;
}

/**
 * @brief 确保解释器与注入模块就绪（幂等）。
 * @return 成功返回 `true`。
 */
bool ensure_runtime(spiration_extension_desc* reporter) {
    std::string error;
    if (!interpreter_ready() && !initialize_interpreter(&error)) {
        log_with(reporter, SPIRATION_LOG_ERROR,
                 "failed to start python interpreter: " + error, "python");
        return false;
    }

    gil_guard gil;
    if (g_injected == nullptr) {
        g_injected = build_injected_module();
        if (g_injected == nullptr) {
            report_python_error(nullptr, "inject");
            log_with(reporter, SPIRATION_LOG_ERROR,
                     "failed to build the injected spiration module", "python");
            return false;
        }
        PyObject* sys_modules = PyImport_GetModuleDict(); /* 借用引用 */
        if (sys_modules != nullptr &&
            PyDict_SetItemString(sys_modules, module_name(), g_injected) != 0) {
            PyErr_Clear();
            log_with(reporter, SPIRATION_LOG_WARNING,
                     "failed to register the spiration module in sys.modules",
                     "python");
        }
    }
    return true;
}

/* -------------------------------------------------------------------------
 * 脚本回调
 * ---------------------------------------------------------------------- */

/** @brief 脚本初始化。 */
spiration_bool SPIRATION_CALL script_initialize(spiration_extension_desc* self) {
    script_state* script =
        self != nullptr ? static_cast<script_state*>(self->user_data) : nullptr;
    if (script == nullptr) return SPIRATION_FALSE;
    if (!interpreter_ready()) {
        log_with(self, SPIRATION_LOG_ERROR, "python interpreter is not ready",
                 "python");
        return SPIRATION_FALSE;
    }

    gil_guard gil;
    script_scope scope(script);

    bool called = false;
    PyObject* result = call_script_function(script, "initialize", &called);
    if (!called) return SPIRATION_TRUE; /* initialize() 是可选的 */
    if (result == nullptr) {
        report_python_error(script, "initialize");
        return SPIRATION_FALSE;
    }

    const int truth = PyObject_IsTrue(result);
    Py_DECREF(result);
    if (truth < 0) {
        PyErr_Clear();
        return SPIRATION_TRUE;
    }
    return truth == 0 ? SPIRATION_FALSE : SPIRATION_TRUE;
}

/** @brief 脚本关闭。 */
void SPIRATION_CALL script_shutdown(spiration_extension_desc* self) {
    script_state* script =
        self != nullptr ? static_cast<script_state*>(self->user_data) : nullptr;
    if (script == nullptr) return;

    /* 只要收到了关闭回调，本脚本就**不再使用解释器**，因此在这里就把它从
       存活计数中移除：该回调之后它只等 `release` 释放内存。这样即便宿主
       因为 `initialize` 失败而跳过了某些拓展的关闭流程，计数也不会残留。 */
    if (g_live_scripts > 0) --g_live_scripts;

    if (!interpreter_ready()) return;

    gil_guard gil;
    script_scope scope(script);

    bool called = false;
    PyObject* result = call_script_function(script, "shutdown", &called);
    if (called && result == nullptr) {
        report_python_error(script, "shutdown");
    }
    Py_XDECREF(result);

    /* 先摘除模块，使新的导入失败；再收回服务；最后停用回调并丢弃模块引用。 */
    PyObject* sys_modules = PyImport_GetModuleDict();
    if (sys_modules != nullptr) {
        if (PyDict_DelItemString(sys_modules, script->module_key.c_str()) != 0) {
            PyErr_Clear();
        }
    }
    release_services(script->id);
    release_agent_providers(script);
    Py_CLEAR(script->module);
    release_callbacks(script);
    Py_CLEAR(script->injected);
}

/* -------------------------------------------------------------------------
 * 加载器接口
 * ---------------------------------------------------------------------- */

/** @brief 判断入口是否为 Python 脚本。 */
spiration_bool SPIRATION_CALL loader_probe(spiration_str_view entry_path,
                                           spiration_str_view manifest_json) {
    SPIRATION_UNUSED(manifest_json);
    const std::string path = to_string(entry_path);
    if (ends_with(path, ".py") || ends_with(path, ".pyw")) {
        return SPIRATION_TRUE;
    }
    return SPIRATION_FALSE;
}

/** @brief 为一个 Python 拓展构造描述符。 */
spiration_extension_desc* SPIRATION_CALL loader_instantiate(
    const spiration_host_api* host, spiration_str_view extension_id,
    spiration_str_view extension_directory, spiration_str_view entry_path,
    spiration_str_view manifest_json) {
    if (host == nullptr || extension_id.data == nullptr) return nullptr;

    script_state* script = new script_state();
    script->id = to_string(extension_id);
    script->directory = to_string(extension_directory);
    script->entry = to_string(entry_path);
    script->manifest = to_string(manifest_json);
    script->module_key = script->id;

    std::memset(&script->desc, 0, sizeof(script->desc));
    script->desc.version = SPIRATION_STRUCT_VERSION;
    script->desc.size = static_cast<uint32_t>(sizeof(spiration_extension_desc));
    script->desc.phase = SPIRATION_PHASE_NORMAL;
    script->desc.api_version_required = SPIRATION_API_VERSION;
    script->desc.id = to_view(script->id);
    script->desc.host = host;
    script->desc.user_data = script;
    script->desc.on_initialize = script_initialize;
    script->desc.on_shutdown = script_shutdown;

    if (!ensure_runtime(&script->desc)) {
        delete script;
        return nullptr;
    }

    /* 宿主给的入口路径可能因目录名与 ID 不一致而失效，此处解析出真实路径。 */
    {
        gil_guard gil;
        script->entry = resolve_entry_path(script->entry, script->directory);
    }

    /* 安装清单声明的 PyPI 依赖：清单含 python.requirements 时，
       在导入脚本前通过 pip 安装到拓展数据目录并注入 sys.path。
       失败则拒绝装载，避免脚本在缺依赖的情况下留下半成品状态。 */
    {
        std::string dependency_error;
        if (!ensure_dependencies(script, &dependency_error)) {
            log_with(&script->desc, SPIRATION_LOG_ERROR,
                     "failed to prepare python dependencies: " +
                         dependency_error,
                     "python/pip");
            delete script;
            return nullptr;
        }
    }

    {
        gil_guard gil;
        script_scope scope(script);

        /* 模块搜索路径按脚本的**实际**目录追加：当拓展目录名与 ID 不一致时，
           解析出的入口可能位于宿主给出的目录之外（见 resolve_entry_path）。 */
        add_dependency_path(script);
        add_script_paths(directory_of(script->entry));
        script->module = import_script_module(script);
        if (script->module == nullptr) {
            report_python_error(script, "import");
            delete script;
            return nullptr;
        }

        script->name = attribute_text(script, "name");
        script->description = attribute_text(script, "description");
        script->version = attribute_text(script, "version");
    }

    script->desc.name = to_view(script->name);
    script->desc.description = to_view(script->description);
    script->desc.version_string = to_view(script->version);

    ++g_live_scripts;
    log_with(&script->desc, SPIRATION_LOG_INFO,
             "python extension loaded from " + script->entry, "python");
    return &script->desc;
}

/** @brief 归还由 `instantiate` 构造的描述符。 */
void SPIRATION_CALL loader_release(spiration_extension_desc* desc) {
    if (desc == nullptr) return;
    script_state* script = static_cast<script_state*>(desc->user_data);
    desc->user_data = nullptr;
    /* 存活计数已在 `script_shutdown` 里递减，此处只释放内存，
       避免同一脚本被计两次。 */
    delete script;
}

/** @brief 加载器函数表。 */
const spiration_loader_api* loader_table() {
    static spiration_loader_api table = []() {
        spiration_loader_api api;
        std::memset(&api, 0, sizeof(api));
        api.version = SPIRATION_API_VERSION;
        api.size = static_cast<uint32_t>(sizeof(spiration_loader_api));
        api.probe = loader_probe;
        api.instantiate = loader_instantiate;
        api.release = loader_release;
        return api;
    }();
    return &table;
}

/* -------------------------------------------------------------------------
 * 拓展本体
 * ---------------------------------------------------------------------- */

/**
 * @brief 加载器拓展的初始化（`SPIRATION_PHASE_EARLY`）。
 * @note `register_loader` 已在入口符号中完成，此处只负责启动解释器，
 *       使 `spiration` 模块在脚本初始化之前就绪。
 */
spiration_bool SPIRATION_CALL extension_initialize(
    spiration_extension_desc* self) {
    if (!ensure_runtime(self)) return SPIRATION_FALSE;

    gil_guard gil;
    if (g_injected == nullptr) return SPIRATION_FALSE;

    PyObject* version_object = PySys_GetObject("version"); /* 借用引用 */
    const char* version =
        version_object != nullptr ? PyUnicode_AsUTF8(version_object) : nullptr;
    if (version == nullptr) {
        PyErr_Clear();
        version = "unknown";
    }

    /* 一并记录解释器来自哪里，便于排查目标环境中的安装问题。 */
    const std::string home = python_home();
    log_with(self, SPIRATION_LOG_INFO,
             std::string("python ") + version + " ready" +
                 (home.empty() ? std::string(" (auto-detected)")
                               : " (home: " + home + ")"),
             "python");
    return SPIRATION_TRUE;
}

/** @brief 加载器拓展的关闭。 */
void SPIRATION_CALL extension_shutdown(spiration_extension_desc* self) {
    SPIRATION_UNUSED(self);
    if (!interpreter_ready()) return;

    if (g_live_scripts > 0) {
        /* 理论上不应发生：加载器应在所有托管拓展之后卸载。
           此时宁可泄漏也不收尾解释器，以免尚未关闭的脚本访问已销毁的运行时。 */
        log_with(self, SPIRATION_LOG_WARNING,
                 "python loader is shutting down while " +
                     std::to_string(g_live_scripts) +
                     " script extension(s) are still alive; interpreter is kept",
                 "python");
        return;
    }

    {
        gil_guard gil;
        release_graveyard();
        Py_CLEAR(g_injected);
    }
    finalize_interpreter();
}

/** @brief 拓展描述符。 */
spiration_extension_desc g_descriptor = {
    SPIRATION_STRUCT_VERSION,                     /* version              */
    (uint32_t)sizeof(spiration_extension_desc),   /* size                 */
    SPIRATION_PHASE_EARLY,                        /* phase                */
    SPIRATION_API_VERSION,                        /* api_version_required */
    SPIRATION_STR_INIT("com.flamence.spiration.python"), /* id            */
    SPIRATION_STR_INIT("Python"),                 /* name                 */
    SPIRATION_STR_INIT("0.1.0"),                  /* version              */
    SPIRATION_STR_INIT("Loads extensions written in Python."), /* description */
    NULL,                                         /* host（宿主填写）      */
    NULL,                                         /* user_data            */
    extension_initialize,                         /* on_initialize        */
    extension_shutdown,                           /* on_shutdown          */
    NULL,                                         /* on_event             */
    {NULL}                                        /* reserved             */
};

} // namespace

/* -------------------------------------------------------------------------
 * 对外（供 module.cpp / runtime.cpp 调用）
 * ---------------------------------------------------------------------- */

script_state* current_script() { return g_current_script; }

void set_current_script(script_state* script) { g_current_script = script; }

void log_to(const spiration_extension_desc* self, spiration_log_level level,
            const std::string& message, const std::string& tag) {
    if (self == nullptr || self->host == nullptr || self->host->log == nullptr) {
        return;
    }
    self->host->log(self, level, to_view(message), to_view(tag));
}

service_entry* register_python_service(script_state* script, PyObject* object) {
    if (script == nullptr || object == nullptr) return nullptr;
    service_entry* entry = new service_entry();
    entry->owner = script->id;
    entry->object = object;
    Py_INCREF(object);
    services()[entry] = entry;
    return entry;
}

PyObject* lookup_python_service(void* handle) {
    std::map<void*, service_entry*>::iterator it = services().find(handle);
    if (it == services().end()) return nullptr;
    return it->second->object;
}

void release_services(const std::string& owner) {
    std::vector<service_entry*> collected;
    for (std::map<void*, service_entry*>::iterator it = services().begin();
         it != services().end(); ++it) {
        if (it->second != nullptr && it->second->owner == owner) {
            collected.push_back(it->second);
        }
    }

    /* 先摘除条目，使再入代码查不到它们；再释放 Python 引用。 */
    for (size_t i = 0; i < collected.size(); ++i) {
        services().erase(collected[i]);
        service_graveyard().push_back(collected[i]);
    }
    for (size_t i = 0; i < collected.size(); ++i) {
        Py_CLEAR(collected[i]->object);
    }
}

void release_callbacks(script_state* script) {
    if (script == nullptr) return;

    /* 先把登记表搬空，避免释放 Python 引用时触发的再入代码破坏遍历。 */
    std::map<int64_t, listener_ref*> listeners;
    listeners.swap(script->listeners);

    for (std::map<int64_t, listener_ref*>::iterator it = listeners.begin();
         it != listeners.end(); ++it) {
        listener_ref* ref = it->second;
        if (ref == nullptr) continue;

        if (ref->persistent) {
            /* 宿主可能仍持有（菜单项、控件事件）：停用后送入坟场。 */
            ref->script = nullptr;
            listener_graveyard().push_back(ref);
        } else {
            Py_XDECREF(ref->callable);
            delete ref;
        }
    }

    /* 宿主保证关闭后丢弃未执行的投递任务，故此处可直接回收。 */
    std::set<dispatch_task*> tasks;
    tasks.swap(script->pending_tasks);
    for (std::set<dispatch_task*>::iterator it = tasks.begin();
         it != tasks.end(); ++it) {
        dispatch_task* task = *it;
        Py_XDECREF(task->callable);
        delete task;
    }
}

void release_graveyard() {
    /* 同样先搬空，避免释放过程中的再入代码影响遍历。 */
    std::vector<listener_ref*> listeners;
    listeners.swap(listener_graveyard());
    std::vector<service_entry*> entries;
    entries.swap(service_graveyard());

    for (size_t i = 0; i < listeners.size(); ++i) {
        Py_XDECREF(listeners[i]->callable);
        delete listeners[i];
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        Py_CLEAR(entries[i]->object);
        delete entries[i];
    }
}

/* 入口钩子位于命名空间之外，通过下面两个包装访问匿名命名空间中的状态。 */

/** @brief 加载器自身的描述符。 */
spiration_extension_desc* loader_descriptor() { return &g_descriptor; }

/** @brief 加载器函数表。 */
const spiration_loader_api* loader_api_table() { return loader_table(); }

} // namespace loader
} // namespace python
} // namespace spiration

/**
 * @brief 入口钩子。
 */
static const spiration_extension_desc* SPIRATION_CALL python_loader_hook(
    const spiration_host_api* host, spiration_str_view manifest_json) {
    SPIRATION_UNUSED(manifest_json);

    spiration::python::loader::loader_descriptor()->host = host;
    if (host != nullptr && host->register_loader != nullptr &&
        host->register_loader(spiration::python::loader::loader_descriptor(),
                              spiration::python::loader::loader_api_table()) !=
            SPIRATION_OK) {
        const std::string message = "failed to register the python loader";
        if (host->log != nullptr) {
            host->log(spiration::python::loader::loader_descriptor(),
                      SPIRATION_LOG_ERROR,
                      spiration_str_view_of(message.data(), message.size()),
                      SPIRATION_STR("python"));
        }
    }
    return spiration::python::loader::loader_descriptor();
}

SPIRATION_DEFINE_EXTENSION_ENTRY_HOOKED(&python_loader_hook)
