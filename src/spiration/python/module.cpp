/**
 * @file module.cpp
 * @brief 注入模块 `spiration` 的构造与实现。
 * @author 陈林锴
 *
 * 每个脚本拓展都会拿到一份**独立**的 `spiration` 模块实例：
 * 模块中的函数以「胶囊」作为 `self`，胶囊里存着该脚本的 `script_state`，
 * 因此同名函数可以被多个脚本同时使用而互不干扰，也可以被安全地
 * 挂到子命名空间（`spiration.events`、`spiration.ui`）下。
 */

#include "internal.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace spiration {
namespace python {
namespace loader {

namespace {

/* -------------------------------------------------------------------------
 * 胶囊与参数工具
 * ---------------------------------------------------------------------- */

/** @brief 取宿主的界面接口，不支持时返回 `nullptr`。 */
const spiration_ui_api* ui_of(script_state* script) {
    if (script == nullptr || script->desc.host == nullptr ||
        script->desc.host->query_interface == nullptr) {
        return nullptr;
    }
    const std::string name(SPIRATION_UI_INTERFACE_NAME);
    return static_cast<const spiration_ui_api*>(script->desc.host->query_interface(
        &script->desc, to_view(name), SPIRATION_UI_VERSION));
}

/** @brief 参数错误。 */
PyObject* arg_error(const char* name, const char* expected) {
    PyErr_Format(PyExc_TypeError, "%s: expected %s", name, expected);
    return nullptr;
}

/** @brief 运行时错误，附带宿主错误详情。 */
PyObject* host_error(script_state* script, const char* name) {
    std::string detail;
    if (script != nullptr && script->desc.host != nullptr &&
        script->desc.host->last_error != nullptr) {
        detail = to_string(script->desc.host->last_error(&script->desc));
    }
    if (detail.empty()) {
        PyErr_Format(PyExc_RuntimeError, "%s failed", name);
    } else {
        PyErr_Format(PyExc_RuntimeError, "%s failed: %s", name, detail.c_str());
    }
    return nullptr;
}

/** @brief 取 `str` 参数。 */
bool text_arg(PyObject* object, std::string* out) {
    if (object == nullptr || !PyUnicode_Check(object)) return false;
    Py_ssize_t size = 0;
    const char* utf8 = PyUnicode_AsUTF8AndSize(object, &size);
    if (utf8 == nullptr) {
        PyErr_Clear();
        return false;
    }
    out->assign(utf8, static_cast<size_t>(size));
    return true;
}

/** @brief 取整数参数。 */
bool int_arg(PyObject* object, int64_t* out) {
    if (object == nullptr || !PyLong_Check(object)) return false;
    const long long value = PyLong_AsLongLong(object);
    if (value == -1 && PyErr_Occurred() != nullptr) {
        PyErr_Clear();
        return false;
    }
    *out = static_cast<int64_t>(value);
    return true;
}

/** @brief 整数 → 宿主句柄。 */
void* handle_arg(int64_t value) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(value));
}

/** @brief 宿主句柄 → 整数。 */
int64_t handle_value(const void* handle) {
    return static_cast<int64_t>(reinterpret_cast<uintptr_t>(handle));
}

/** @brief 简易的位置参数视图。 */
class args_view {
public:
    explicit args_view(PyObject* args)
        : args_(args), count_(args != nullptr ? PyTuple_GET_SIZE(args) : 0) {}

    Py_ssize_t count() const { return count_; }

