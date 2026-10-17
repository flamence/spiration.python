/**
 * @file internal.h
 * @brief Python 加载器的内部声明。
 * @author 陈林锴
 *
 * 加载器本身是一个普通拓展（`com.flamence.spiration.python`），
 * 通过 `spiration_loader_api` 为脚本类拓展提供宿主能力：
 *
 * ```
 * 宿主 ──(register_loader)──▶ 本加载器 ──(importlib)──▶ extension.py
 *      ◀──(spiration_extension_desc)──
 * ```
 *
 * 线程与 GIL 约定：
 * - 宿主保证所有回调都在宿主主线程执行，符合 C API 的线程模型；
 * - 解释器初始化后立即 `PyEval_SaveThread()` 释放 GIL，
 *   此后每次进入 Python 都由 `gil_guard` 获取、退出时释放；
 * - 脚本在自建线程中调用宿主能力时，必须先 `spiration.dispatch(...)`
 *   把操作送回主线程（`host->dispatch` 是唯一线程安全的宿主函数）。
 *
 * 生命周期注意事项：宿主可能长期持有菜单项与控件回调（例如用户点击
 * 由脚本创建的按钮，而脚本已卸载），因此这类回调载体在脚本关闭后被
 * **停用并搬入加载器级坟场**，直到加载器自身卸载才释放。
 */

#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <spiration/api.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace spiration {
namespace python_loader {

/** @brief 加载器拓展 ID。 */
inline const char* loader_id() { return "com.flamence.spiration.python"; }

/** @brief 注入模块名。 */
inline const char* module_name() { return "spiration"; }

/**
 * @brief 本加载器所编译链接的 CPython 版本（`major.minor.patch`）。
 * @note 与解释器动态库名、标准库布局绑定，由构建期强制校验，
 *       详见 `cmake/ConfigureDependencies.cmake`。
 */
#ifndef SPIRATION_PYTHON_VERSION_STRING
#define SPIRATION_PYTHON_VERSION_STRING "3.11.9"
#endif

/** @brief 去掉点号的紧凑版本号，用于 `Python311` 形式的目录名。 */
#ifndef SPIRATION_PYTHON_VERSION_COMPACT
#define SPIRATION_PYTHON_VERSION_COMPACT "311"
#endif

/**
 * @brief 脚本描述符胶囊名。
 * @note 仅此一处定义，保证胶囊名校验时的指针唯一。
 */
const char* script_capsule_name();

/** @brief 回调载体（定义见下）。 */
struct listener_ref;
/** @brief 投递任务载体（定义见下）。 */
struct dispatch_task;

/**
 * @brief 单个脚本拓展的状态。
 *
 * @note 本对象由加载器在堆上分配、直到 `release` 才销毁；
 *       描述符中的 `spiration_str_view` 借用其中的字符串，
 *       因此本对象一经创建便不可移动或复制。
 */
struct script_state {
    /** 拓展 ID。 */
    std::string id;
    /** 拓展安装目录。 */
    std::string directory;
    /** 入口脚本路径。 */
    std::string entry;
    /** 清单原文。 */
    std::string manifest;

    /** 身份字段，供描述符借用。 */
    std::string name;
    std::string description;
    std::string version;

    /** 已加载的脚本模块。 */
    PyObject* module = nullptr;
    /** 注入的 `spiration` 模块。 */
    PyObject* injected = nullptr;
    /** 模块在 `sys.modules` 中的键名。 */
    std::string module_key;

    /** 回调登记：句柄 → 载体。 */
    std::map<int64_t, listener_ref*> listeners;
    /** 下一个回调句柄。 */
    int64_t next_callback = 1;
    /** `host->dispatch` 待执行任务的登记（用于关闭时清理）。 */
    std::set<dispatch_task*> pending_tasks;

    /** 返回给宿主的描述符。必须位于上述字符串之后。 */
    spiration_extension_desc desc;
};

/**
 * @brief 回调载体：把一个 Python 可调用对象映射为一个宿主回调。
 * @note 该对象一经创建便不可移动，其地址即 `user_data`。
 */
struct listener_ref {
    /** 所属脚本；关闭后置空表示已停用。 */
    script_state* script = nullptr;
    /** Python 可调用对象，持有引用。 */
    PyObject* callable = nullptr;
    /** 是否可能被宿主长期持有（菜单、控件回调）。 */
    bool persistent = false;
};

/** @brief `spiration.dispatch` 的任务载体。 */
struct dispatch_task {
    /** 所属脚本。 */
    script_state* script = nullptr;
    /** 可调用对象，持有引用。 */
    PyObject* callable = nullptr;
};

/**
 * @brief 服务条目：把 Python 对象暴露给其他脚本。
 */
struct service_entry {
    /** Python 服务对象，持有引用；脚本卸载后置空。 */
    PyObject* object = nullptr;
    /** 提供者拓展 ID。 */
    std::string owner;
    /** 服务名。 */
    std::string name;
};

/** @brief GIL 守卫。 */
class gil_guard {
public:
    gil_guard() : state_(PyGILState_Ensure()) {}
    ~gil_guard() { PyGILState_Release(state_); }

    gil_guard(const gil_guard&) = delete;
    gil_guard& operator=(const gil_guard&) = delete;

private:
    PyGILState_STATE state_;
};

/** @brief 取当前线程正在执行的脚本；不在脚本执行中时为 `nullptr`。 */
script_state* current_script();

/** @brief 直接设定当前脚本。 */
void set_current_script(script_state* script);

/**
 * @brief 在宿主回调期间设定「当前脚本」，析构时恢复。
 * @note 事件可能引发脚本之间的再入（脚本 A 发布事件、脚本 B 的监听器被调用），
 *       因此必须保存并恢复上一个值。
 */
class script_scope {
public:
    explicit script_scope(script_state* script)
        : previous_(current_script()) {
        set_current_script(script);
    }

