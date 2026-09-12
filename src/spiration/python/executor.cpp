#include <pybind11/embed.h>
#include <pybind11/pybind11.h>
#include <spiration/python/executor.h>

extern "C" {

int run(const char* name) {
    pybind11::scoped_interpreter guard{};
    try {
        pybind11::module_ sys = pybind11::module_::import("sys");
        pybind11::list sys_path = sys.attr("path");
        // sys_path.attr("append")("C:/path/to/scripts");
        
        pybind11::object module = pybind11::module_::import(name);
        if (pybind11::hasattr(module, "main")) {
            pybind11::object main_func = module.attr("main");
            if (pybind11::isinstance<pybind11::function>(main_func)) {
                main_func();
                return 0;
            } else {
                return 1;
            }
        } else {
            return 1;
        }
    } catch (const pybind11::error_already_set& e) {
        return 1;
    } catch (const std::exception& e) {
        return 1;
    }
    return 0;
}

}