    PyObject* at(Py_ssize_t index) const {
        return index < count_ ? PyTuple_GET_ITEM(args_, index) : nullptr;
    }

private:
    PyObject* args_;
    Py_ssize_t count_;
};

/* -------------------------------------------------------------------------
 * 字符串值的暂存：宿主借用视图，仅在下一次转换前有效
 * ---------------------------------------------------------------------- */

thread_local std::string g_value_text;

/* -------------------------------------------------------------------------
 * 方法实现
 * ---------------------------------------------------------------------- */

/** @brief 记录一条日志。 */
void log_at(script_state* script, spiration_log_level level, PyObject* args) {
    args_view view(args);
    std::string message;
    std::string tag;
    if (!text_arg(view.at(0), &message)) {
        arg_error("log", "str message");
        return;
    }
    if (view.count() > 1 && view.at(1) != nullptr && view.at(1) != Py_None &&
        !text_arg(view.at(1), &tag)) {
        arg_error("log", "str tag");
        return;
    }
    if (script->desc.host != nullptr && script->desc.host->log != nullptr) {
        script->desc.host->log(&script->desc, level, to_view(message),
                               to_view(tag));
    }
}

PyObject* m_log(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    if (script == nullptr) Py_RETURN_NONE;
    args_view view(args);
    int64_t level = SPIRATION_LOG_INFO;
    if (!int_arg(view.at(0), &level)) return arg_error("log", "int level");
    std::string message;
    std::string tag;
    if (!text_arg(view.at(1), &message)) {
        return arg_error("log", "str message");
    }
    if (view.count() > 2) {
        if (!text_arg(view.at(2), &tag)) return arg_error("log", "str tag");
    }
    const spiration_log_level clamped =
        level < SPIRATION_LOG_TRACE || level > SPIRATION_LOG_ERROR
            ? SPIRATION_LOG_INFO
            : static_cast<spiration_log_level>(level);
    if (script->desc.host != nullptr && script->desc.host->log != nullptr) {
        script->desc.host->log(&script->desc, clamped, to_view(message),
                               to_view(tag));
    }
    Py_RETURN_NONE;
}

#define SPIRATION_PY_LOG_LEVEL(fn_name, level_value)                \
    PyObject* fn_name(PyObject* self, PyObject* args) {             \
        script_state* script = current_script();                     \
        if (script != nullptr) log_at(script, level_value, args);    \
        if (PyErr_Occurred() != nullptr) return nullptr;             \
        Py_RETURN_NONE;                                              \
    }

SPIRATION_PY_LOG_LEVEL(m_log_trace, SPIRATION_LOG_TRACE)
SPIRATION_PY_LOG_LEVEL(m_log_debug, SPIRATION_LOG_DEBUG)
SPIRATION_PY_LOG_LEVEL(m_log_info, SPIRATION_LOG_INFO)
SPIRATION_PY_LOG_LEVEL(m_log_warning, SPIRATION_LOG_WARNING)
SPIRATION_PY_LOG_LEVEL(m_log_error, SPIRATION_LOG_ERROR)

#undef SPIRATION_PY_LOG_LEVEL

/** @brief 翻译文本。 */
PyObject* m_tr(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    if (script == nullptr || script->desc.host == nullptr) Py_RETURN_NONE;

    std::string key;
    if (!text_arg(view.at(0), &key)) return arg_error("tr", "str key");

    std::vector<std::string> values;
    for (Py_ssize_t i = 1; i < view.count(); ++i) {
        std::string value;
        if (!text_arg(view.at(i), &value)) return arg_error("tr", "str arguments");
        values.push_back(value);
    }

    std::vector<spiration_str_view> views;
    views.reserve(values.size());
    for (size_t i = 0; i < values.size(); ++i) views.push_back(to_view(values[i]));

    const spiration_str_view result = script->desc.host->translate(
        &script->desc, to_view(key), views.empty() ? nullptr : views.data(),
        views.size());
    return PyUnicode_FromStringAndSize(result.data != nullptr ? result.data : "",
                                       static_cast<Py_ssize_t>(result.length));
}

/** @brief 载入翻译文件。 */
PyObject* m_load_translations(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    std::string path;
    if (script == nullptr) Py_RETURN_NONE;
    if (!text_arg(view.at(0), &path)) {
        return arg_error("load_translations", "str path");
    }
    if (script->desc.host->load_translations(&script->desc, to_view(path)) !=
        SPIRATION_OK) {
        return host_error(script, "load_translations");
    }
    Py_RETURN_NONE;
}

/** @brief 取拓展安装目录下的路径。 */
PyObject* m_extension_path(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    std::string relative;
    if (script == nullptr) Py_RETURN_NONE;
    if (view.count() > 0 && !text_arg(view.at(0), &relative)) {
        return arg_error("extension_path", "str relative");
    }
    const spiration_str_view result =
        script->desc.host->extension_path(&script->desc, to_view(relative));
    return PyUnicode_FromStringAndSize(result.data != nullptr ? result.data : "",
                                       static_cast<Py_ssize_t>(result.length));
}

/** @brief 取拓展私有数据目录下的路径。 */
PyObject* m_data_path(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    std::string relative;
    if (script == nullptr) Py_RETURN_NONE;
    if (view.count() > 0 && !text_arg(view.at(0), &relative)) {
        return arg_error("data_path", "str relative");
    }
    const spiration_str_view result =
        script->desc.host->data_path(&script->desc, to_view(relative));
    return PyUnicode_FromStringAndSize(result.data != nullptr ? result.data : "",
                                       static_cast<Py_ssize_t>(result.length));
}

/** @brief 当前线程是否为主线程。 */
PyObject* m_is_host_thread(PyObject* self, PyObject* args) {
    SPIRATION_UNUSED(args);
    script_state* script = current_script();
    if (script == nullptr) Py_RETURN_FALSE;
    return PyBool_FromLong(
        script->desc.host->is_host_thread(&script->desc) == SPIRATION_TRUE ? 1 : 0);
}

/**
 * @brief 实际用于启动解释器的根目录。
 * @return 根目录字符串；由 CPython 自行探测时返回空串。
 * @note 排查「装载器找不到 Python」时最有用的一项诊断信息。
 */
PyObject* m_python_home(PyObject* self, PyObject* args) {
    SPIRATION_UNUSED(self);
    SPIRATION_UNUSED(args);
    const std::string home = python_home();
    return PyUnicode_FromStringAndSize(home.data(),
                                       static_cast<Py_ssize_t>(home.size()));
}

/** @brief 本加载器所链接的 CPython 版本。 */
PyObject* m_python_version(PyObject* self, PyObject* args) {
    SPIRATION_UNUSED(self);
    SPIRATION_UNUSED(args);
    return PyUnicode_FromString(SPIRATION_PYTHON_VERSION_STRING);
}

/** @brief 投递可调用对象到宿主主线程。 */
PyObject* m_dispatch(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    if (script == nullptr) Py_RETURN_NONE;

    PyObject* callable = view.at(0);
    if (callable == nullptr || !PyCallable_Check(callable)) {
        return arg_error("dispatch", "callable");
    }

    dispatch_task* task = new dispatch_task();
    task->script = script;
    task->callable = callable;
    Py_INCREF(callable);
    script->pending_tasks.insert(task);

    if (script->desc.host->dispatch(&script->desc, dispatch_trampoline, task) !=
        SPIRATION_OK) {
        script->pending_tasks.erase(task);
        Py_DECREF(task->callable);
        delete task;
        return host_error(script, "dispatch");
    }
    Py_RETURN_NONE;
}

/** @brief 发布事件。 */
PyObject* m_emit(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    std::string event;
    std::string data;
    if (script == nullptr) Py_RETURN_NONE;
    if (!text_arg(view.at(0), &event)) return arg_error("emit", "str event");
    if (view.count() > 1 && view.at(1) != Py_None &&
        !text_arg(view.at(1), &data)) {
        return arg_error("emit", "str data");
    }
    if (script->desc.host->publish(&script->desc, to_view(event), to_view(data)) !=
        SPIRATION_OK) {
        return host_error(script, "emit");
    }
    Py_RETURN_NONE;
}

/** @brief 订阅事件。 */
PyObject* m_on_event(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    std::string event;
    if (script == nullptr) Py_RETURN_NONE;
    if (!text_arg(view.at(0), &event)) return arg_error("on_event", "str event");

    PyObject* callable = view.at(1);
    if (callable == nullptr || !PyCallable_Check(callable)) {
        return arg_error("on_event", "callable");
    }

    listener_ref* ref = new listener_ref();
    ref->script = script;
    ref->callable = callable;
    Py_INCREF(callable);

    const int64_t handle = script->desc.host->subscribe(
        &script->desc, to_view(event), event_trampoline, ref);
    if (handle <= 0) {
        Py_DECREF(callable);
        delete ref;
        return host_error(script, "on_event");
    }

    script->listeners[handle] = ref;
    return PyLong_FromLongLong(static_cast<long long>(handle));
}

/** @brief 取消订阅。 */
PyObject* m_off_event(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    int64_t handle = 0;
    if (script == nullptr) Py_RETURN_NONE;
    if (!int_arg(view.at(0), &handle)) return arg_error("off_event", "int handle");

    script->desc.host->unsubscribe(&script->desc, handle);

    std::map<int64_t, listener_ref*>::iterator it = script->listeners.find(handle);
    if (it != script->listeners.end()) {
        Py_XDECREF(it->second->callable);
        delete it->second;
        script->listeners.erase(it);
    }
    Py_RETURN_NONE;
}

/** @brief 注册命名服务（服务对象可为任意 Python 对象）。 */
PyObject* m_register_service(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    std::string name;
    if (script == nullptr) Py_RETURN_NONE;
    if (!text_arg(view.at(0), &name)) {
        return arg_error("register_service", "str name");
    }
    PyObject* service = view.at(1);
    if (service == nullptr) return arg_error("register_service", "service object");

    service_entry* entry = register_python_service(script, service);
    if (entry == nullptr) {
        PyErr_NoMemory();
        return nullptr;
    }
    if (script->desc.host->register_service(&script->desc, to_view(name),
                                            entry) != SPIRATION_OK) {
        return host_error(script, "register_service");
    }
    Py_RETURN_NONE;
}

/** @brief 查询其他拓展的服务。 */
PyObject* m_query_service(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    std::string extension_id;
    std::string name;
    if (script == nullptr) Py_RETURN_NONE;
    if (!text_arg(view.at(0), &extension_id)) {
        return arg_error("query_service", "str extension_id");
    }
    if (!text_arg(view.at(1), &name)) {
        return arg_error("query_service", "str name");
    }

    void* raw = script->desc.host->query_service(&script->desc,
                                                 to_view(extension_id),
                                                 to_view(name));
    if (raw == nullptr) Py_RETURN_NONE;

    PyObject* object = lookup_python_service(raw);
    if (object != nullptr) {
        Py_INCREF(object);
        return object;
    }
    /* 非 Python 服务：以不透明整数形式交回脚本。 */
    return PyLong_FromVoidPtr(raw);
}

/** @brief 列出所有已装载拓展。 */
PyObject* m_extensions(PyObject* self, PyObject* args) {
    SPIRATION_UNUSED(args);
    script_state* script = current_script();
    if (script == nullptr) Py_RETURN_NONE;

    const spiration_host_api* host = script->desc.host;
    const size_t count =
        host->enumerate_extensions(&script->desc, nullptr, static_cast<size_t>(0));
    std::vector<const spiration_extension_desc*> all(count);
    if (count > 0) {
        host->enumerate_extensions(&script->desc, all.data(), count);
    }

    PyObject* list = PyList_New(0);
    if (list == nullptr) return nullptr;
    for (size_t i = 0; i < all.size(); ++i) {
        const spiration_extension_desc* other = all[i];
        if (other == nullptr) continue;

        PyObject* item = PyDict_New();
        if (item == nullptr) {
            Py_DECREF(list);
            return nullptr;
        }
        const std::string id = to_string(other->id);
        const std::string name = to_string(other->name);
        const std::string version = to_string(other->version_string);
        const std::string description = to_string(other->description);

        PyObject* value = PyUnicode_FromStringAndSize(id.data(), (Py_ssize_t)id.size());
        PyDict_SetItemString(item, "id", value);
        Py_XDECREF(value);
        value = PyUnicode_FromStringAndSize(name.data(), (Py_ssize_t)name.size());
        PyDict_SetItemString(item, "name", value);
        Py_XDECREF(value);
        value = PyUnicode_FromStringAndSize(version.data(), (Py_ssize_t)version.size());
        PyDict_SetItemString(item, "version", value);
        Py_XDECREF(value);
        value = PyUnicode_FromStringAndSize(description.data(),
                                            (Py_ssize_t)description.size());
        PyDict_SetItemString(item, "description", value);
        Py_XDECREF(value);
        value = PyLong_FromLong(static_cast<long>(other->phase));
        PyDict_SetItemString(item, "phase", value);
        Py_XDECREF(value);

        PyList_Append(list, item);
        Py_DECREF(item);
    }
    return list;
}

/* -------------------------------------------------------------------------
 * 界面接口
 * ---------------------------------------------------------------------- */

/** @brief 宿主是否提供界面接口。 */
PyObject* m_ui_supported(PyObject* self, PyObject* args) {
    SPIRATION_UNUSED(self);
    SPIRATION_UNUSED(args);
    return PyBool_FromLong(ui_of(current_script()) != nullptr ? 1 : 0);
}

PyObject* m_ui_request_repaint(PyObject* self, PyObject* args) {
    SPIRATION_UNUSED(args);
    script_state* script = current_script();
    const spiration_ui_api* ui = ui_of(script);
    if (ui == nullptr) Py_RETURN_NONE;
    ui->request_repaint(&script->desc);
    Py_RETURN_NONE;
}

PyObject* m_ui_add_menu_item(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    std::string menu;
    std::string label;
    if (script == nullptr) Py_RETURN_NONE;

    const spiration_ui_api* ui = ui_of(script);
    if (ui == nullptr) return host_error(script, "ui.add_menu_item");

    if (!text_arg(view.at(0), &menu)) {
        return arg_error("ui.add_menu_item", "str menu");
    }
    if (!text_arg(view.at(1), &label)) {
        return arg_error("ui.add_menu_item", "str label");
    }
    PyObject* callable = view.at(2);
    if (callable == nullptr || !PyCallable_Check(callable)) {
        return arg_error("ui.add_menu_item", "callable");
    }

    const int64_t handle = register_listener(script, callable, true);
    if (handle == 0) return nullptr;
    listener_ref* ref = lookup_listener(script, handle);

    if (ui->add_menu_item(&script->desc, to_view(menu), to_view(label),
                          menu_trampoline, ref) != SPIRATION_OK) {
        return host_error(script, "ui.add_menu_item");
    }
    Py_RETURN_NONE;
}

PyObject* m_ui_tab_open(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    std::string title;
    if (script == nullptr) Py_RETURN_NONE;
    const spiration_ui_api* ui = ui_of(script);
    if (ui == nullptr) return host_error(script, "ui.tab_open");
    if (!text_arg(view.at(0), &title)) return arg_error("ui.tab_open", "str title");

    spiration_tab* tab = ui->tab_open(&script->desc, to_view(title));
    if (tab == nullptr) return host_error(script, "ui.tab_open");
    return PyLong_FromLongLong(handle_value(tab));
}

PyObject* m_ui_tab_close(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    int64_t handle = 0;
    if (script == nullptr) Py_RETURN_NONE;
    const spiration_ui_api* ui = ui_of(script);
    if (ui == nullptr) Py_RETURN_NONE;
    if (!int_arg(view.at(0), &handle)) return arg_error("ui.tab_close", "int tab");
    ui->tab_close(reinterpret_cast<spiration_tab*>(handle_arg(handle)));
    Py_RETURN_NONE;
}

PyObject* m_ui_tab_activate(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    int64_t handle = 0;
    if (script == nullptr) Py_RETURN_NONE;
    const spiration_ui_api* ui = ui_of(script);
    if (ui == nullptr) Py_RETURN_FALSE;
    if (!int_arg(view.at(0), &handle)) return arg_error("ui.tab_activate", "int tab");
    spiration_tab* tab = reinterpret_cast<spiration_tab*>(handle_arg(handle));
    return PyBool_FromLong(ui->tab_activate(tab) == SPIRATION_OK ? 1 : 0);
}

PyObject* m_ui_tab_set_title(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    int64_t handle = 0;
    std::string title;
    if (script == nullptr) Py_RETURN_NONE;
    const spiration_ui_api* ui = ui_of(script);
    if (ui == nullptr) return host_error(script, "ui.tab_set_title");
    if (!int_arg(view.at(0), &handle)) return arg_error("ui.tab_set_title", "int tab");
    if (!text_arg(view.at(1), &title)) return arg_error("ui.tab_set_title", "str title");
    spiration_tab* tab = reinterpret_cast<spiration_tab*>(handle_arg(handle));
    if (ui->tab_set_title(tab, to_view(title)) != SPIRATION_OK) {
        return host_error(script, "ui.tab_set_title");
    }
    Py_RETURN_NONE;
}

PyObject* m_ui_tab_set_content(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    int64_t tab_handle = 0;
    int64_t widget_handle = 0;
    if (script == nullptr) Py_RETURN_NONE;
    const spiration_ui_api* ui = ui_of(script);
    if (ui == nullptr) return host_error(script, "ui.tab_set_content");
    if (!int_arg(view.at(0), &tab_handle)) {
        return arg_error("ui.tab_set_content", "int tab");
    }
    if (view.count() > 1 && view.at(1) != Py_None) {
        if (!int_arg(view.at(1), &widget_handle)) {
            return arg_error("ui.tab_set_content", "int widget");
        }
    }
    spiration_tab* tab = reinterpret_cast<spiration_tab*>(handle_arg(tab_handle));
    spiration_widget* content = widget_handle == 0
                                    ? nullptr
                                    : reinterpret_cast<spiration_widget*>(
                                          handle_arg(widget_handle));
    if (ui->tab_set_content(tab, content) != SPIRATION_OK) {
        return host_error(script, "ui.tab_set_content");
    }
    Py_RETURN_NONE;
}

PyObject* m_ui_tab_is_alive(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    int64_t handle = 0;
    if (script == nullptr) Py_RETURN_FALSE;
    const spiration_ui_api* ui = ui_of(script);
    if (ui == nullptr) Py_RETURN_FALSE;
    if (!int_arg(view.at(0), &handle)) return arg_error("ui.tab_is_alive", "int tab");
    return PyBool_FromLong(ui->tab_is_alive(
                               reinterpret_cast<spiration_tab*>(handle_arg(handle))) ==
                                   SPIRATION_TRUE
                               ? 1
                               : 0);
}

PyObject* m_ui_widget_create(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    int64_t kind = 0;
    if (script == nullptr) Py_RETURN_NONE;
    const spiration_ui_api* ui = ui_of(script);
    if (ui == nullptr) return host_error(script, "ui.widget_create");
    if (!int_arg(view.at(0), &kind)) return arg_error("ui.widget_create", "int kind");

    spiration_widget* widget =
        ui->widget_create(static_cast<spiration_widget_kind>(kind));
    if (widget == nullptr) return host_error(script, "ui.widget_create");
    return PyLong_FromLongLong(handle_value(widget));
}

PyObject* m_ui_widget_destroy(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    int64_t handle = 0;
    if (script == nullptr) Py_RETURN_NONE;
    const spiration_ui_api* ui = ui_of(script);
    if (ui == nullptr) Py_RETURN_NONE;
    if (!int_arg(view.at(0), &handle)) {
        return arg_error("ui.widget_destroy", "int widget");
    }
    ui->widget_destroy(reinterpret_cast<spiration_widget*>(handle_arg(handle)));
    Py_RETURN_NONE;
}

PyObject* m_ui_widget_append(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    int64_t parent_handle = 0;
    int64_t child_handle = 0;
    if (script == nullptr) Py_RETURN_NONE;
    const spiration_ui_api* ui = ui_of(script);
    if (ui == nullptr) return host_error(script, "ui.widget_append");
    if (!int_arg(view.at(0), &parent_handle) || !int_arg(view.at(1), &child_handle)) {
        return arg_error("ui.widget_append", "int parent, int child");
    }
    if (ui->widget_append(
            reinterpret_cast<spiration_widget*>(handle_arg(parent_handle)),
            reinterpret_cast<spiration_widget*>(handle_arg(child_handle))) !=
        SPIRATION_OK) {
        return host_error(script, "ui.widget_append");
    }
    Py_RETURN_NONE;
}

PyObject* m_ui_widget_set(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    int64_t handle = 0;
    std::string property;
    if (script == nullptr) Py_RETURN_NONE;
    const spiration_ui_api* ui = ui_of(script);
    if (ui == nullptr) return host_error(script, "ui.widget_set");
    if (!int_arg(view.at(0), &handle)) return arg_error("ui.widget_set", "int widget");
    if (!text_arg(view.at(1), &property)) {
        return arg_error("ui.widget_set", "str property");
    }

    spiration_value value = spiration_value_none();
    if (view.count() > 2 && !python_to_value(view.at(2), &value)) {
        return arg_error("ui.widget_set", "bool / int / float / str value");
    }
    if (ui->widget_set(reinterpret_cast<spiration_widget*>(handle_arg(handle)),
                       to_view(property), value) != SPIRATION_OK) {
        return host_error(script, "ui.widget_set");
    }
    Py_RETURN_NONE;
}

PyObject* m_ui_widget_get(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    int64_t handle = 0;
    std::string property;
    if (script == nullptr) Py_RETURN_NONE;
    const spiration_ui_api* ui = ui_of(script);
    if (ui == nullptr) return host_error(script, "ui.widget_get");
    if (!int_arg(view.at(0), &handle)) return arg_error("ui.widget_get", "int widget");
    if (!text_arg(view.at(1), &property)) {
        return arg_error("ui.widget_get", "str property");
    }

    spiration_value value = spiration_value_none();
    if (ui->widget_get(reinterpret_cast<const spiration_widget*>(handle_arg(handle)),
                       to_view(property), &value) != SPIRATION_OK) {
        return host_error(script, "ui.widget_get");
    }
    return value_to_python(value);
}

PyObject* m_ui_widget_listen(PyObject* self, PyObject* args) {
    script_state* script = current_script();
    args_view view(args);
    int64_t handle = 0;
    std::string event;
    if (script == nullptr) Py_RETURN_NONE;
    const spiration_ui_api* ui = ui_of(script);
    if (ui == nullptr) return host_error(script, "ui.widget_listen");
    if (!int_arg(view.at(0), &handle)) {
        return arg_error("ui.widget_listen", "int widget");
    }
    if (!text_arg(view.at(1), &event)) {
        return arg_error("ui.widget_listen", "str event");
    }
    PyObject* callable = view.at(2);
    if (callable == nullptr || !PyCallable_Check(callable)) {
        return arg_error("ui.widget_listen", "callable");
    }

    const int64_t listener = register_listener(script, callable, true);
    if (listener == 0) return nullptr;
    listener_ref* ref = lookup_listener(script, listener);

    if (ui->widget_listen(
            reinterpret_cast<spiration_widget*>(handle_arg(handle)),
            to_view(event), widget_trampoline, ref) != SPIRATION_OK) {
        return host_error(script, "ui.widget_listen");
    }
    Py_RETURN_NONE;
}

/* -------------------------------------------------------------------------
 * 方法表
 * ---------------------------------------------------------------------- */

PyMethodDef k_module_methods[] = {
    {"log", m_log, METH_VARARGS,
     "log(level, message, tag=\"\") -> None\n\n记录一条指定等级的日志。"},
    {"log_trace", m_log_trace, METH_VARARGS, "log_trace(message, tag=\"\") -> None"},
    {"log_debug", m_log_debug, METH_VARARGS, "log_debug(message, tag=\"\") -> None"},
    {"log_info", m_log_info, METH_VARARGS, "log_info(message, tag=\"\") -> None"},
    {"log_warning", m_log_warning, METH_VARARGS, "log_warning(message, tag=\"\") -> None"},
    {"log_error", m_log_error, METH_VARARGS, "log_error(message, tag=\"\") -> None"},
    {"tr", m_tr, METH_VARARGS, "tr(key, *args) -> str\n\n翻译文本。"},
    {"load_translations", m_load_translations, METH_VARARGS,
     "load_translations(path) -> None"},
    {"extension_path", m_extension_path, METH_VARARGS,
     "extension_path(relative=\"\") -> str"},
    {"data_path", m_data_path, METH_VARARGS, "data_path(relative=\"\") -> str"},
    {"is_host_thread", m_is_host_thread, METH_NOARGS,
     "is_host_thread() -> bool"},
    {"python_home", m_python_home, METH_NOARGS,
     "python_home() -> str\n\n"
     "实际用于启动解释器的根目录；由 CPython 自行探测时返回空串。"},
    {"python_version", m_python_version, METH_NOARGS,
     "python_version() -> str\n\n本加载器所链接的 CPython 版本。"},
    {"dispatch", m_dispatch, METH_VARARGS,
     "dispatch(callable) -> None\n\n把可调用对象投递到宿主主线程。"},
    {"emit", m_emit, METH_VARARGS, "emit(event, data=\"\") -> None"},
    {"on_event", m_on_event, METH_VARARGS,
     "on_event(event, callback) -> int\n\n订阅事件，返回订阅句柄。"},
    {"off_event", m_off_event, METH_VARARGS, "off_event(handle) -> None"},
    {"register_service", m_register_service, METH_VARARGS,
     "register_service(name, service) -> None"},
    {"query_service", m_query_service, METH_VARARGS,
     "query_service(extension_id, name) -> object"},
    {"extensions", m_extensions, METH_NOARGS, "extensions() -> list[dict]"},
    {"ui_supported", m_ui_supported, METH_NOARGS,
     "ui_supported() -> bool\n\n宿主是否提供 spiration.ui 接口。"},
    {NULL, NULL, 0, NULL}};

PyMethodDef k_ui_methods[] = {
    {"request_repaint", m_ui_request_repaint, METH_NOARGS,
     "request_repaint() -> None"},
    {"add_menu_item", m_ui_add_menu_item, METH_VARARGS,
     "add_menu_item(menu, label, callback) -> None"},
    {"tab_open", m_ui_tab_open, METH_VARARGS, "tab_open(title) -> int"},
    {"tab_close", m_ui_tab_close, METH_VARARGS, "tab_close(tab) -> None"},
    {"tab_activate", m_ui_tab_activate, METH_VARARGS, "tab_activate(tab) -> bool"},
    {"tab_set_title", m_ui_tab_set_title, METH_VARARGS,
     "tab_set_title(tab, title) -> None"},
    {"tab_set_content", m_ui_tab_set_content, METH_VARARGS,
     "tab_set_content(tab, widget) -> None"},
    {"tab_is_alive", m_ui_tab_is_alive, METH_VARARGS,
     "tab_is_alive(tab) -> bool"},
    {"widget_create", m_ui_widget_create, METH_VARARGS,
     "widget_create(kind) -> int"},
    {"widget_destroy", m_ui_widget_destroy, METH_VARARGS,
     "widget_destroy(widget) -> None"},
    {"widget_append", m_ui_widget_append, METH_VARARGS,
     "widget_append(parent, child) -> None"},
    {"widget_set", m_ui_widget_set, METH_VARARGS,
     "widget_set(widget, property, value) -> None"},
    {"widget_get", m_ui_widget_get, METH_VARARGS,
     "widget_get(widget, property) -> object"},
    {"widget_listen", m_ui_widget_listen, METH_VARARGS,
     "widget_listen(widget, event, callback) -> None"},
    {NULL, NULL, 0, NULL}};

/** @brief 在方法表中按名字查找定义。 */
PyMethodDef* find_def(PyMethodDef* table, const char* name) {
    for (PyMethodDef* def = table; def->ml_name != nullptr; ++def) {
        if (std::strcmp(def->ml_name, name) == 0) return def;
    }
    return nullptr;
}

/** @brief 把名字对应的函数绑定到目标模块。 */
bool bind_into(PyMethodDef* table, const char* name, PyObject* target) {
    PyMethodDef* def = find_def(table, name);
    if (def == nullptr) return false;
    PyObject* function = PyCFunction_NewEx(def, NULL, NULL);
    if (function == nullptr) return false;
    PyModule_AddObject(target, name, function); /* 成功时接管引用 */
    return true;
}

} // namespace