    ~script_scope() { set_current_script(previous_); }

    script_scope(const script_scope&) = delete;
    script_scope& operator=(const script_scope&) = delete;

private:
    script_state* previous_;
};

/* ------------------------------------------------------------------------- */
/* runtime.cpp                                                               */
/* ------------------------------------------------------------------------- */

/**
 * @brief 启动解释器（若已被其他方启动则直接复用）。
 *
 * 查找顺序：`SPIRATION_PYTHON_HOME` / `PYTHONHOME` → 在**目标环境**中寻找
 * 可用的解释器根目录（加载器邻近目录、注册表、常见安装位置、`PATH`）→
 * 交由 CPython 自行探测。
 *
 * @param error 失败原因（含已尝试的候选），可为 `nullptr`。
 * @return 成功返回 `true`。
 */
bool initialize_interpreter(std::string* error);

/**
 * @brief 关闭由本加载器启动的解释器。
 */
void finalize_interpreter();

/**
 * @brief 判断解释器是否已启动。
 */
bool interpreter_ready();

/**
 * @brief 实际用于启动解释器的根目录。
 * @return 根目录；由 CPython 自行探测时返回空串。
 */
std::string python_home();

/**
 * @brief 为脚本拓展追加模块搜索路径（需持有 GIL）。
 * @note 追加 `<dir>`、`<dir>/python`、`<dir>/lib`、`<dir>/site-packages`。
 */
void add_script_paths(const std::string& directory);

/**
 * @brief 由文件路径导入脚本模块（需持有 GIL）。
 * @param script 脚本状态。
 * @return 新引用；失败返回 `nullptr` 并已设置 Python 异常。
 */
PyObject* import_script_module(script_state* script);

/**
 * @brief 把当前 Python 异常写入宿主日志（需持有 GIL）。
 * @param script  脚本状态，用于获取宿主函数表。
 * @param context 上下文描述，如 `"initialize"`。
 */
void report_python_error(script_state* script, const char* context);

/**
 * @brief 停用并回收脚本的全部回调与待执行任务（需持有 GIL）。
 */
void release_callbacks(script_state* script);

/**
 * @brief 释放加载器级坟场（需持有 GIL），应在解释器收尾之前调用。
 */
void release_graveyard();

/**
 * @brief 回收某脚本登记的服务条目（需持有 GIL）。
 */
void release_services(const std::string& owner);

/**
 * @brief 取 `std::string` 对应的 UTF-8 视图。
 */
spiration_str_view to_view(const std::string& text);

/**
 * @brief 由 UTF-8 视图取 `std::string`。
 */
std::string to_string(spiration_str_view text);

/* ------------------------------------------------------------------------- */
/* module.cpp                                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief 构造注入模块 `spiration`（需持有 GIL）。
 * @return 新引用；失败返回 `nullptr`。
 */
PyObject* build_injected_module();

/**
 * @brief `spiration_value` → Python 对象（需持有 GIL，返回新引用）。
 */
PyObject* value_to_python(const spiration_value& value);

/**
 * @brief Python 对象 → `spiration_value`（需持有 GIL）。
 * @return 不支持的类型返回 `false`。
 */
bool python_to_value(PyObject* object, spiration_value* out);

/**
 * @brief 记录一次回调异常（需持有 GIL）。
 */
void report_callback_error(script_state* script, const char* context);

/**
 * @brief 登记一个回调载体（需持有 GIL）。
 * @param script     脚本状态。
 * @param callable   可调用对象（借用，函数内会 `Py_INCREF`）。
 * @param persistent 宿主是否可能长期持有该回调。
 * @return 句柄（> 0）；失败返回 0。
 */
int64_t register_listener(script_state* script, PyObject* callable,
                          bool persistent);

/**
 * @brief 查询回调载体（需持有 GIL）。
 */
listener_ref* lookup_listener(script_state* script, int64_t handle);

/** @brief 宿主事件回调，`user_data` 为 `listener_ref*`。 */
void SPIRATION_CALL event_trampoline(void* user_data, spiration_str_view event,
                                     spiration_str_view data);

/** @brief 宿主菜单回调，`user_data` 为 `listener_ref*`。 */
void SPIRATION_CALL menu_trampoline(void* user_data);

/** @brief 宿主控件回调，`user_data` 为 `listener_ref*`。 */
void SPIRATION_CALL widget_trampoline(void* user_data,
                                      spiration_widget* widget,
                                      spiration_str_view event,
                                      spiration_value data);

/** @brief 宿主投递回调，`user_data` 为 `dispatch_task*`。 */
void SPIRATION_CALL dispatch_trampoline(void* user_data);

/* ------------------------------------------------------------------------- */
/* loader.cpp                                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief 登记一个 Python 服务对象（需持有 GIL）。
 * @return 服务条目指针，可直接作为 `void*` 交给宿主。
 */
service_entry* register_python_service(script_state* script, PyObject* object);

/**
 * @brief 由宿主返回的 `void*` 复原 Python 服务对象（需持有 GIL）。
 * @return 借用引用；非 Python 服务返回 `nullptr`。
 */
PyObject* lookup_python_service(void* handle);

} // namespace python_loader
} // namespace spiration
