/**
 * @file agent.cpp
 * @brief 加载器侧智能体绑定：把 Python provider 对象适配为宿主 `spiration.agent` vtable。
 * @author 陈林锴
 *
 * 注入模块 `spiration.agent` 暴露三个入口：
 *
 * | Python | 宿主能力 |
 * |---|---|
 * | `supported()` | `host->query_interface("spiration.agent")` 是否可用 |
 * | `register_provider(name, provider)` | 注册一句柄 |
 * | `unregister_provider(name)` | 注销并释放 Python 引用 |
 *
 * provider 对象需提供 `execute(request, stream)`；可选 `reasoning_levels()`
 * 在本模块注册时调用一次，结果缓存为宿主挡位数组。
 *
 * 线程与 GIL 约定：
 * - `execute` 由宿主工作线程调用：进入时用 `gil_guard` 获取 GIL，并以
 *   `script_scope` 设定当前脚本，使 provider 内的 `spiration.*` 调用落到正确脚本；
 * - 注销时先在调用宿主前释放 GIL（`Py_BEGIN_ALLOW_THREADS`）：宿主会等待正在
 *   执行的 `execute` 结束，而它回来时需要 GIL，若不放行会死锁。
 */

#include "internal.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace spiration {
namespace python {
namespace loader {

/**
 * @brief 单个 provider 的绑定。
 * @note 由 `script_state::agent_providers` 持有，注销时释放。
 */
struct agent_provider_binding {
    /** 所属脚本。 */
    script_state* script = nullptr;
    /** 取用时的宿主接口。 */
    const spiration_agent_api* api = nullptr;
    /** provider 名称。 */
    std::string name;
    /** Python provider 对象（持有引用）。 */
    PyObject* provider = nullptr;
    /** Python `execute` 绑定方法（持有引用）。 */
    PyObject* execute = nullptr;
    /** 交给宿主的函数表。 */
    spiration_agent_provider vtable;
    /** 缓存的思考挡位。 */
    std::vector<spiration_reasoning_level> levels;
    /** 最近一次响应 JSON，`execute` 返回视图借用其存储。 */
    std::string response_text;
};

namespace {

/** @brief 流回调对象的胶囊名。 */
const char* kStreamCapsuleName = "spiration.agent.stream";
/** @brief 胶囊内部状态。 */
struct stream_context {
    /** 宿主提供的回调集合；`execute` 返回后置空。 */
    const spiration_agent_stream* stream = nullptr;
};
/** @brief 挡位名称表，下标与 `spiration_reasoning_level` 一致。 */
const char* const kLevelNames[] = {"none",  "low",  "medium",
                                   "high",  "xhigh", "max"};
/** @brief 挡位数量。 */
const size_t kLevelCount = sizeof(kLevelNames) / sizeof(kLevelNames[0]);

/* -------------------------------------------------------------------------
 * JSON 与值转换
 * ---------------------------------------------------------------------- */

/** @brief `python_to_value` 的补充：`spiration_value` 之外的 JSON 文本解析。 */
PyObject* json_loads(spiration_str_view text) {
    PyObject* json = PyImport_ImportModule("json");
    if (json == nullptr) return nullptr;
    const char* data = text.data != nullptr ? text.data : "";
    PyObject* result = PyObject_CallMethod(
        json, "loads", "s#", data, static_cast<Py_ssize_t>(text.length));
    Py_DECREF(json);
    return result;
}

/** @brief Python 对象 → JSON 文本（新引用 `str`）。 */
PyObject* json_dumps(PyObject* object) {
    PyObject* json = PyImport_ImportModule("json");
    if (json == nullptr) return nullptr;
    PyObject* result = PyObject_CallMethod(json, "dumps", "O", object);
    Py_DECREF(json);
    return result;
}

/** @brief 挡位名称 → 枚举；未知名称返回 `false`。 */
bool level_from_name(const std::string& name, spiration_reasoning_level* out) {
    for (size_t i = 0; i < kLevelCount; ++i) {
        if (name == kLevelNames[i]) {
            *out = static_cast<spiration_reasoning_level>(i);
            return true;
        }
    }
    return false;
}

/** @brief 默认挡位：全部。 */
std::vector<spiration_reasoning_level> all_levels() {
    std::vector<spiration_reasoning_level> levels;
    levels.reserve(kLevelCount);
    for (size_t i = 0; i < kLevelCount; ++i) {
        levels.push_back(static_cast<spiration_reasoning_level>(i));
    }
    return levels;
}

/**
 * @brief 读取 provider 声明的思考挡位。
 * @param provider Python provider 对象。
 * @param levels   输出挡位；未声明 `reasoning_levels()` 时回退为全部。
 * @return 成功返回 `true`；失败返回 `false` 并已设置 Python 异常。
 */
bool read_reasoning_levels(PyObject* provider,
                           std::vector<spiration_reasoning_level>* levels) {
    *levels = all_levels();

    PyObject* method = PyObject_GetAttrString(provider, "reasoning_levels");
    if (method == nullptr) {
        /* 未声明是可选的；清掉 AttributeError 即可。 */
        PyErr_Clear();
        return true;
    }
    if (!PyCallable_Check(method)) {
        Py_DECREF(method);
        PyErr_SetString(PyExc_TypeError,
                        "provider.reasoning_levels must be callable");
        return false;
    }

    PyObject* declared = PyObject_CallObject(method, nullptr);
    Py_DECREF(method);
    if (declared == nullptr) return false;

    PyObject* sequence =
        PySequence_Fast(declared, "provider.reasoning_levels() must return a sequence");
    Py_DECREF(declared);
    if (sequence == nullptr) return false;

    std::vector<spiration_reasoning_level> parsed;
    const Py_ssize_t count = PySequence_Fast_GET_SIZE(sequence);
    for (Py_ssize_t i = 0; i < count; ++i) {
        PyObject* item = PySequence_Fast_GET_ITEM(sequence, i); /* 借用引用 */
        const char* text = item != nullptr ? PyUnicode_AsUTF8(item) : nullptr;
        if (text == nullptr) {
            PyErr_Clear();
            Py_DECREF(sequence);
            PyErr_SetString(PyExc_TypeError,
                            "provider.reasoning_levels() must contain strings");
            return false;
        }
        spiration_reasoning_level level = SPIRATION_REASONING_MEDIUM;
        if (!level_from_name(text, &level)) {
            const std::string unknown = text;
            Py_DECREF(sequence);
            PyErr_Format(PyExc_ValueError, "unknown reasoning level: %s",
                         unknown.c_str());
            return false;
        }
        parsed.push_back(level);
    }
    Py_DECREF(sequence);

    if (parsed.empty()) {
        PyErr_SetString(PyExc_ValueError,
                        "provider.reasoning_levels() must not be empty");
        return false;
    }
    levels->swap(parsed);
    return true;
}

/* -------------------------------------------------------------------------
 * 流式回调对象
 * ---------------------------------------------------------------------- */

/**
 * @brief 取胶囊内的流上下文。
 * @return 成功返回上下文；失败返回 `nullptr` 并已设置 Python 异常。
 */
stream_context* stream_of(PyObject* capsule) {
    if (capsule == nullptr || !PyCapsule_CheckExact(capsule)) {
        PyErr_SetString(PyExc_TypeError, "agent stream handle is invalid");
        return nullptr;
    }
    return static_cast<stream_context*>(
        PyCapsule_GetPointer(capsule, kStreamCapsuleName));
}

/** @brief 胶囊析构：回收流上下文。 */
void destroy_stream_context(PyObject* capsule) {
    stream_context* ctx = static_cast<stream_context*>(
        PyCapsule_GetPointer(capsule, kStreamCapsuleName));
    if (ctx != nullptr) delete ctx;
    PyErr_Clear();
}

/** @brief `stream.delta(text)`：上报正文增量。 */
PyObject* stream_delta(PyObject* self, PyObject* args) {
    stream_context* ctx = stream_of(self);
    if (ctx == nullptr) return nullptr;

    PyObject* text = nullptr;
    if (!PyArg_ParseTuple(args, "U:delta", &text)) return nullptr;

    Py_ssize_t size = 0;
    const char* utf8 = PyUnicode_AsUTF8AndSize(text, &size);
    if (utf8 == nullptr) return nullptr;

    if (ctx->stream != nullptr && ctx->stream->on_delta != nullptr) {
        ctx->stream->on_delta(ctx->stream->user_data,
                              spiration_str_view_of(utf8, static_cast<size_t>(size)));
    }
    Py_RETURN_NONE;
}

/** @brief `stream.reasoning_delta(text)`：上报思考过程增量。 */
PyObject* stream_reasoning_delta(PyObject* self, PyObject* args) {
    stream_context* ctx = stream_of(self);
    if (ctx == nullptr) return nullptr;

    PyObject* text = nullptr;
    if (!PyArg_ParseTuple(args, "U:reasoning_delta", &text)) return nullptr;

    Py_ssize_t size = 0;
    const char* utf8 = PyUnicode_AsUTF8AndSize(text, &size);
    if (utf8 == nullptr) return nullptr;

    if (ctx->stream != nullptr && ctx->stream->on_reasoning_delta != nullptr) {
        ctx->stream->on_reasoning_delta(
            ctx->stream->user_data,
            spiration_str_view_of(utf8, static_cast<size_t>(size)));
    }
    Py_RETURN_NONE;
}

/** @brief `stream.should_stop()`：查询用户是否已请求中止。 */
PyObject* stream_should_stop(PyObject* self, PyObject* args) {
    SPIRATION_UNUSED(args);
    stream_context* ctx = stream_of(self);
    if (ctx == nullptr) return nullptr;

    if (ctx->stream != nullptr && ctx->stream->should_stop != nullptr) {
        return PyBool_FromLong(
            ctx->stream->should_stop(ctx->stream->user_data) == SPIRATION_TRUE
                ? 1
                : 0);
    }
    Py_RETURN_FALSE;
}

/** @brief `delta` 方法定义。 */
PyMethodDef kStreamDeltaDef = {"delta", stream_delta, METH_VARARGS,
                               "delta(text) -> None\n\n上报正文增量。"};
/** @brief `reasoning_delta` 方法定义。 */
PyMethodDef kStreamReasoningDef = {
    "reasoning_delta", stream_reasoning_delta, METH_VARARGS,
    "reasoning_delta(text) -> None\n\n上报思考过程增量。"};
/** @brief `should_stop` 方法定义。 */
PyMethodDef kStreamStopDef = {"should_stop", stream_should_stop, METH_NOARGS,
                              "should_stop() -> bool\n\n用户是否已请求中止。"};

/**
 * @brief 构造传给 provider 的 `stream` 对象。
 * @note 该对象只在一次 `execute` 期间有效；`binding_execute` 返回前会把
 *       上下文中的回调指针置空，因此即使 provider 保留引用也不会回调进宿主。
 */
PyObject* build_stream_object(stream_context* ctx) {
    PyObject* capsule = PyCapsule_New(ctx, kStreamCapsuleName,
                                      destroy_stream_context);
    if (capsule == nullptr) return nullptr;

    PyObject* types = PyImport_ImportModule("types");
    if (types == nullptr) {
        Py_DECREF(capsule);
        return nullptr;
    }
    PyObject* namespace_type = PyObject_GetAttrString(types, "SimpleNamespace");
    Py_DECREF(types);
    if (namespace_type == nullptr) {
        Py_DECREF(capsule);
        return nullptr;
    }
    PyObject* object = PyObject_CallObject(namespace_type, nullptr);
    Py_DECREF(namespace_type);
    if (object == nullptr) {
        Py_DECREF(capsule);
        return nullptr;
    }

    struct binding {
        PyMethodDef* def;
        const char* name;
    } methods[] = {{&kStreamDeltaDef, "delta"},
                   {&kStreamReasoningDef, "reasoning_delta"},
                   {&kStreamStopDef, "should_stop"}};

    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        PyObject* function = PyCFunction_NewEx(methods[i].def, capsule, nullptr);
        if (function == nullptr) {
            Py_DECREF(capsule);
            Py_DECREF(object);
            return nullptr;
        }
        const int status =
            PyObject_SetAttrString(object, methods[i].name, function);
        Py_DECREF(function);
        if (status != 0) {
            Py_DECREF(capsule);
            Py_DECREF(object);
            return nullptr;
        }
    }