const char* script_capsule_name() {
    static const char name[] = "spiration.script";
    return name;
}

PyObject* value_to_python(const spiration_value& value) {
    switch (value.type) {
        case SPIRATION_VALUE_BOOL:
            return PyBool_FromLong(value.data.boolean == SPIRATION_TRUE ? 1 : 0);
        case SPIRATION_VALUE_INT:
            return PyLong_FromLongLong(static_cast<long long>(value.data.integer));
        case SPIRATION_VALUE_FLOAT:
            return PyFloat_FromDouble(value.data.number);
        case SPIRATION_VALUE_STRING:
            return PyUnicode_FromStringAndSize(
                value.data.text.data != nullptr ? value.data.text.data : "",
                static_cast<Py_ssize_t>(value.data.text.length));
        case SPIRATION_VALUE_COLOR:
            return Py_BuildValue("(fff)", value.data.color.r, value.data.color.g,
                                 value.data.color.b);
        case SPIRATION_VALUE_STRINGS: {
            PyObject* list = PyList_New(0);
            if (list == nullptr) return nullptr;
            for (size_t i = 0; i < value.data.strings.count; ++i) {
                const spiration_str_view item = value.data.strings.items[i];
                PyObject* text = PyUnicode_FromStringAndSize(
                    item.data != nullptr ? item.data : "",
                    static_cast<Py_ssize_t>(item.length));
                if (text == nullptr) {
                    Py_DECREF(list);
                    return nullptr;
                }
                PyList_Append(list, text);
                Py_DECREF(text);
            }
            return list;
        }
        case SPIRATION_VALUE_POINTER:
            return PyLong_FromVoidPtr(value.data.pointer);
        case SPIRATION_VALUE_NONE:
        default:
            Py_RETURN_NONE;
    }
}

bool python_to_value(PyObject* object, spiration_value* out) {
    *out = spiration_value_none();
    if (object == nullptr || object == Py_None) return true;

    if (PyBool_Check(object)) {
        *out = spiration_value_bool(object == Py_True ? SPIRATION_TRUE
                                                     : SPIRATION_FALSE);
        return true;
    }
    if (PyLong_Check(object)) {
        const long long value = PyLong_AsLongLong(object);
        if (value == -1 && PyErr_Occurred() != nullptr) {
            PyErr_Clear();
            return false;
        }
        *out = spiration_value_int(static_cast<int64_t>(value));
        return true;
    }
    if (PyFloat_Check(object)) {
        *out = spiration_value_float(PyFloat_AsDouble(object));
        return true;
    }
    if (PyUnicode_Check(object)) {
        Py_ssize_t size = 0;
        const char* utf8 = PyUnicode_AsUTF8AndSize(object, &size);
        if (utf8 == nullptr) {
            PyErr_Clear();
            return false;
        }
        g_value_text.assign(utf8, static_cast<size_t>(size));
        *out = spiration_value_string(to_view(g_value_text));
        return true;
    }
    return false;
}

int64_t register_listener(script_state* script, PyObject* callable,
                          bool persistent) {
    if (script == nullptr || callable == nullptr) return 0;
    listener_ref* ref = new listener_ref();
    ref->script = script;
    ref->callable = callable;
    ref->persistent = persistent;
    Py_INCREF(callable);

    const int64_t handle = script->next_callback++;
    script->listeners[handle] = ref;
    return handle;
}