    /* 方法对象已各自持有胶囊引用，这里释放局部引用即可。 */
    Py_DECREF(capsule);
    return object;
}

/* -------------------------------------------------------------------------
 * provider vtable 条目
 * ---------------------------------------------------------------------- */

/** @brief 查询支持的思考挡位（缓存值，无需 GIL）。 */
size_t SPIRATION_CALL binding_levels(void* user_data,
                                     spiration_reasoning_level* levels,
                                     size_t capacity) {
    agent_provider_binding* binding =
        static_cast<agent_provider_binding*>(user_data);
    if (binding == nullptr) return 0u;

    if (levels != nullptr && capacity > 0u) {
        const size_t count = std::min(capacity, binding->levels.size());
        for (size_t i = 0; i < count; ++i) levels[i] = binding->levels[i];
    }
    return binding->levels.size();
}

/** @brief 执行一次补全：请求 JSON → Python dict，响应 dict → JSON。 */
spiration_status SPIRATION_CALL binding_execute(
    void* user_data, spiration_str_view request,
    const spiration_agent_stream* stream, spiration_str_view* response) {
    agent_provider_binding* binding =
        static_cast<agent_provider_binding*>(user_data);
    if (binding == nullptr || binding->execute == nullptr ||
        response == nullptr) {
        return SPIRATION_ERR_INVALID_ARGUMENT;
    }
    response->data = nullptr;
    response->length = 0u;

    gil_guard gil;
    script_scope scope(binding->script);

    PyObject* py_request = json_loads(request);
    if (py_request == nullptr) {
        report_python_error(binding->script, "agent.execute/request");
        return SPIRATION_ERR_INTERNAL;
    }

    stream_context* ctx = new stream_context();
    ctx->stream = stream;
    PyObject* py_stream = build_stream_object(ctx);
    if (py_stream == nullptr) {
        delete ctx;
        Py_DECREF(py_request);
        report_python_error(binding->script, "agent.execute/stream");
        return SPIRATION_ERR_INTERNAL;
    }

    PyObject* result =
        PyObject_CallFunctionObjArgs(binding->execute, py_request, py_stream,
                                     nullptr);
    /* 本次调用的回调窗口到此结束：此后即使 provider 保留 stream 引用也不会
       回调进宿主（其上下文中的指针已置空）。 */
    ctx->stream = nullptr;
    Py_DECREF(py_request);
    Py_DECREF(py_stream);
    if (result == nullptr) {
        report_python_error(binding->script, "agent.execute");
        return SPIRATION_ERR_INTERNAL;
    }

    PyObject* dumped = json_dumps(result);
    Py_DECREF(result);
    if (dumped == nullptr) {
        report_python_error(binding->script, "agent.execute/response");
        return SPIRATION_ERR_INTERNAL;
    }

    Py_ssize_t length = 0;
    const char* utf8 = PyUnicode_AsUTF8AndSize(dumped, &length);
    if (utf8 == nullptr) {
        Py_DECREF(dumped);
        PyErr_Clear();
        return SPIRATION_ERR_INTERNAL;
    }
    binding->response_text.assign(utf8, static_cast<size_t>(length));
    Py_DECREF(dumped);

    *response = to_view(binding->response_text);
    return SPIRATION_OK;
}

/* -------------------------------------------------------------------------
 * 注册 / 注销
 * ---------------------------------------------------------------------- */

/** @brief 释放绑定及其持有的 Python 引用（需持有 GIL）。 */
void destroy_binding(agent_provider_binding* binding) {
    if (binding == nullptr) return;
    Py_CLEAR(binding->execute);
    Py_CLEAR(binding->provider);
    delete binding;
}

} // namespace

const spiration_agent_api* agent_api(script_state* script) {
    if (script == nullptr || script->desc.host == nullptr ||
        script->desc.host->query_interface == nullptr) {
        return nullptr;
    }
    const void* raw = script->desc.host->query_interface(
        &script->desc, SPIRATION_STR(SPIRATION_AGENT_INTERFACE_NAME),
        SPIRATION_AGENT_VERSION);
    return static_cast<const spiration_agent_api*>(raw);
}

spiration_status register_python_provider(script_state* script,
                                          const std::string& name,
                                          PyObject* provider) {
    if (script == nullptr || name.empty() || provider == nullptr) {
        return SPIRATION_ERR_INVALID_ARGUMENT;
    }

    const spiration_agent_api* api = agent_api(script);
    if (api == nullptr || api->register_provider == nullptr) {
        return SPIRATION_ERR_UNSUPPORTED;
    }
    if (script->agent_providers.find(name) != script->agent_providers.end()) {
        return SPIRATION_ERR_ALREADY_EXISTS;
    }

    PyObject* execute = PyObject_GetAttrString(provider, "execute");
    if (execute == nullptr || !PyCallable_Check(execute)) {
        Py_XDECREF(execute);
        PyErr_Format(PyExc_TypeError,
                     "agent provider \"%s\" must provide callable execute()",
                     name.c_str());
        return SPIRATION_ERR_INVALID_ARGUMENT;
    }

    std::vector<spiration_reasoning_level> levels;
    if (!read_reasoning_levels(provider, &levels)) {
        Py_DECREF(execute);
        return SPIRATION_ERR_INVALID_ARGUMENT;
    }

    agent_provider_binding* binding = new agent_provider_binding();
    binding->script = script;
    binding->api = api;
    binding->name = name;
    binding->provider = provider;
    Py_INCREF(provider);
    binding->execute = execute; /* GetAttr 的引用直接接管 */
    binding->levels.swap(levels);
    std::memset(&binding->vtable, 0, sizeof(binding->vtable));
    binding->vtable.version = SPIRATION_STRUCT_VERSION;
    binding->vtable.size = static_cast<uint32_t>(sizeof(binding->vtable));
    binding->vtable.supported_reasoning_levels = &binding_levels;
    binding->vtable.execute = &binding_execute;

    const spiration_status status = api->register_provider(
        &script->desc, to_view(name), &binding->vtable, binding);
    if (status != SPIRATION_OK) {
        destroy_binding(binding);
        return status;
    }

    script->agent_providers[name] = binding;
    return SPIRATION_OK;
}

spiration_status unregister_python_provider(script_state* script,
                                            const std::string& name) {
    if (script == nullptr) return SPIRATION_ERR_INVALID_ARGUMENT;

    std::map<std::string, agent_provider_binding*>::iterator it =
        script->agent_providers.find(name);
    if (it == script->agent_providers.end()) return SPIRATION_OK;

    agent_provider_binding* binding = it->second;
    script->agent_providers.erase(it);

    const spiration_agent_api* api = binding->api;
    spiration_status status = SPIRATION_OK;
    if (api != nullptr && api->unregister_provider != nullptr) {
        /* 宿主可能等待进行中的 execute 结束，而它回来时需要 GIL：
           因此这段调用必须放开 GIL，否则会互相等待。 */
        Py_BEGIN_ALLOW_THREADS
        status = api->unregister_provider(&script->desc, to_view(name));
        Py_END_ALLOW_THREADS
    }

    destroy_binding(binding);
    return status;
}

void release_agent_providers(script_state* script) {
    if (script == nullptr) return;

    std::vector<std::string> names;
    names.reserve(script->agent_providers.size());
    for (std::map<std::string, agent_provider_binding*>::const_iterator it =
             script->agent_providers.begin();
         it != script->agent_providers.end(); ++it) {
        names.push_back(it->first);
    }
    for (size_t i = 0; i < names.size(); ++i) {
        unregister_python_provider(script, names[i]);
    }
}

} // namespace loader
} // namespace python
} // namespace spiration