listener_ref* lookup_listener(script_state* script, int64_t handle) {
    if (script == nullptr) return nullptr;
    std::map<int64_t, listener_ref*>::iterator it = script->listeners.find(handle);
    return it == script->listeners.end() ? nullptr : it->second;
}

namespace {

/** @brief 取 UTF-8 文本对象。 */
PyObject* text_object(spiration_str_view text) {
    return PyUnicode_FromStringAndSize(text.data != nullptr ? text.data : "",
                                       static_cast<Py_ssize_t>(text.length));
}

/** @brief 调用回调并处理返回值与异常。 */
void invoke_listener(listener_ref* ref, PyObject* args, const char* context) {
    PyObject* result = PyObject_CallObject(ref->callable, args);
    if (result == nullptr) {
        report_callback_error(ref->script, context);
        return;
    }
    Py_DECREF(result);
}

} // namespace

void SPIRATION_CALL event_trampoline(void* user_data, spiration_str_view event,
                                     spiration_str_view data) {
    listener_ref* ref = static_cast<listener_ref*>(user_data);
    if (ref == nullptr || ref->script == nullptr || ref->callable == nullptr) return;

    gil_guard gil;
    PyObject* event_text = text_object(event);
    PyObject* data_text = text_object(data);
    if (event_text == nullptr || data_text == nullptr) {
        Py_XDECREF(event_text);
        Py_XDECREF(data_text);
        PyErr_Clear();
        return;
    }
    PyObject* args = PyTuple_Pack(2, event_text, data_text);
    Py_DECREF(event_text);
    Py_DECREF(data_text);
    if (args == nullptr) {
        PyErr_Clear();
        return;
    }
    invoke_listener(ref, args, "event");
    Py_DECREF(args);
}

void SPIRATION_CALL menu_trampoline(void* user_data) {
    listener_ref* ref = static_cast<listener_ref*>(user_data);
    if (ref == nullptr || ref->script == nullptr || ref->callable == nullptr) return;

    gil_guard gil;
    invoke_listener(ref, nullptr, "menu");
}

void SPIRATION_CALL widget_trampoline(void* user_data,
                                      spiration_widget* widget,
                                      spiration_str_view event,
                                      spiration_value data) {
    listener_ref* ref = static_cast<listener_ref*>(user_data);
    if (ref == nullptr || ref->script == nullptr || ref->callable == nullptr) return;

    gil_guard gil;
    PyObject* widget_value = PyLong_FromLongLong(
        handle_value(reinterpret_cast<const void*>(widget)));
    PyObject* event_text = text_object(event);
    PyObject* data_value = value_to_python(data);
    if (widget_value == nullptr || event_text == nullptr || data_value == nullptr) {
        Py_XDECREF(widget_value);
        Py_XDECREF(event_text);
        Py_XDECREF(data_value);
        PyErr_Clear();
        return;
    }
    PyObject* args = PyTuple_Pack(3, widget_value, event_text, data_value);
    Py_DECREF(widget_value);
    Py_DECREF(event_text);
    Py_DECREF(data_value);
    if (args == nullptr) {
        PyErr_Clear();
        return;
    }
    invoke_listener(ref, args, "widget");
    Py_DECREF(args);
}

void SPIRATION_CALL dispatch_trampoline(void* user_data) {
    dispatch_task* task = static_cast<dispatch_task*>(user_data);
    if (task == nullptr || task->script == nullptr) return;

    gil_guard gil;
    task->script->pending_tasks.erase(task);
    if (task->callable != nullptr) {
        PyObject* result = PyObject_CallObject(task->callable, nullptr);
        if (result == nullptr) {
            report_callback_error(task->script, "dispatch");
        } else {
            Py_DECREF(result);
        }
        Py_DECREF(task->callable);
    }
    delete task;
}

PyObject* build_injected_module() {
    PyObject* module = PyModule_New(module_name());
    if (module == nullptr) return nullptr;

    for (PyMethodDef* def = k_module_methods; def->ml_name != nullptr; ++def) {
        PyObject* function = PyCFunction_NewEx(def, NULL, NULL);
        if (function == nullptr) {
            Py_DECREF(module);
            return nullptr;
        }
        PyModule_AddObject(module, def->ml_name, function);
    }

    /* spiration.events：仅承载事件相关的三个入口。 */
    PyObject* events = PyModule_New("spiration.events");
    if (events != nullptr) {
        bind_into(k_module_methods, "on_event", events);
        bind_into(k_module_methods, "off_event", events);
        bind_into(k_module_methods, "emit", events);
        PyModule_AddObject(module, "events", events);
    }

    /* spiration.ui：可选界面接口，宿主可能不支持。 */
    PyObject* ui_module = PyModule_New("spiration.ui");
    if (ui_module != nullptr) {
        for (PyMethodDef* def = k_ui_methods; def->ml_name != nullptr; ++def) {
            PyObject* function = PyCFunction_NewEx(def, NULL, NULL);
            if (function == nullptr) continue;
            PyModule_AddObject(ui_module, def->ml_name, function);
        }
        PyModule_AddIntConstant(ui_module, "WIDGET_CONTAINER", SPIRATION_WIDGET_CONTAINER);
        PyModule_AddIntConstant(ui_module, "WIDGET_LABEL", SPIRATION_WIDGET_LABEL);
        PyModule_AddIntConstant(ui_module, "WIDGET_BUTTON", SPIRATION_WIDGET_BUTTON);
        PyModule_AddIntConstant(ui_module, "WIDGET_CHECKBOX", SPIRATION_WIDGET_CHECKBOX);
        PyModule_AddIntConstant(ui_module, "WIDGET_RADIO_BUTTON",
                                SPIRATION_WIDGET_RADIO_BUTTON);
        PyModule_AddIntConstant(ui_module, "WIDGET_TOGGLE_SWITCH",
                                SPIRATION_WIDGET_TOGGLE_SWITCH);
        PyModule_AddIntConstant(ui_module, "WIDGET_SLIDER", SPIRATION_WIDGET_SLIDER);
        PyModule_AddIntConstant(ui_module, "WIDGET_PROGRESS_BAR",
                                SPIRATION_WIDGET_PROGRESS_BAR);
        PyModule_AddIntConstant(ui_module, "WIDGET_COMBO_BOX", SPIRATION_WIDGET_COMBO_BOX);
        PyModule_AddIntConstant(ui_module, "WIDGET_LIST_VIEW", SPIRATION_WIDGET_LIST_VIEW);
        PyModule_AddIntConstant(ui_module, "WIDGET_TEXT_FIELD",
                                SPIRATION_WIDGET_TEXT_FIELD);
        PyModule_AddIntConstant(ui_module, "WIDGET_TEXT_AREA", SPIRATION_WIDGET_TEXT_AREA);
        PyModule_AddIntConstant(ui_module, "WIDGET_SCROLL_ROW",
                                SPIRATION_WIDGET_SCROLL_ROW);
        PyModule_AddIntConstant(ui_module, "WIDGET_SPLIT_PANE",
                                SPIRATION_WIDGET_SPLIT_PANE);
        PyModule_AddIntConstant(ui_module, "WIDGET_COLLAPSIBLE",
                                SPIRATION_WIDGET_COLLAPSIBLE);
        PyModule_AddIntConstant(ui_module, "WIDGET_SEPARATOR", SPIRATION_WIDGET_SEPARATOR);
        PyModule_AddIntConstant(ui_module, "WIDGET_SPACER", SPIRATION_WIDGET_SPACER);
        PyModule_AddIntConstant(ui_module, "WIDGET_MARKDOWN", SPIRATION_WIDGET_MARKDOWN);

        PyModule_AddObject(module, "ui", ui_module);
    }

    /* 日志等级常量，便于脚本写 spiration.log(spiration.LOG_INFO, ...)。 */
    PyModule_AddIntConstant(module, "LOG_TRACE", SPIRATION_LOG_TRACE);
    PyModule_AddIntConstant(module, "LOG_DEBUG", SPIRATION_LOG_DEBUG);
    PyModule_AddIntConstant(module, "LOG_INFO", SPIRATION_LOG_INFO);
    PyModule_AddIntConstant(module, "LOG_WARNING", SPIRATION_LOG_WARNING);
    PyModule_AddIntConstant(module, "LOG_ERROR", SPIRATION_LOG_ERROR);

    return module;
}

} // namespace loader
} // namespace python
} // namespace spiration
